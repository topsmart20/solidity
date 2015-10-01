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
/**
 * @author Christian <c@ethdev.com>
 * @date 2015
 * Type analyzer and checker.
 */

#include <libsolidity/TypeChecker.h>
#include <memory>
#include <boost/range/adaptor/reversed.hpp>
#include <libsolidity/AST.h>

using namespace std;
using namespace dev;
using namespace dev::solidity;


bool TypeChecker::checkTypeRequirements(const ContractDefinition& _contract)
{
	try
	{
		visit(_contract);
	}
	catch (FatalError const&)
	{
		// We got a fatal error which required to stop further type checking, but we can
		// continue normally from here.
		if (m_errors.empty())
			throw; // Something is weird here, rather throw again.
	}
	bool success = true;
	for (auto const& it: m_errors)
		if (!dynamic_cast<Warning const*>(it.get()))
		{
			success = false;
			break;
		}
	return success;
}

TypePointer const& TypeChecker::type(Expression const& _expression) const
{
	solAssert(!!_expression.annotation().type, "Type requested but not present.");
	return _expression.annotation().type;
}

TypePointer const& TypeChecker::type(VariableDeclaration const& _variable) const
{
	solAssert(!!_variable.annotation().type, "Type requested but not present.");
	return _variable.annotation().type;
}

bool TypeChecker::visit(ContractDefinition const& _contract)
{
	// We force our own visiting order here.
	ASTNode::listAccept(_contract.definedStructs(), *this);
	ASTNode::listAccept(_contract.baseContracts(), *this);

	checkContractDuplicateFunctions(_contract);
	checkContractIllegalOverrides(_contract);
	checkContractAbstractFunctions(_contract);
	checkContractAbstractConstructors(_contract);

	FunctionDefinition const* function = _contract.constructor();
	if (function && !function->returnParameters().empty())
		typeError(*function->returnParameterList(), "Non-empty \"returns\" directive for constructor.");

	FunctionDefinition const* fallbackFunction = nullptr;
	for (ASTPointer<FunctionDefinition> const& function: _contract.definedFunctions())
	{
		if (function->name().empty())
		{
			if (fallbackFunction)
			{
				auto err = make_shared<DeclarationError>();
				*err << errinfo_comment("Only one fallback function is allowed.");
				m_errors.push_back(err);
			}
			else
			{
				fallbackFunction = function.get();
				if (!fallbackFunction->parameters().empty())
					typeError(fallbackFunction->parameterList(), "Fallback function cannot take parameters.");
			}
		}
		if (!function->isImplemented())
			_contract.annotation().isFullyImplemented = false;
	}

	ASTNode::listAccept(_contract.stateVariables(), *this);
	ASTNode::listAccept(_contract.events(), *this);
	ASTNode::listAccept(_contract.functionModifiers(), *this);
	ASTNode::listAccept(_contract.definedFunctions(), *this);

	checkContractExternalTypeClashes(_contract);
	// check for hash collisions in function signatures
	set<FixedHash<4>> hashes;
	for (auto const& it: _contract.interfaceFunctionList())
	{
		FixedHash<4> const& hash = it.first;
		if (hashes.count(hash))
			typeError(
				_contract,
				string("Function signature hash collision for ") + it.second->externalSignature()
			);
		hashes.insert(hash);
	}

	if (_contract.isLibrary())
		checkLibraryRequirements(_contract);

	return false;
}

void TypeChecker::checkContractDuplicateFunctions(ContractDefinition const& _contract)
{
	/// Checks that two functions with the same name defined in this contract have different
	/// argument types and that there is at most one constructor.
	map<string, vector<FunctionDefinition const*>> functions;
	for (ASTPointer<FunctionDefinition> const& function: _contract.definedFunctions())
		functions[function->name()].push_back(function.get());

	// Constructor
	if (functions[_contract.name()].size() > 1)
	{
		SecondarySourceLocation ssl;
		auto it = ++functions[_contract.name()].begin();
		for (; it != functions[_contract.name()].end(); ++it)
			ssl.append("Another declaration is here:", (*it)->location());

		auto err = make_shared<DeclarationError>();
		*err <<
			errinfo_sourceLocation(functions[_contract.name()].front()->location()) <<
			errinfo_comment("More than one constructor defined.") <<
			errinfo_secondarySourceLocation(ssl);
		m_errors.push_back(err);
	}
	for (auto const& it: functions)
	{
		vector<FunctionDefinition const*> const& overloads = it.second;
		for (size_t i = 0; i < overloads.size(); ++i)
			for (size_t j = i + 1; j < overloads.size(); ++j)
				if (FunctionType(*overloads[i]).hasEqualArgumentTypes(FunctionType(*overloads[j])))
				{
					auto err = make_shared<DeclarationError>();
					*err <<
						errinfo_sourceLocation(overloads[j]->location()) <<
						errinfo_comment("Function with same name and arguments defined twice.") <<
						errinfo_secondarySourceLocation(SecondarySourceLocation().append(
							"Other declaration is here:", overloads[i]->location()));
					m_errors.push_back(err);
				}
	}
}

