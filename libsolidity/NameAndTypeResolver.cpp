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
 * @date 2014
 * Parser part that determines the declarations corresponding to names and the types of expressions.
 */

#include <libsolidity/NameAndTypeResolver.h>
#include <libsolidity/AST.h>
#include <libsolidity/TypeChecker.h>
#include <libsolidity/Exceptions.h>

using namespace std;

namespace dev
{
namespace solidity
{

NameAndTypeResolver::NameAndTypeResolver(
	vector<Declaration const*> const& _globals,
	ErrorList& _errors
) :
	m_errors(_errors)
{
	for (Declaration const* declaration: _globals)
		m_scopes[nullptr].registerDeclaration(*declaration);
}

bool NameAndTypeResolver::registerDeclarations(SourceUnit& _sourceUnit)
{
	// The helper registers all declarations in m_scopes as a side-effect of its construction.
	try
	{
		DeclarationRegistrationHelper registrar(m_scopes, _sourceUnit, m_errors);
	}
	catch (FatalError)
	{
		return false;
	}
	return true;
}

bool NameAndTypeResolver::resolveNamesAndTypes(ContractDefinition& _contract)
{
	try
	{
		m_currentScope = &m_scopes[nullptr];

		for (ASTPointer<InheritanceSpecifier> const& baseContract: _contract.baseContracts())
			ReferencesResolver resolver(*baseContract, *this, &_contract, nullptr);

		m_currentScope = &m_scopes[&_contract];

		linearizeBaseContracts(_contract);
		std::vector<ContractDefinition const*> properBases(
			++_contract.annotation().linearizedBaseContracts.begin(),
			_contract.annotation().linearizedBaseContracts.end()
		);

		for (ContractDefinition const* base: properBases)
			importInheritedScope(*base);

		for (ASTPointer<StructDefinition> const& structDef: _contract.definedStructs())
			ReferencesResolver resolver(*structDef, *this, &_contract, nullptr);
		for (ASTPointer<EnumDefinition> const& enumDef: _contract.definedEnums())
			ReferencesResolver resolver(*enumDef, *this, &_contract, nullptr);
		for (ASTPointer<VariableDeclaration> const& variable: _contract.stateVariables())
			ReferencesResolver resolver(*variable, *this, &_contract, nullptr);
		for (ASTPointer<EventDefinition> const& event: _contract.events())
			ReferencesResolver resolver(*event, *this, &_contract, nullptr);

		// these can contain code, only resolve parameters for now
		for (ASTPointer<ModifierDefinition> const& modifier: _contract.functionModifiers())
		{
			m_currentScope = &m_scopes[modifier.get()];
			ReferencesResolver resolver(*modifier, *this, &_contract, nullptr);
		}
		for (ASTPointer<FunctionDefinition> const& function: _contract.definedFunctions())
		{
			m_currentScope = &m_scopes[function.get()];
			ReferencesResolver referencesResolver(
				*function,
				*this,
				&_contract,
				function->returnParameterList().get()
			);
		}

		m_currentScope = &m_scopes[&_contract];

		// now resolve references inside the code
		for (ASTPointer<ModifierDefinition> const& modifier: _contract.functionModifiers())
		{
			m_currentScope = &m_scopes[modifier.get()];
			ReferencesResolver resolver(*modifier, *this, &_contract, nullptr, true);
		}
		for (ASTPointer<FunctionDefinition> const& function: _contract.definedFunctions())
		{
			m_currentScope = &m_scopes[function.get()];
			ReferencesResolver referencesResolver(
				*function,
				*this,
				&_contract,
				function->returnParameterList().get(),
				true
			);
		}
	}
	catch (FatalError const& _e)
	{
		return false;
	}
	return true;
}

bool NameAndTypeResolver::updateDeclaration(Declaration const& _declaration)
{
	try
	{
		m_scopes[nullptr].registerDeclaration(_declaration, false, true);
		solAssert(_declaration.scope() == nullptr, "Updated declaration outside global scope.");
	}
	catch(FatalError _error)
	{
		return false;
	}
	return true;
}

vector<Declaration const*> NameAndTypeResolver::resolveName(ASTString const& _name, Declaration const* _scope) const
{
	auto iterator = m_scopes.find(_scope);
	if (iterator == end(m_scopes))
		return vector<Declaration const*>({});
	return iterator->second.resolveName(_name, false);
}

vector<Declaration const*> NameAndTypeResolver::nameFromCurrentScope(ASTString const& _name, bool _recursive) const
{
	return m_currentScope->resolveName(_name, _recursive);
}

Declaration const* NameAndTypeResolver::pathFromCurrentScope(vector<ASTString> const& _path, bool _recursive) const
{
	solAssert(!_path.empty(), "");
	vector<Declaration const*> candidates = m_currentScope->resolveName(_path.front(), _recursive);
	for (size_t i = 1; i < _path.size() && candidates.size() == 1; i++)
	{
		if (!m_scopes.count(candidates.front()))
			return nullptr;
		candidates = m_scopes.at(candidates.front()).resolveName(_path[i], false);
	}
	if (candidates.size() == 1)
		return candidates.front();
	else
		return nullptr;
}

vector<Declaration const*> NameAndTypeResolver::cleanedDeclarations(
		Identifier const& _identifier,
		vector<Declaration const*> const& _declarations
)
{
	solAssert(_declarations.size() > 1, "");
	vector<Declaration const*> uniqueFunctions;

	for (auto it = _declarations.begin(); it != _declarations.end(); ++it)
	{
		solAssert(*it, "");
		// the declaration is functionDefinition while declarations > 1
		FunctionDefinition const& functionDefinition = dynamic_cast<FunctionDefinition const&>(**it);
		FunctionType functionType(functionDefinition);
		for (auto parameter: functionType.parameterTypes() + functionType.returnParameterTypes())
			if (!parameter)
				reportFatalDeclarationError(_identifier.location(), "Function type can not be used in this context");

		if (uniqueFunctions.end() == find_if(
			uniqueFunctions.begin(),
			uniqueFunctions.end(),
			[&](Declaration const* d)
			{
				FunctionType newFunctionType(dynamic_cast<FunctionDefinition const&>(*d));
				return functionType.hasEqualArgumentTypes(newFunctionType);
			}
		))
			uniqueFunctions.push_back(*it);
	}
	return uniqueFunctions;
}

void NameAndTypeResolver::importInheritedScope(ContractDefinition const& _base)
{
	auto iterator = m_scopes.find(&_base);
	solAssert(iterator != end(m_scopes), "");
	for (auto const& nameAndDeclaration: iterator->second.declarations())
		for (auto const& declaration: nameAndDeclaration.second)
			// Import if it was declared in the base, is not the constructor and is visible in derived classes
			if (declaration->scope() == &_base && declaration->isVisibleInDerivedContracts())
				m_currentScope->registerDeclaration(*declaration);
}

void NameAndTypeResolver::linearizeBaseContracts(ContractDefinition& _contract)
{
	// order in the lists is from derived to base
	// list of lists to linearize, the last element is the list of direct bases
	list<list<ContractDefinition const*>> input(1, {});
	for (ASTPointer<InheritanceSpecifier> const& baseSpecifier: _contract.baseContracts())
	{
		Identifier const& baseName = baseSpecifier->name();
		auto base = dynamic_cast<ContractDefinition const*>(baseName.annotation().referencedDeclaration);
		if (!base)
			reportFatalTypeError(baseName.createTypeError("Contract expected."));
		// "push_front" has the effect that bases mentioned later can overwrite members of bases
		// mentioned earlier
		input.back().push_front(base);
		vector<ContractDefinition const*> const& basesBases = base->annotation().linearizedBaseContracts;
		if (basesBases.empty())
			reportFatalTypeError(baseName.createTypeError("Definition of base has to precede definition of derived contract"));
		input.push_front(list<ContractDefinition const*>(basesBases.begin(), basesBases.end()));
	}
	input.back().push_front(&_contract);
	vector<ContractDefinition const*> result = cThreeMerge(input);
	if (result.empty())
		reportFatalTypeError(_contract.createTypeError("Linearization of inheritance graph impossible"));
	_contract.annotation().linearizedBaseContracts = result;
	_contract.annotation().contractDependencies.insert(result.begin() + 1, result.end());
}

template <class _T>
vector<_T const*> NameAndTypeResolver::cThreeMerge(list<list<_T const*>>& _toMerge)
{
	// returns true iff _candidate appears only as last element of the lists
	auto appearsOnlyAtHead = [&](_T const* _candidate) -> bool
	{
		for (list<_T const*> const& bases: _toMerge)
		{
			solAssert(!bases.empty(), "");
			if (find(++bases.begin(), bases.end(), _candidate) != bases.end())
				return false;
		}
		return true;
	};
	// returns the next candidate to append to the linearized list or nullptr on failure
	auto nextCandidate = [&]() -> _T const*
	{
		for (list<_T const*> const& bases: _toMerge)
		{
			solAssert(!bases.empty(), "");
			if (appearsOnlyAtHead(bases.front()))
				return bases.front();
		}
		return nullptr;
	};
	// removes the given contract from all lists
	auto removeCandidate = [&](_T const* _candidate)
	{
		for (auto it = _toMerge.begin(); it != _toMerge.end();)
		{
			it->remove(_candidate);
			if (it->empty())
				it = _toMerge.erase(it);
			else
				++it;
		}
	};

	_toMerge.remove_if([](list<_T const*> const& _bases) { return _bases.empty(); });
	vector<_T const*> result;
	while (!_toMerge.empty())
	{
		_T const* candidate = nextCandidate();
		if (!candidate)
			return vector<_T const*>();
		result.push_back(candidate);
		removeCandidate(candidate);
	}
	return result;
}

void NameAndTypeResolver::reportDeclarationError(
	SourceLocation _sourceLoction,
	string const& _description,
	SourceLocation _secondarySourceLocation,
	string const& _secondaryDescription
)
{
	auto err = make_shared<Error>(Error::Type::DeclarationError); // todo remove Error?
	*err <<
		errinfo_sourceLocation(_sourceLoction) <<
		errinfo_comment(_description) <<
		errinfo_secondarySourceLocation(
			SecondarySourceLocation().append(_secondaryDescription, _secondarySourceLocation)
		);

	m_errors.push_back(err);
}

void NameAndTypeResolver::reportDeclarationError(SourceLocation _sourceLoction,	string const& _description)
{
	auto err = make_shared<Error>(Error::Type::DeclarationError); // todo remove Error?
	*err <<	errinfo_sourceLocation(_sourceLoction) << errinfo_comment(_description);

	m_errors.push_back(err);
}

void NameAndTypeResolver::reportFatalDeclarationError(
	SourceLocation _sourceLoction,
	string _description
)
{
	reportDeclarationError(_sourceLoction, _description);
	BOOST_THROW_EXCEPTION(FatalError());
}

void NameAndTypeResolver::reportTypeError(Error _e)
{
	m_errors.push_back(make_shared<Error>(_e));
}

void NameAndTypeResolver::reportFatalTypeError(Error _e)
{
	reportTypeError(_e);
	BOOST_THROW_EXCEPTION(FatalError());
}

DeclarationRegistrationHelper::DeclarationRegistrationHelper(
	map<ASTNode const*, DeclarationContainer>& _scopes,
	ASTNode& _astRoot,
	ErrorList& _errors
):
	m_scopes(_scopes),
	m_currentScope(nullptr),
	m_errors(_errors)
{
	_astRoot.accept(*this);
}

bool DeclarationRegistrationHelper::visit(ContractDefinition& _contract)
{
	registerDeclaration(_contract, true);
	_contract.annotation().canonicalName = currentCanonicalName();
	return true;
}

void DeclarationRegistrationHelper::endVisit(ContractDefinition&)
{
	closeCurrentScope();
}

bool DeclarationRegistrationHelper::visit(StructDefinition& _struct)
{
	registerDeclaration(_struct, true);
	_struct.annotation().canonicalName = currentCanonicalName();
	return true;
}

void DeclarationRegistrationHelper::endVisit(StructDefinition&)
{
	closeCurrentScope();
}

bool DeclarationRegistrationHelper::visit(EnumDefinition& _enum)
{
	registerDeclaration(_enum, true);
	_enum.annotation().canonicalName = currentCanonicalName();
	return true;
}

void DeclarationRegistrationHelper::endVisit(EnumDefinition&)
{
	closeCurrentScope();
}

bool DeclarationRegistrationHelper::visit(EnumValue& _value)
{
	registerDeclaration(_value, false);
	return true;
}

bool DeclarationRegistrationHelper::visit(FunctionDefinition& _function)
{
	registerDeclaration(_function, true);
	m_currentFunction = &_function;
	return true;
}

void DeclarationRegistrationHelper::endVisit(FunctionDefinition&)
{
	m_currentFunction = nullptr;
	closeCurrentScope();
}

bool DeclarationRegistrationHelper::visit(ModifierDefinition& _modifier)
{
	registerDeclaration(_modifier, true);
	m_currentFunction = &_modifier;
	return true;
}

void DeclarationRegistrationHelper::endVisit(ModifierDefinition&)
{
	m_currentFunction = nullptr;
	closeCurrentScope();
}

void DeclarationRegistrationHelper::endVisit(VariableDeclarationStatement& _variableDeclarationStatement)
{
	// Register the local variables with the function
	// This does not fit here perfectly, but it saves us another AST visit.
	solAssert(m_currentFunction, "Variable declaration without function.");
	for (ASTPointer<VariableDeclaration> const& var: _variableDeclarationStatement.declarations())
		if (var)
			m_currentFunction->addLocalVariable(*var);
}

bool DeclarationRegistrationHelper::visit(VariableDeclaration& _declaration)
{
	registerDeclaration(_declaration, false);
	return true;
}

bool DeclarationRegistrationHelper::visit(EventDefinition& _event)
{
	registerDeclaration(_event, true);
	return true;
}

void DeclarationRegistrationHelper::endVisit(EventDefinition&)
{
	closeCurrentScope();
}

void DeclarationRegistrationHelper::enterNewSubScope(Declaration const& _declaration)
{
	map<ASTNode const*, DeclarationContainer>::iterator iter;
	bool newlyAdded;
	tie(iter, newlyAdded) = m_scopes.emplace(&_declaration, DeclarationContainer(m_currentScope, &m_scopes[m_currentScope]));
	solAssert(newlyAdded, "Unable to add new scope.");
	m_currentScope = &_declaration;
}

void DeclarationRegistrationHelper::closeCurrentScope()
{
	solAssert(m_currentScope, "Closed non-existing scope.");
	m_currentScope = m_scopes[m_currentScope].enclosingDeclaration();
}

void DeclarationRegistrationHelper::registerDeclaration(Declaration& _declaration, bool _opensScope)
{
	if (!m_scopes[m_currentScope].registerDeclaration(_declaration, !_declaration.isVisibleInContract()))
	{
		SourceLocation firstDeclarationLocation;
		SourceLocation secondDeclarationLocation;
		Declaration const* conflictingDeclaration = m_scopes[m_currentScope].conflictingDeclaration(_declaration);
		solAssert(conflictingDeclaration, "");

		if (_declaration.location().start < conflictingDeclaration->location().start)
		{
			firstDeclarationLocation = _declaration.location();
			secondDeclarationLocation = conflictingDeclaration->location();
		}
		else
		{
			firstDeclarationLocation = conflictingDeclaration->location();
			secondDeclarationLocation = _declaration.location();
		}

		declarationError(
			secondDeclarationLocation,
			"Identifier already declared.",
			firstDeclarationLocation,
			"The previous declaration is here:"
		);
	}

	_declaration.setScope(m_currentScope);
	if (_opensScope)
		enterNewSubScope(_declaration);
}

string DeclarationRegistrationHelper::currentCanonicalName() const
{
	string ret;
	for (
		Declaration const* scope = m_currentScope;
		scope != nullptr;
		scope = m_scopes[scope].enclosingDeclaration()
	)
	{
		if (!ret.empty())
			ret = "." + ret;
		ret = scope->name() + ret;
	}
	return ret;
}

void DeclarationRegistrationHelper::declarationError(
	SourceLocation _sourceLoction,
	string const& _description,
	SourceLocation _secondarySourceLocation,
	string const& _secondaryDescription
)
{
	auto err = make_shared<Error>(Error::Type::DeclarationError);
	*err <<
		errinfo_sourceLocation(_sourceLoction) <<
		errinfo_comment(_description) <<
		errinfo_secondarySourceLocation(
			SecondarySourceLocation().append(_secondaryDescription, _secondarySourceLocation)
		);

	m_errors.push_back(err);
}

void DeclarationRegistrationHelper::declarationError(SourceLocation _sourceLoction, string const& _description)
{
	auto err = make_shared<Error>(Error::Type::DeclarationError);
	*err <<	errinfo_sourceLocation(_sourceLoction) << errinfo_comment(_description);

	m_errors.push_back(err);
}

void DeclarationRegistrationHelper::fatalDeclarationError(
	SourceLocation _sourceLoction,
	string const& _description
)
{
	declarationError(_sourceLoction, _description);
	BOOST_THROW_EXCEPTION(FatalError());
}

}
}
