/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file GasMeter.cpp
 * @author Christian <c@ethdev.com>
 * @date 2015
 */

#include "GasMeter.h"
#include <libevmasm/KnownState.h>
using namespace std;
using namespace dev;
using namespace dev::eth;

GasMeter::GasConsumption& GasMeter::GasConsumption::operator+=(GasConsumption const& _other)
{
	if (_other.isInfinite && !isInfinite)
		*this = infinite();
	if (isInfinite)
		return *this;
	bigint v = bigint(value) + _other.value;
	if (v > numeric_limits<u256>::max())
		*this = infinite();
	else
		value = u256(v);
	return *this;
}

GasMeter::GasConsumption GasMeter::estimateMax(AssemblyItem const& _item)
{
	GasConsumption gas;
	switch (_item.type())
	{
	case Push:
	case PushTag:
	case PushData:
	case PushString:
	case PushSub:
	case PushSubSize:
	case PushProgramSize:
	case PushLibraryAddress:
		gas = runGas(Instruction::PUSH1);
		break;
	case Tag:
		gas = runGas(Instruction::JUMPDEST);
		break;
	case Operation:
	{
		ExpressionClasses& classes = m_state->expressionClasses();
		gas = runGas(_item.instruction());
		switch (_item.instruction())
		{
		case Instruction::SSTORE:
		{
			ExpressionClasses::Id slot = m_state->relativeStackElement(0);
			ExpressionClasses::Id value = m_state->relativeStackElement(-1);
			if (classes.knownZero(value) || (
				m_state->storageContent().count(slot) &&
				classes.knownNonZero(m_state->storageContent().at(slot))
			))
				gas += m_schedule.sstoreResetGas; //@todo take refunds into account
			else
				gas += m_schedule.sstoreSetGas;
			break;
		}
		case Instruction::SLOAD:
			gas += m_schedule.sloadGas;
			break;
		case Instruction::RETURN:
			gas += memoryGas(0, -1);
			break;
		case Instruction::MLOAD:
		case Instruction::MSTORE:
			gas += memoryGas(classes.find(solidity::Instruction::ADD, {
				m_state->relativeStackElement(0),
				classes.find(AssemblyItem(32))
			}));
			break;
		case Instruction::MSTORE8:
			gas += memoryGas(classes.find(solidity::Instruction::ADD, {
				m_state->relativeStackElement(0),
				classes.find(AssemblyItem(1))
			}));
			break;
		case Instruction::SHA3:
			gas = m_schedule.sha3Gas;
			gas += wordGas(m_schedule.sha3WordGas, m_state->relativeStackElement(-1));
			gas += memoryGas(0, -1);
			break;
		case Instruction::CALLDATACOPY:
		case Instruction::CODECOPY:
			gas += memoryGas(0, -2);
			gas += wordGas(m_schedule.copyGas, m_state->relativeStackElement(-2));
			break;
		case Instruction::EXTCODECOPY:
			gas += memoryGas(-1, -3);
			gas += wordGas(m_schedule.copyGas, m_state->relativeStackElement(-3));
			break;
		case Instruction::LOG0:
		case Instruction::LOG1:
		case Instruction::LOG2:
		case Instruction::LOG3:
		case Instruction::LOG4:
		{
			unsigned n = unsigned(_item.instruction()) - unsigned(Instruction::LOG0);
			gas = m_schedule.logGas + m_schedule.logTopicGas * n;
			gas += memoryGas(0, -1);
			if (u256 const* value = classes.knownConstant(m_state->relativeStackElement(-1)))
				gas += m_schedule.logDataGas * (*value);
			else
				gas = GasConsumption::infinite();
			break;
		}
		case Instruction::CALL:
		case Instruction::CALLCODE:
		case Instruction::DELEGATECALL:
		{
			gas = m_schedule.callGas;
			if (u256 const* value = classes.knownConstant(m_state->relativeStackElement(0)))
				gas += (*value);
			else
				gas = GasConsumption::infinite();
			if (_item.instruction() == Instruction::CALL)
				gas += m_schedule.callNewAccountGas; // We very rarely know whether the address exists.
			int valueSize = _item.instruction() == Instruction::DELEGATECALL ? 0 : 1;
			if (!classes.knownZero(m_state->relativeStackElement(-1 - valueSize)))
				gas += m_schedule.callValueTransferGas;
			gas += memoryGas(-2 - valueSize, -3 - valueSize);
			gas += memoryGas(-4 - valueSize, -5 - valueSize);
			break;
		}
		case Instruction::CREATE:
			gas = m_schedule.createGas;
			gas += memoryGas(-1, -2);
			break;
		case Instruction::EXP:
			gas = m_schedule.expGas;
			if (u256 const* value = classes.knownConstant(m_state->relativeStackElement(-1)))
				gas += m_schedule.expByteGas * (32 - (h256(*value).firstBitSet() / 8));
			else
				gas += m_schedule.expByteGas * 32;
			break;
		default:
			break;
		}
		break;
	}
	default:
		gas = GasConsumption::infinite();
		break;
	}

	m_state->feedItem(_item);
	return gas;
}

GasMeter::GasConsumption GasMeter::wordGas(u256 const& _multiplier, ExpressionClasses::Id _position)
{
	u256 const* value = m_state->expressionClasses().knownConstant(_position);
	if (!value)
		return GasConsumption::infinite();
	return GasConsumption(_multiplier * ((*value + 31) / 32));
}

GasMeter::GasConsumption GasMeter::memoryGas(ExpressionClasses::Id _position)
{
	u256 const* value = m_state->expressionClasses().knownConstant(_position);
	if (!value)
		return GasConsumption::infinite();
	if (*value < m_largestMemoryAccess)
		return GasConsumption(u256(0));
	u256 previous = m_largestMemoryAccess;
	m_largestMemoryAccess = *value;
	auto memGas = [=](u256 const& pos) -> u256
	{
		u256 size = (pos + 31) / 32;
		return m_schedule.memoryGas * size + size * size / m_schedule.quadCoeffDiv;
	};
	return memGas(*value) - memGas(previous);
}

GasMeter::GasConsumption GasMeter::memoryGas(int _stackPosOffset, int _stackPosSize)
{
	ExpressionClasses& classes = m_state->expressionClasses();
	if (classes.knownZero(m_state->relativeStackElement(_stackPosSize)))
		return GasConsumption(0);
	else
		return memoryGas(classes.find(solidity::Instruction::ADD, {
			m_state->relativeStackElement(_stackPosOffset),
			m_state->relativeStackElement(_stackPosSize)
		}));
}

u256 GasMeter::runGas(Instruction _instruction, EVMSchedule const& _es)
{
	if (_instruction == Instruction::JUMPDEST)
		return 1;

	int tier = instructionInfo(_instruction).gasPriceTier;
	assertThrow(tier != InvalidTier, OptimizerException, "Invalid gas tier.");
	return _es.tierStepGas[tier];
}