void TypeChecker::checkContractAbstractFunctions(ContractDefinition const& _contract)
{
	// Mapping from name to function definition (exactly one per argument type equality class) and
	// flag to indicate whether it is fully implemented.
	using FunTypeAndFlag = std::pair<FunctionTypePointer, bool>;
	map<string, vector<FunTypeAndFlag>> functions;

	// Search from base to derived
	for (ContractDefinition const* contract: boost::adaptors::reverse(_contract.annotation().linearizedBaseContracts))
		for (ASTPointer<FunctionDefinition> const& function: contract->definedFunctions())
		{
			auto& overloads = functions[function->name()];
			FunctionTypePointer funType = make_shared<FunctionType>(*function);
			auto it = find_if(overloads.begin(), overloads.end(), [&](FunTypeAndFlag const& _funAndFlag)
			{
				return funType->hasEqualArgumentTypes(*_funAndFlag.first);
			});
			if (it == overloads.end())
				overloads.push_back(make_pair(funType, function->isImplemented()));
			else if (it->second)
			{
				if (!function->isImplemented())
					typeError(*function, "Redeclaring an already implemented function as abstract");
			}
			else if (function->isImplemented())
				it->second = true;
		}

	// Set to not fully implemented if at least one flag is false.
	for (auto const& it: functions)
		for (auto const& funAndFlag: it.second)
			if (!funAndFlag.second)
			{
				_contract.annotation().isFullyImplemented = false;
				return;
			}
}

void TypeChecker::checkContractAbstractConstructors(ContractDefinition const& _contract)
{
	set<ContractDefinition const*> argumentsNeeded;
	// check that we get arguments for all base constructors that need it.
	// If not mark the contract as abstract (not fully implemented)

	vector<ContractDefinition const*> const& bases = _contract.annotation().linearizedBaseContracts;
	for (ContractDefinition const* contract: bases)
		if (FunctionDefinition const* constructor = contract->constructor())
			if (contract != &_contract && !constructor->parameters().empty())
				argumentsNeeded.insert(contract);

	for (ContractDefinition const* contract: bases)
	{
		if (FunctionDefinition const* constructor = contract->constructor())
			for (auto const& modifier: constructor->modifiers())
			{
				auto baseContract = dynamic_cast<ContractDefinition const*>(
					&dereference(*modifier->name())
				);
				if (baseContract)
					argumentsNeeded.erase(baseContract);
			}


		for (ASTPointer<InheritanceSpecifier> const& base: contract->baseContracts())
		{
			auto baseContract = dynamic_cast<ContractDefinition const*>(&dereference(base->name()));
			solAssert(baseContract, "");
			if (!base->arguments().empty())
				argumentsNeeded.erase(baseContract);
		}
	}
	if (!argumentsNeeded.empty())
		_contract.annotation().isFullyImplemented = false;
}

void TypeChecker::checkContractIllegalOverrides(ContractDefinition const& _contract)
{
	// TODO unify this at a later point. for this we need to put the constness and the access specifier
	// into the types
	map<string, vector<FunctionDefinition const*>> functions;
	map<string, ModifierDefinition const*> modifiers;

	// We search from derived to base, so the stored item causes the error.
	for (ContractDefinition const* contract: _contract.annotation().linearizedBaseContracts)
	{
		for (ASTPointer<FunctionDefinition> const& function: contract->definedFunctions())
		{
			if (function->isConstructor())
				continue; // constructors can neither be overridden nor override anything
			string const& name = function->name();
			if (modifiers.count(name))
				typeError(*modifiers[name], "Override changes function to modifier.");
			FunctionType functionType(*function);
			// function should not change the return type
			for (FunctionDefinition const* overriding: functions[name])
			{
				FunctionType overridingType(*overriding);
				if (!overridingType.hasEqualArgumentTypes(functionType))
					continue;
				if (
					overriding->visibility() != function->visibility() ||
					overriding->isDeclaredConst() != function->isDeclaredConst() ||
					overridingType != functionType
				)
					typeError(*overriding, "Override changes extended function signature.");
			}
			functions[name].push_back(function.get());
		}
		for (ASTPointer<ModifierDefinition> const& modifier: contract->functionModifiers())
		{
			string const& name = modifier->name();
			ModifierDefinition const*& override = modifiers[name];
			if (!override)
				override = modifier.get();
			else if (ModifierType(*override) != ModifierType(*modifier))
				typeError(*override, "Override changes modifier signature.");
			if (!functions[name].empty())
				typeError(*override, "Override changes modifier to function.");
		}
	}
}

void TypeChecker::checkContractExternalTypeClashes(ContractDefinition const& _contract)
{
	map<string, vector<pair<Declaration const*, FunctionTypePointer>>> externalDeclarations;
	for (ContractDefinition const* contract: _contract.annotation().linearizedBaseContracts)
	{
		for (ASTPointer<FunctionDefinition> const& f: contract->definedFunctions())
			if (f->isPartOfExternalInterface())
			{
				auto functionType = make_shared<FunctionType>(*f);
				externalDeclarations[functionType->externalSignature(f->name())].push_back(
					make_pair(f.get(), functionType)
				);
			}
		for (ASTPointer<VariableDeclaration> const& v: contract->stateVariables())
			if (v->isPartOfExternalInterface())
			{
				auto functionType = make_shared<FunctionType>(*v);
				externalDeclarations[functionType->externalSignature(v->name())].push_back(
					make_pair(v.get(), functionType)
				);
			}
	}
	for (auto const& it: externalDeclarations)
		for (size_t i = 0; i < it.second.size(); ++i)
			for (size_t j = i + 1; j < it.second.size(); ++j)
				if (!it.second[i].second->hasEqualArgumentTypes(*it.second[j].second))
					typeError(
						*it.second[j].first,
						"Function overload clash during conversion to external types for arguments."
					);
}

void TypeChecker::checkLibraryRequirements(ContractDefinition const& _contract)
{
	solAssert(_contract.isLibrary(), "");
	if (!_contract.baseContracts().empty())
		typeError(_contract, "Library is not allowed to inherit.");

	for (auto const& var: _contract.stateVariables())
		if (!var->isConstant())
			typeError(*var, "Library cannot have non-constant state variables");
}

void TypeChecker::endVisit(InheritanceSpecifier const& _inheritance)
{
	auto base = dynamic_cast<ContractDefinition const*>(&dereference(_inheritance.name()));
	solAssert(base, "Base contract not available.");

	if (base->isLibrary())
		typeError(_inheritance, "Libraries cannot be inherited from.");

	auto const& arguments = _inheritance.arguments();
	TypePointers parameterTypes = ContractType(*base).constructorType()->parameterTypes();
	if (!arguments.empty() && parameterTypes.size() != arguments.size())
		typeError(
			_inheritance,
			"Wrong argument count for constructor call: " +
			toString(arguments.size()) +
			" arguments given but expected " +
			toString(parameterTypes.size()) +
			"."
		);

	for (size_t i = 0; i < arguments.size(); ++i)
		if (!type(*arguments[i])->isImplicitlyConvertibleTo(*parameterTypes[i]))
			typeError(
				*arguments[i],
				"Invalid type for argument in constructor call. "
				"Invalid implicit conversion from " +
				type(*arguments[i])->toString() +
				" to " +
				parameterTypes[i]->toString() +
				" requested."
			);
}

bool TypeChecker::visit(StructDefinition const& _struct)
{
	for (ASTPointer<VariableDeclaration> const& member: _struct.members())
		if (!type(*member)->canBeStored())
			typeError(*member, "Type cannot be used in struct.");

	// Check recursion, fatal error if detected.
	using StructPointer = StructDefinition const*;
	using StructPointersSet = set<StructPointer>;
	function<void(StructPointer,StructPointersSet const&)> check = [&](StructPointer _struct, StructPointersSet const& _parents)
	{
		if (_parents.count(_struct))
			fatalTypeError(*_struct, "Recursive struct definition.");
		StructPointersSet parents = _parents;
		parents.insert(_struct);
		for (ASTPointer<VariableDeclaration> const& member: _struct->members())
			if (type(*member)->category() == Type::Category::Struct)
			{
				auto const& typeName = dynamic_cast<UserDefinedTypeName const&>(*member->typeName());
				check(&dynamic_cast<StructDefinition const&>(*typeName.annotation().referencedDeclaration), parents);
			}
	};
	check(&_struct, StructPointersSet{});

	ASTNode::listAccept(_struct.members(), *this);

	return false;
}

bool TypeChecker::visit(FunctionDefinition const& _function)
{
	for (ASTPointer<VariableDeclaration> const& var: _function.parameters() + _function.returnParameters())
	{
		if (!type(*var)->canLiveOutsideStorage())
			typeError(*var, "Type is required to live outside storage.");
		if (_function.visibility() >= FunctionDefinition::Visibility::Public && !(type(*var)->externalType()))
			typeError(*var, "Internal type is not allowed for public and external functions.");
	}
	for (ASTPointer<ModifierInvocation> const& modifier: _function.modifiers())
		visitManually(
			*modifier,
			_function.isConstructor() ?
			dynamic_cast<ContractDefinition const&>(*_function.scope()).annotation().linearizedBaseContracts :
			vector<ContractDefinition const*>()
		);
	if (_function.isImplemented())
		_function.body().accept(*this);
	return false;
}

bool TypeChecker::visit(VariableDeclaration const& _variable)
{
	// Variables can be declared without type (with "var"), in which case the first assignment
	// sets the type.
	// Note that assignments before the first declaration are legal because of the special scoping
	// rules inherited from JavaScript.

	// This only infers the type from its type name.
	// If an explicit type is required, it throws, otherwise it returns TypePointer();
	TypePointer varType = _variable.annotation().type;
	if (_variable.isConstant())
	{
		if (!dynamic_cast<ContractDefinition const*>(_variable.scope()))
			typeError(_variable, "Illegal use of \"constant\" specifier.");
		if (!_variable.value())
			typeError(_variable, "Uninitialized \"constant\" variable.");
		if (varType && !varType->isValueType())
		{
			bool constImplemented = false;
			if (auto arrayType = dynamic_cast<ArrayType const*>(varType.get()))
				constImplemented = arrayType->isByteArray();
			if (!constImplemented)
				typeError(
					_variable,
					"Illegal use of \"constant\" specifier. \"constant\" "
					"is not yet implemented for this type."
				);
		}
	}
	if (varType)
	{
		if (_variable.value())
			expectType(*_variable.value(), *varType);
		else
		{
			if (auto ref = dynamic_cast<ReferenceType const *>(varType.get()))
				if (ref->dataStoredIn(DataLocation::Storage) && _variable.isLocalVariable() && !_variable.isCallableParameter())
				{
					auto err = make_shared<Warning>();
					*err <<
						errinfo_sourceLocation(_variable.location()) <<
						errinfo_comment("Uninitialized storage pointer. Did you mean '<type> memory " + _variable.name() + "'?");
					m_errors.push_back(err);
				}
		}
	}
	else
	{
		// Infer type from value.
		if (!_variable.value())
			fatalTypeError(_variable, "Assignment necessary for type detection.");
		_variable.value()->accept(*this);

		TypePointer const& valueType = type(*_variable.value());
		solAssert(!!valueType, "");
		if (
			valueType->category() == Type::Category::IntegerConstant &&
			!dynamic_pointer_cast<IntegerConstantType const>(valueType)->integerType()
		)
			fatalTypeError(*_variable.value(), "Invalid integer constant " + valueType->toString() + ".");
		else if (valueType->category() == Type::Category::Void)
			fatalTypeError(_variable, "Variable cannot have void type.");
		varType = valueType->mobileType();
	}
	solAssert(!!varType, "");
	_variable.annotation().type = varType;
	if (!_variable.isStateVariable())
	{
		if (varType->dataStoredIn(DataLocation::Memory) || varType->dataStoredIn(DataLocation::CallData))
			if (!varType->canLiveOutsideStorage())
				typeError(_variable, "Type " + varType->toString() + " is only valid in storage.");
	}
	else if (
		_variable.visibility() >= VariableDeclaration::Visibility::Public &&
		!FunctionType(_variable).externalType()
	)
		typeError(_variable, "Internal type is not allowed for public state variables.");
	return false;
}

void TypeChecker::visitManually(
	ModifierInvocation const& _modifier,
	vector<ContractDefinition const*> const& _bases
)
{
	std::vector<ASTPointer<Expression>> const& arguments = _modifier.arguments();
	for (ASTPointer<Expression> const& argument: arguments)
		argument->accept(*this);
	_modifier.name()->accept(*this);

	auto const* declaration = &dereference(*_modifier.name());
	vector<ASTPointer<VariableDeclaration>> emptyParameterList;
	vector<ASTPointer<VariableDeclaration>> const* parameters = nullptr;
	if (auto modifierDecl = dynamic_cast<ModifierDefinition const*>(declaration))
		parameters = &modifierDecl->parameters();
	else
		// check parameters for Base constructors
		for (ContractDefinition const* base: _bases)
			if (declaration == base)
			{
				if (auto referencedConstructor = base->constructor())
					parameters = &referencedConstructor->parameters();
				else
					parameters = &emptyParameterList;
				break;
			}
	if (!parameters)
		typeError(_modifier, "Referenced declaration is neither modifier nor base class.");
	if (parameters->size() != arguments.size())
		typeError(
			_modifier,
			"Wrong argument count for modifier invocation: " +
			toString(arguments.size()) +
			" arguments given but expected " +
			toString(parameters->size()) +
			"."
		);
	for (size_t i = 0; i < _modifier.arguments().size(); ++i)
		if (!type(*arguments[i])->isImplicitlyConvertibleTo(*type(*(*parameters)[i])))
			typeError(
				*arguments[i],
				"Invalid type for argument in modifier invocation. "
				"Invalid implicit conversion from " +
				type(*arguments[i])->toString() +
				" to " +
				type(*(*parameters)[i])->toString() +
				" requested."
			);
}

bool TypeChecker::visit(EventDefinition const& _eventDef)
{
	unsigned numIndexed = 0;
	for (ASTPointer<VariableDeclaration> const& var: _eventDef.parameters())
	{
		if (var->isIndexed())
			numIndexed++;
		if (numIndexed > 3)
			typeError(_eventDef, "More than 3 indexed arguments for event.");
		if (!type(*var)->canLiveOutsideStorage())
			typeError(*var, "Type is required to live outside storage.");
		if (!type(*var)->externalType())
			typeError(*var, "Internal type is not allowed as event parameter type.");
	}
	return false;
}


bool TypeChecker::visit(IfStatement const& _ifStatement)
{
	expectType(_ifStatement.condition(), BoolType());
	_ifStatement.trueStatement().accept(*this);
	if (_ifStatement.falseStatement())
		_ifStatement.falseStatement()->accept(*this);
	return false;
}

bool TypeChecker::visit(WhileStatement const& _whileStatement)
{
	expectType(_whileStatement.condition(), BoolType());
	_whileStatement.body().accept(*this);
	return false;
}

bool TypeChecker::visit(ForStatement const& _forStatement)
{
	if (_forStatement.initializationExpression())
		_forStatement.initializationExpression()->accept(*this);
	if (_forStatement.condition())
		expectType(*_forStatement.condition(), BoolType());
	if (_forStatement.loopExpression())
		_forStatement.loopExpression()->accept(*this);
	_forStatement.body().accept(*this);
	return false;
}

void TypeChecker::endVisit(Return const& _return)
{
	if (!_return.expression())
		return;
	ParameterList const* params = _return.annotation().functionReturnParameters;
	if (!params)
		typeError(_return, "Return arguments not allowed.");
	else if (params->parameters().size() != 1)
		typeError(_return, "Different number of arguments in return statement than in returns declaration.");
	else
	{
		// this could later be changed such that the paramaters type is an anonymous struct type,
		// but for now, we only allow one return parameter
		TypePointer const& expected = type(*params->parameters().front());
		if (!type(*_return.expression())->isImplicitlyConvertibleTo(*expected))
			typeError(
				*_return.expression(),
				"Return argument type " +
				type(*_return.expression())->toString() +
				" is not implicitly convertible to expected type (type of first return variable) " +
				expected->toString() +
				"."
			);
	}
}

void TypeChecker::endVisit(ExpressionStatement const& _statement)
{
	if (type(_statement.expression())->category() == Type::Category::IntegerConstant)
		if (!dynamic_pointer_cast<IntegerConstantType const>(type(_statement.expression()))->integerType())
			typeError(_statement.expression(), "Invalid integer constant.");
}

bool TypeChecker::visit(Assignment const& _assignment)
{
	requireLValue(_assignment.leftHandSide());
	TypePointer t = type(_assignment.leftHandSide());
	_assignment.annotation().type = t;
	if (t->category() == Type::Category::Mapping)
	{
		typeError(_assignment, "Mappings cannot be assigned to.");
		_assignment.rightHandSide().accept(*this);
	}
	else if (_assignment.assignmentOperator() == Token::Assign)
		expectType(_assignment.rightHandSide(), *t);
	else
	{
		// compound assignment
		_assignment.rightHandSide().accept(*this);
		TypePointer resultType = t->binaryOperatorResult(
			Token::AssignmentToBinaryOp(_assignment.assignmentOperator()),
			type(_assignment.rightHandSide())
		);
		if (!resultType || *resultType != *t)
			typeError(
				_assignment,
				"Operator " +
				string(Token::toString(_assignment.assignmentOperator())) +
				" not compatible with types " +
				t->toString() +
				" and " +
				type(_assignment.rightHandSide())->toString()
			);
	}
	return false;
}

bool TypeChecker::visit(UnaryOperation const& _operation)
{
	// Inc, Dec, Add, Sub, Not, BitNot, Delete
	Token::Value op = _operation.getOperator();
	if (op == Token::Value::Inc || op == Token::Value::Dec || op == Token::Value::Delete)
		requireLValue(_operation.subExpression());
	else
		_operation.subExpression().accept(*this);
	TypePointer const& subExprType = type(_operation.subExpression());
	TypePointer t = type(_operation.subExpression())->unaryOperatorResult(op);
	if (!t)
	{
		typeError(
			_operation,
			"Unary operator " +
			string(Token::toString(op)) +
			" cannot be applied to type " +
			subExprType->toString()
		);
		t = subExprType;
	}
	_operation.annotation().type = t;
	return false;
}

void TypeChecker::endVisit(BinaryOperation const& _operation)
{
	TypePointer const& leftType = type(_operation.leftExpression());
	TypePointer const& rightType = type(_operation.rightExpression());
	TypePointer commonType = leftType->binaryOperatorResult(_operation.getOperator(), rightType);
	if (!commonType)
	{
		typeError(
			_operation,
			"Operator " +
			string(Token::toString(_operation.getOperator())) +
			" not compatible with types " +
			leftType->toString() +
			" and " +
			rightType->toString()
		);
		commonType = leftType;
	}
	_operation.annotation().commonType = commonType;
	_operation.annotation().type =
		Token::isCompareOp(_operation.getOperator()) ?
		make_shared<BoolType>() :
		commonType;
}

bool TypeChecker::visit(FunctionCall const& _functionCall)
{
	bool isPositionalCall = _functionCall.names().empty();
	vector<ASTPointer<Expression const>> arguments = _functionCall.arguments();
	vector<ASTPointer<ASTString>> const& argumentNames = _functionCall.names();

	// We need to check arguments' type first as they will be needed for overload resolution.
	shared_ptr<TypePointers> argumentTypes;
	if (isPositionalCall)
		argumentTypes = make_shared<TypePointers>();
	for (ASTPointer<Expression const> const& argument: arguments)
	{
		argument->accept(*this);
		// only store them for positional calls
		if (isPositionalCall)
			argumentTypes->push_back(type(*argument));
	}
	if (isPositionalCall)
		_functionCall.expression().annotation().argumentTypes = move(argumentTypes);

	_functionCall.expression().accept(*this);
	TypePointer expressionType = type(_functionCall.expression());

	if (auto const* typeType = dynamic_cast<TypeType const*>(expressionType.get()))
	{
		_functionCall.annotation().isStructConstructorCall = (typeType->actualType()->category() == Type::Category::Struct);
		_functionCall.annotation().isTypeConversion = !_functionCall.annotation().isStructConstructorCall;
	}
	else
		_functionCall.annotation().isStructConstructorCall = _functionCall.annotation().isTypeConversion = false;

	if (_functionCall.annotation().isTypeConversion)
	{
		TypeType const& t = dynamic_cast<TypeType const&>(*expressionType);
		TypePointer resultType = t.actualType();
		if (arguments.size() != 1)
			typeError(_functionCall, "Exactly one argument expected for explicit type conversion.");
		else if (!isPositionalCall)
			typeError(_functionCall, "Type conversion cannot allow named arguments.");
		else
		{
			TypePointer const& argType = type(*arguments.front());
			if (auto argRefType = dynamic_cast<ReferenceType const*>(argType.get()))
				// do not change the data location when converting
				// (data location cannot yet be specified for type conversions)
				resultType = ReferenceType::copyForLocationIfReference(argRefType->location(), resultType);
			if (!argType->isExplicitlyConvertibleTo(*resultType))
				typeError(_functionCall, "Explicit type conversion not allowed.");
		}
		_functionCall.annotation().type = resultType;

		return false;
	}

	// Actual function call or struct constructor call.

	FunctionTypePointer functionType;

	/// For error message: Struct members that were removed during conversion to memory.
	set<string> membersRemovedForStructConstructor;
	if (_functionCall.annotation().isStructConstructorCall)
	{
		TypeType const& t = dynamic_cast<TypeType const&>(*expressionType);
		auto const& structType = dynamic_cast<StructType const&>(*t.actualType());
		functionType = structType.constructorType();
		membersRemovedForStructConstructor = structType.membersMissingInMemory();
	}
	else
		functionType = dynamic_pointer_cast<FunctionType const>(expressionType);

	if (!functionType)
	{
		typeError(_functionCall, "Type is not callable");
		_functionCall.annotation().type = make_shared<VoidType>();
		return false;
	}
	else
	{
		// @todo actually the return type should be an anonymous struct,
		// but we change it to the type of the first return value until we have anonymous
		// structs and tuples
		if (functionType->returnParameterTypes().empty())
			_functionCall.annotation().type = make_shared<VoidType>();
		else
			_functionCall.annotation().type = functionType->returnParameterTypes().front();
	}

	//@todo would be nice to create a struct type from the arguments
	// and then ask if that is implicitly convertible to the struct represented by the
	// function parameters
	TypePointers const& parameterTypes = functionType->parameterTypes();
	if (!functionType->takesArbitraryParameters() && parameterTypes.size() != arguments.size())
	{
		string msg =
			"Wrong argument count for function call: " +
			toString(arguments.size()) +
			" arguments given but expected " +
			toString(parameterTypes.size()) +
			".";
		// Extend error message in case we try to construct a struct with mapping member.
		if (_functionCall.annotation().isStructConstructorCall && !membersRemovedForStructConstructor.empty())
		{
			msg += " Members that have to be skipped in memory:";
			for (auto const& member: membersRemovedForStructConstructor)
				msg += " " + member;
		}
		typeError(_functionCall, msg);
	}
	else if (isPositionalCall)
	{
		// call by positional arguments
		for (size_t i = 0; i < arguments.size(); ++i)
			if (
				!functionType->takesArbitraryParameters() &&
				!type(*arguments[i])->isImplicitlyConvertibleTo(*parameterTypes[i])
			)
				typeError(
					*arguments[i],
					"Invalid type for argument in function call. "
					"Invalid implicit conversion from " +
					type(*arguments[i])->toString() +
					" to " +
					parameterTypes[i]->toString() +
					" requested."
				);
	}
	else
	{
		// call by named arguments
		auto const& parameterNames = functionType->parameterNames();
		if (functionType->takesArbitraryParameters())
			typeError(
				_functionCall,
				"Named arguments cannnot be used for functions that take arbitrary parameters."
			);
		else if (parameterNames.size() > argumentNames.size())
			typeError(_functionCall, "Some argument names are missing.");
		else if (parameterNames.size() < argumentNames.size())
			typeError(_functionCall, "Too many arguments.");
		else
		{
			// check duplicate names
			bool duplication = false;
			for (size_t i = 0; i < argumentNames.size(); i++)
				for (size_t j = i + 1; j < argumentNames.size(); j++)
					if (*argumentNames[i] == *argumentNames[j])
					{
						duplication = true;
						typeError(*arguments[i], "Duplicate named argument.");
					}

			// check actual types
			if (!duplication)
				for (size_t i = 0; i < argumentNames.size(); i++)
				{
					bool found = false;
					for (size_t j = 0; j < parameterNames.size(); j++)
						if (parameterNames[j] == *argumentNames[i])
						{
							found = true;
							// check type convertible
							if (!type(*arguments[i])->isImplicitlyConvertibleTo(*parameterTypes[j]))
								typeError(
									*arguments[i],
									"Invalid type for argument in function call. "
									"Invalid implicit conversion from " +
									type(*arguments[i])->toString() +
									" to " +
									parameterTypes[i]->toString() +
									" requested."
								);
							break;
						}

					if (!found)
						typeError(
							_functionCall,
							"Named argument does not match function declaration."
						);
				}
		}
	}

	return false;
}

void TypeChecker::endVisit(NewExpression const& _newExpression)
{
	auto contract = dynamic_cast<ContractDefinition const*>(&dereference(_newExpression.contractName()));

	if (!contract)
		fatalTypeError(_newExpression, "Identifier is not a contract.");
	if (!contract->annotation().isFullyImplemented)
		typeError(_newExpression, "Trying to create an instance of an abstract contract.");

	auto scopeContract = _newExpression.contractName().annotation().contractScope;
	auto const& bases = contract->annotation().linearizedBaseContracts;
	solAssert(!bases.empty(), "Linearized base contracts not yet available.");
	if (find(bases.begin(), bases.end(), scopeContract) != bases.end())
		typeError(
			_newExpression,
			"Circular reference for contract creation: cannot create instance of derived or same contract."
		);

	auto contractType = make_shared<ContractType>(*contract);
	TypePointers const& parameterTypes = contractType->constructorType()->parameterTypes();
	_newExpression.annotation().type = make_shared<FunctionType>(
		parameterTypes,
		TypePointers{contractType},
		strings(),
		strings(),
		FunctionType::Location::Creation
	);
}

bool TypeChecker::visit(MemberAccess const& _memberAccess)
{
	_memberAccess.expression().accept(*this);
	TypePointer exprType = type(_memberAccess.expression());
	ASTString const& memberName = _memberAccess.memberName();

	// Retrieve the types of the arguments if this is used to call a function.
	auto const& argumentTypes = _memberAccess.annotation().argumentTypes;
	MemberList::MemberMap possibleMembers = exprType->members().membersByName(memberName);
	if (possibleMembers.size() > 1 && argumentTypes)
	{
		// do overload resolution
		for (auto it = possibleMembers.begin(); it != possibleMembers.end();)
			if (
				it->type->category() == Type::Category::Function &&
				!dynamic_cast<FunctionType const&>(*it->type).canTakeArguments(*argumentTypes)
			)
				it = possibleMembers.erase(it);
			else
				++it;
	}
	if (possibleMembers.size() == 0)
	{
		auto storageType = ReferenceType::copyForLocationIfReference(
			DataLocation::Storage,
			exprType
		);
		if (!storageType->members().membersByName(memberName).empty())
			fatalTypeError(
				_memberAccess,
				"Member \"" + memberName + "\" is not available in " +
				exprType->toString() +
				" outside of storage."
			);
		fatalTypeError(
			_memberAccess,
			"Member \"" + memberName + "\" not found or not visible "
			"after argument-dependent lookup in " + exprType->toString()
		);
	}
	else if (possibleMembers.size() > 1)
		fatalTypeError(
			_memberAccess,
			"Member \"" + memberName + "\" not unique "
			"after argument-dependent lookup in " + exprType->toString()
		);

	auto& annotation = _memberAccess.annotation();
	annotation.referencedDeclaration = possibleMembers.front().declaration;
	annotation.type = possibleMembers.front().type;
	if (exprType->category() == Type::Category::Struct)
		annotation.isLValue = true;
	else if (exprType->category() == Type::Category::Array)
	{
		auto const& arrayType(dynamic_cast<ArrayType const&>(*exprType));
		annotation.isLValue = (
			memberName == "length" &&
			arrayType.location() == DataLocation::Storage &&
			arrayType.isDynamicallySized()
		);
	}

	return false;
}

bool TypeChecker::visit(IndexAccess const& _access)
{
	_access.baseExpression().accept(*this);
	TypePointer baseType = type(_access.baseExpression());
	TypePointer resultType;
	bool isLValue = false;
	Expression const* index = _access.indexExpression();
	switch (baseType->category())
	{
	case Type::Category::Array:
	{
		ArrayType const& actualType = dynamic_cast<ArrayType const&>(*baseType);
		if (!index)
			typeError(_access, "Index expression cannot be omitted.");
		else if (actualType.isString())
		{
			typeError(_access, "Index access for string is not possible.");
			index->accept(*this);
		}
		else
		{
			expectType(*index, IntegerType(256));
			if (auto integerType = dynamic_cast<IntegerConstantType const*>(type(*index).get()))
				if (!actualType.isDynamicallySized() && actualType.length() <= integerType->literalValue(nullptr))
					typeError(_access, "Out of bounds array access.");
		}
		resultType = actualType.baseType();
		isLValue = actualType.location() != DataLocation::CallData;
		break;
	}
	case Type::Category::Mapping:
	{
		MappingType const& actualType = dynamic_cast<MappingType const&>(*baseType);
		if (!index)
			typeError(_access, "Index expression cannot be omitted.");
		else
			expectType(*index, *actualType.keyType());
		resultType = actualType.valueType();
		isLValue = true;
		break;
	}
	case Type::Category::TypeType:
	{
		TypeType const& typeType = dynamic_cast<TypeType const&>(*baseType);
		if (!index)
			resultType = make_shared<TypeType>(make_shared<ArrayType>(DataLocation::Memory, typeType.actualType()));
		else
		{
			index->accept(*this);
			if (auto length = dynamic_cast<IntegerConstantType const*>(type(*index).get()))
				resultType = make_shared<TypeType>(make_shared<ArrayType>(
					DataLocation::Memory,
					typeType.actualType(),
					length->literalValue(nullptr)
				));
			else
				typeError(*index, "Integer constant expected.");
		}
		break;
	}
	default:
		fatalTypeError(
			_access.baseExpression(),
			"Indexed expression has to be a type, mapping or array (is " + baseType->toString() + ")"
		);
	}
	_access.annotation().type = move(resultType);
	_access.annotation().isLValue = isLValue;

	return false;
}

bool TypeChecker::visit(Identifier const& _identifier)
{
	IdentifierAnnotation& annotation = _identifier.annotation();
	if (!annotation.referencedDeclaration)
	{
		if (!annotation.argumentTypes)
			fatalTypeError(_identifier, "Unable to determine overloaded type.");
		if (annotation.overloadedDeclarations.empty())
			fatalTypeError(_identifier, "No candidates for overload resolution found.");
		else if (annotation.overloadedDeclarations.size() == 1)
			annotation.referencedDeclaration = *annotation.overloadedDeclarations.begin();
		else
		{
			vector<Declaration const*> candidates;

			for (Declaration const* declaration: annotation.overloadedDeclarations)
			{
				TypePointer function = declaration->type(_identifier.annotation().contractScope);
				solAssert(!!function, "Requested type not present.");
				auto const* functionType = dynamic_cast<FunctionType const*>(function.get());
				if (functionType && functionType->canTakeArguments(*annotation.argumentTypes))
					candidates.push_back(declaration);
			}
			if (candidates.empty())
				fatalTypeError(_identifier, "No matching declaration found after argument-dependent lookup.");
			else if (candidates.size() == 1)
				annotation.referencedDeclaration = candidates.front();
			else
				fatalTypeError(_identifier, "No unique declaration found after argument-dependent lookup.");
		}
	}
	solAssert(
		!!annotation.referencedDeclaration,
		"Referenced declaration is null after overload resolution."
	);
	annotation.isLValue = annotation.referencedDeclaration->isLValue();
	annotation.type = annotation.referencedDeclaration->type(_identifier.annotation().contractScope);
	if (!annotation.type)
		fatalTypeError(_identifier, "Declaration referenced before type could be determined.");
	return false;
}

void TypeChecker::endVisit(ElementaryTypeNameExpression const& _expr)
{
	_expr.annotation().type = make_shared<TypeType>(Type::fromElementaryTypeName(_expr.typeToken()));
}

void TypeChecker::endVisit(Literal const& _literal)
{
	_literal.annotation().type = Type::forLiteral(_literal);
	if (!_literal.annotation().type)
		fatalTypeError(_literal, "Invalid literal value.");
}

Declaration const& TypeChecker::dereference(Identifier const& _identifier)
{
	solAssert(!!_identifier.annotation().referencedDeclaration, "Declaration not stored.");
	return *_identifier.annotation().referencedDeclaration;
}

void TypeChecker::expectType(Expression const& _expression, Type const& _expectedType)
{
	_expression.accept(*this);

	if (!type(_expression)->isImplicitlyConvertibleTo(_expectedType))
		typeError(
			_expression,
			"Type " +
			type(_expression)->toString() +
			" is not implicitly convertible to expected type " +
			_expectedType.toString() +
			"."
		);
}

void TypeChecker::requireLValue(Expression const& _expression)
{
	_expression.accept(*this);
	if (!_expression.annotation().isLValue)
		typeError(_expression, "Expression has to be an lvalue.");
	_expression.annotation().lValueRequested = true;
}

void TypeChecker::typeError(ASTNode const& _node, string const& _description)
{
	auto err = make_shared<TypeError>();
	*err <<
		errinfo_sourceLocation(_node.location()) <<
		errinfo_comment(_description);

	m_errors.push_back(err);
}

void TypeChecker::fatalTypeError(ASTNode const& _node, string const& _description)
{
	typeError(_node, _description);
	BOOST_THROW_EXCEPTION(FatalError());
}
