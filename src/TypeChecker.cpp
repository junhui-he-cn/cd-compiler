#include "TypeChecker.hpp"

#include "TypeCheckerInternal.hpp"
#include "NativeStdlib.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>


TypeError::TypeError(std::string message)
    : DiagnosticError(DiagnosticKind::Type, std::move(message))
{
}

TypeError::TypeError(const Token& token, std::string message)
    : DiagnosticError(
        DiagnosticKind::Type,
        SourceLocation{token.line, token.column},
        token.range,
        std::move(message))
{
}

void TypeChecker::setPreloadedModuleInterfaces(std::vector<ModuleInterface> interfaces)
{
    preloadedModuleInterfaces_ = std::move(interfaces);
}

void TypeChecker::check(const Program& program)
{
    declarationIndex_ = DeclarationIndex::collect(program);
    declarationIndexMismatchCount_ = 0;
    moduleInterfaceMismatchCount_ = 0;
    scopes_.clear();
    bindingsById_.clear();
    typeParameterScopes_.clear();
    structTypes_.clear();
    structDeclarations_.clear();
    structCheckStates_.clear();
    enumTypes_.clear();
    enumDeclarations_.clear();
    enumCheckStates_.clear();
    methods_.clear();
    moduleSymbols_.clear();
    moduleInterfaces_ = preloadedModuleInterfaces_;
    preloadedModuleIds_.clear();
    for (const ModuleInterface& interfaceInfo : moduleInterfaces_) {
        preloadedModuleIds_.insert(interfaceInfo.moduleId);
    }
    checkedModules_.clear();
    checkedModuleBodyIds_.clear();
    moduleStack_.clear();
    currentProgram_ = &program;
    nextResolvedName_ = 0;
    const auto observeResolvedName = [this](const std::string& resolvedName) {
        const std::size_t marker = resolvedName.rfind('#');
        if (marker == std::string::npos || marker + 1 == resolvedName.size()) {
            return;
        }
        const std::string suffix = resolvedName.substr(marker + 1);
        if (suffix.find_first_not_of("0123456789") != std::string::npos) {
            return;
        }
        try {
            const unsigned long long value = std::stoull(suffix);
            if (value < std::numeric_limits<std::size_t>::max()) {
                nextResolvedName_ = std::max(nextResolvedName_, static_cast<std::size_t>(value + 1));
            }
        } catch (const std::exception&) {
            // A sidecar with a non-numeric linkage suffix remains usable; it
            // simply does not contribute to the local numeric allocator.
        }
    };
    for (const ModuleInterface& interfaceInfo : moduleInterfaces_) {
        for (const ModuleInterfaceValue& value : interfaceInfo.values) {
            observeResolvedName(value.resolvedName);
        }
        for (const ModuleInterfaceStruct& structure : interfaceInfo.structs) {
            for (const ModuleInterfaceMethod& method : structure.methods) {
                observeResolvedName(method.resolvedName);
            }
        }
        nextResolvedName_ = std::max(nextResolvedName_, interfaceInfo.resolvedNameNext);
    }
    nextBindingId_ = 0;
    functionDepth_ = 0;
    loopDepth_ = 0;
    returnContexts_.clear();

    checkModulesInDependencyOrder(program);

    buildModuleInterfaces(program);
    moduleInterfaceMismatchCount_ = validateModuleInterfaces(program);
    declarationIndexMismatchCount_ = declarationIndex_.validateMetadata();
    currentProgram_ = nullptr;
}

const std::vector<ModuleInterface>& TypeChecker::moduleInterfaces() const
{
    return moduleInterfaces_;
}

const std::vector<std::size_t>& TypeChecker::checkedModuleBodyIds() const
{
    return checkedModuleBodyIds_;
}

const DeclarationIndex& TypeChecker::declarationIndex() const
{
    return declarationIndex_;
}

std::size_t TypeChecker::declarationIndexMismatchCount() const
{
    return declarationIndexMismatchCount_;
}

std::size_t TypeChecker::moduleInterfaceMismatchCount() const
{
    return moduleInterfaceMismatchCount_;
}

void TypeChecker::beginScope()
{
    scopes_.emplace_back();
}

void TypeChecker::endScope()
{
    if (scopes_.empty()) {
        throw TypeError("scope stack is empty");
    }
    for (const auto& entry : scopes_.back()) {
        if (entry.second.declarationId.valid()) {
            bindingsById_.erase(entry.second.declarationId);
        }
    }
    scopes_.pop_back();
}

void TypeChecker::beginTypeParameterScope(const std::vector<TypeParameter>& parameters)
{
    std::unordered_map<std::string, TypeInfo> scope;
    for (const TypeParameter& parameter : parameters) {
        if (scope.find(parameter.name.lexeme) != scope.end()) {
            throw TypeError(parameter.name,
                "duplicate type parameter `" + parameter.name.lexeme + "`");
        }
        scope.emplace(parameter.name.lexeme, typeParameterType(parameter.name.lexeme));
    }
    typeParameterScopes_.push_back(std::move(scope));

    for (const TypeParameter& parameter : parameters) {
        if (parameter.constraints.empty()) {
            continue;
        }
        TypeInfo constraint = resolveTypeParameterConstraints(parameter);
        std::unordered_set<std::string> noTypeParameters;
        if (hasEscapingTypeParameter(constraint, noTypeParameters)) {
            throw TypeError(parameter.name,
                "constraint for type parameter `" + parameter.name.lexeme
                    + "` must be concrete");
        }
        typeParameterScopes_.back().at(parameter.name.lexeme).typeParameterConstraint
            = std::make_shared<TypeInfo>(std::move(constraint));
    }
}

void TypeChecker::endTypeParameterScope()
{
    if (typeParameterScopes_.empty()) {
        throw TypeError("type parameter scope stack is empty");
    }
    typeParameterScopes_.pop_back();
}

const TypeInfo* TypeChecker::findTypeParameter(const std::string& name) const
{
    for (auto scope = typeParameterScopes_.rbegin(); scope != typeParameterScopes_.rend(); ++scope) {
        const auto found = scope->find(name);
        if (found != scope->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

TypeChecker::Scope& TypeChecker::currentScope()
{
    if (scopes_.empty()) {
        throw TypeError("scope stack is empty");
    }
    return scopes_.back();
}

const TypeChecker::Scope& TypeChecker::currentScope() const
{
    if (scopes_.empty()) {
        throw TypeError("scope stack is empty");
    }
    return scopes_.back();
}

TypeChecker::Binding* TypeChecker::findVariable(const std::string& name)
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto found = scope->find(name);
        if (found != scope->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

const TypeChecker::Binding* TypeChecker::findVariable(const std::string& name) const
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto found = scope->find(name);
        if (found != scope->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

TypeChecker::Binding* TypeChecker::bindingById(DeclarationId id)
{
    const auto found = bindingsById_.find(id);
    return found == bindingsById_.end() ? nullptr : found->second;
}

const TypeChecker::Binding* TypeChecker::bindingById(DeclarationId id) const
{
    const auto found = bindingsById_.find(id);
    return found == bindingsById_.end() ? nullptr : found->second;
}

TypeChecker::Binding* TypeChecker::resolveVariableReference(const VariableExpr& expression)
{
    if (const auto reference = declarationIndex_.variableReference(expression)) {
        if (Binding* binding = bindingById(reference->declarationId)) {
            return binding;
        }
    }
    return findVariable(expression.name.lexeme);
}

const TypeChecker::Binding* TypeChecker::resolveVariableReference(const VariableExpr& expression) const
{
    if (const auto reference = declarationIndex_.variableReference(expression)) {
        if (const Binding* binding = bindingById(reference->declarationId)) {
            return binding;
        }
    }
    return findVariable(expression.name.lexeme);
}

TypeChecker::Binding* TypeChecker::resolveAssignmentTarget(const AssignExpr& expression)
{
    if (const auto reference = declarationIndex_.assignmentReference(expression)) {
        if (Binding* binding = bindingById(reference->declarationId)) {
            return binding;
        }
    }
    return findVariable(expression.name.lexeme);
}

TypeChecker::Binding* TypeChecker::resolveCompoundAssignmentTarget(
    const CompoundAssignExpr& expression)
{
    if (const auto reference = declarationIndex_.compoundAssignmentReference(expression)) {
        if (Binding* binding = bindingById(reference->declarationId)) {
            return binding;
        }
    }
    return findVariable(expression.name.lexeme);
}

TypeChecker::Binding* TypeChecker::findSimpleVariableBinding(const Expr& expression)
{
    const auto* variable = dynamic_cast<const VariableExpr*>(&expression);
    if (!variable) {
        return nullptr;
    }
    return resolveVariableReference(*variable);
}

const TypeChecker::Binding* TypeChecker::findSimpleVariableBinding(const Expr& expression) const
{
    const auto* variable = dynamic_cast<const VariableExpr*>(&expression);
    if (!variable) {
        return nullptr;
    }
    return resolveVariableReference(*variable);
}

TypeChecker::Binding TypeChecker::declareVariable(
    const Token& name,
    TypeInfo type,
    bool explicitType,
    const DeclarationRecord* record)
{
    auto& scope = currentScope();
    if (scope.find(name.lexeme) != scope.end()) {
        throw TypeError(name, "variable `" + name.lexeme + "` already declared in this scope");
    }

    Binding binding;
    binding.type = std::move(type);
    binding.resolvedName = makeResolvedName(name.lexeme);
    binding.scopeDepth = scopes_.size() - 1;
    binding.functionDepth = functionDepth_;
    binding.explicitType = explicitType;
    binding.imported = false;
    binding.bindingId = BindingId{nextBindingId_++};
    if (record) {
        binding.declarationId = record->declarationId;
        binding.symbolId = record->symbolId;
    } else {
        binding.declarationId = DeclarationId{};
        binding.symbolId = SymbolId{};
    }
    binding.range = name.range;
    scope.emplace(name.lexeme, binding);
    if (binding.declarationId.valid()) {
        bindingsById_.emplace(binding.declarationId, &scope.at(name.lexeme));
    }
    return binding;
}

TypeChecker::Binding TypeChecker::declareVariable(
    const LetStmt& statement,
    TypeInfo type,
    bool explicitType)
{
    Binding binding = declareVariable(
        statement.name,
        std::move(type),
        explicitType,
        declarationIndex_.declaration(statement));
    declarationIndex_.recordLetBinding(
        statement,
        BindingMetadataRecord{
            binding.resolvedName,
            binding.bindingId,
            ResolvedSymbol{binding.declarationId, binding.symbolId},
            binding.range});
    return binding;
}

TypeChecker::Binding TypeChecker::declareImportedVariable(const Token& name, const Binding& importedBinding)
{
    auto& scope = currentScope();
    if (scope.find(name.lexeme) != scope.end()) {
        throw TypeError(name, "variable `" + name.lexeme + "` already declared in this scope");
    }

    Binding binding = importedBinding;
    binding.imported = true;
    binding.scopeDepth = scopes_.size() - 1;
    // Imported bindings are new bindings in the importing snapshot. Interface
    // records intentionally drop the exporter's snapshot-local IDs, so assign
    // a fresh local ID here for bytecode lowering.
    binding.bindingId = BindingId{nextBindingId_++};
    scope.emplace(name.lexeme, binding);
    return binding;
}

std::string TypeChecker::makeResolvedName(const std::string& sourceName)
{
    return sourceName + "#" + std::to_string(nextResolvedName_++);
}

void TypeChecker::predeclareStructDeclaration(const StructDeclStmt& statement)
{
    if (structTypes_.find(statement.name.lexeme) != structTypes_.end()) {
        throw TypeError(statement.name, "duplicate struct `" + statement.name.lexeme + "`");
    }

    StructTypeDecl declaration{
        statement.name,
        {},
        typeParameterNames(statement.typeParameters),
        {},
        false,
        std::nullopt};
    if (!moduleStack_.empty()) {
        declaration.definingModuleId = moduleStack_.back();
    }
    structTypes_.emplace(statement.name.lexeme, std::move(declaration));
    structDeclarations_.emplace(statement.name.lexeme, &statement);
    structCheckStates_.emplace(statement.name.lexeme, StructCheckState::Declared);

    if (!moduleStack_.empty()) {
        moduleSymbols_.markLocalStruct(moduleStack_.back(), statement.name.lexeme);
    }
}

void TypeChecker::predeclareStructDeclarations(const std::vector<StmtPtr>& statements)
{
    for (const auto& statement : statements) {
        if (const auto* structDecl = dynamic_cast<const StructDeclStmt*>(statement.get())) {
            predeclareStructDeclaration(*structDecl);
        }
    }

}

void TypeChecker::resolvePredeclaredStructParameters(const std::vector<StmtPtr>& statements)
{
    for (const auto& statement : statements) {
        const auto* structDecl = dynamic_cast<const StructDeclStmt*>(statement.get());
        if (!structDecl) {
            continue;
        }
        beginTypeParameterScope(structDecl->typeParameters);
        std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints
            = typeParameterConstraints(structDecl->typeParameters);
        endTypeParameterScope();
        structTypes_.at(structDecl->name.lexeme).genericParameterConstraints
            = std::move(genericParameterConstraints);
    }
}

void TypeChecker::predeclareEnumDeclarations(const std::vector<StmtPtr>& statements)
{
    for (const auto& statement : statements) {
        if (const auto* enumDecl = dynamic_cast<const EnumDeclStmt*>(statement.get())) {
            if (enumTypes_.find(enumDecl->name.lexeme) != enumTypes_.end()
                || structTypes_.find(enumDecl->name.lexeme) != structTypes_.end()) {
                throw TypeError(enumDecl->name, "duplicate type " + enumDecl->name.lexeme);
            }
            enumTypes_.emplace(
                enumDecl->name.lexeme,
                EnumTypeDecl{
                    enumDecl->name,
                    typeParameterNames(enumDecl->typeParameters),
                    {},
                    {}});
            enumDeclarations_.emplace(enumDecl->name.lexeme, enumDecl);
            enumCheckStates_.emplace(enumDecl->name.lexeme, EnumCheckState::Declared);
            if (!moduleStack_.empty()) {
                moduleSymbols_.markLocalEnum(moduleStack_.back(), enumDecl->name.lexeme);
            }
        }
    }

}

void TypeChecker::resolvePredeclaredEnumParameters(const std::vector<StmtPtr>& statements)
{
    for (const auto& statement : statements) {
        const auto* enumDecl = dynamic_cast<const EnumDeclStmt*>(statement.get());
        if (!enumDecl) {
            continue;
        }
        beginTypeParameterScope(enumDecl->typeParameters);
        std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints
            = typeParameterConstraints(enumDecl->typeParameters);
        endTypeParameterScope();
        enumTypes_.at(enumDecl->name.lexeme).genericParameterConstraints
            = std::move(genericParameterConstraints);
    }
}

void TypeChecker::checkStatementList(const std::vector<StmtPtr>& statements)
{
    predeclareStructDeclarations(statements);
    predeclareEnumDeclarations(statements);
    resolvePredeclaredStructParameters(statements);
    resolvePredeclaredEnumParameters(statements);
    for (const auto& statement : statements) {
        checkStatement(*statement);
    }
}

void TypeChecker::checkStatement(const Stmt& statement)
{
    if (const auto* module = dynamic_cast<const ModuleStmt*>(&statement)) {
        checkModule(*module);
        return;
    }

    if (const auto* import = dynamic_cast<const ImportStmt*>(&statement)) {
        checkImport(*import);
        return;
    }

    if (const auto* exportStmt = dynamic_cast<const ExportStmt*>(&statement)) {
        checkExport(*exportStmt);
        return;
    }

    if (const auto* structDecl = dynamic_cast<const StructDeclStmt*>(&statement)) {
        checkStructDeclaration(*structDecl);
        return;
    }

    if (const auto* enumDecl = dynamic_cast<const EnumDeclStmt*>(&statement)) {
        checkEnumDeclaration(*enumDecl);
        return;
    }

    if (const auto* impl = dynamic_cast<const ImplStmt*>(&statement)) {
        checkImpl(*impl);
        return;
    }

    if (const auto* function = dynamic_cast<const FunctionStmt*>(&statement)) {
        checkFunction(*function);
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&statement)) {
        if (functionDepth_ == 0) {
            throw TypeError(returnStmt->keyword, "return outside function");
        }
        const TypeInfo* expectedReturn = nullptr;
        if (!returnContexts_.empty() && returnContexts_.back().expectedReturnType) {
            expectedReturn = &*returnContexts_.back().expectedReturnType;
        }
        const TypeInfo returned = returnStmt->value
            ? checkExpressionInfo(*returnStmt->value, expectedReturn).type
            : simpleType(StaticType::Nil);
        recordReturn(returnStmt->keyword, returned);
        declarationIndex_.recordReturn(*returnStmt, returned);
        return;
    }

    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&statement)) {
        if (loopDepth_ == 0) {
            throw TypeError(breakStmt->keyword, "`break` can only be used inside a loop");
        }
        return;
    }

    if (const auto* continueStmt = dynamic_cast<const ContinueStmt*>(&statement)) {
        if (loopDepth_ == 0) {
            throw TypeError(continueStmt->keyword, "`continue` can only be used inside a loop");
        }
        return;
    }

    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
        beginScope();
        checkStatementList(block->statements);
        endScope();
        return;
    }

    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
        checkExpression(*ifStmt->condition);
        checkStatement(*ifStmt->thenBranch);
        if (ifStmt->elseBranch) {
            checkStatement(*ifStmt->elseBranch);
        }
        return;
    }

    if (const auto* ifLetStmt = dynamic_cast<const IfLetStmt*>(&statement)) {
        const TypeInfo valueType = checkExpression(*ifLetStmt->value);
        if (SemanticTypes::isKnown(valueType) && !SemanticTypes::isNullable(valueType)) {
            throw TypeError(ifLetStmt->variable,
                "if-let expects an optional value, got " + typeInfoName(valueType));
        }
        const TypeInfo bindingType = SemanticTypes::isNullable(valueType) && valueType.nullableOf
            ? *valueType.nullableOf
            : unknownType();

        beginScope();
        const Binding binding = declareVariable(
            ifLetStmt->variable,
            bindingType,
            false,
            declarationIndex_.declaration(*ifLetStmt));
        declarationIndex_.recordIfLetBinding(
            *ifLetStmt,
            BindingMetadataRecord{
                binding.resolvedName,
                binding.bindingId,
                ResolvedSymbol{binding.declarationId, binding.symbolId},
                binding.range});
        checkStatement(*ifLetStmt->thenBranch);
        endScope();
        if (ifLetStmt->elseBranch) {
            checkStatement(*ifLetStmt->elseBranch);
        }
        return;
    }

    if (const auto* match = dynamic_cast<const MatchStmt*>(&statement)) {
        checkMatch(*match);
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
        checkExpression(*whileStmt->condition);
        ++loopDepth_;
        checkStatement(*whileStmt->body);
        --loopDepth_;
        return;
    }

    if (const auto* whileLetStmt = dynamic_cast<const WhileLetStmt*>(&statement)) {
        const TypeInfo valueType = checkExpression(*whileLetStmt->value);
        if (SemanticTypes::isKnown(valueType) && !SemanticTypes::isNullable(valueType)) {
            throw TypeError(whileLetStmt->variable,
                "while-let expects an optional value, got " + typeInfoName(valueType));
        }
        const TypeInfo bindingType = SemanticTypes::isNullable(valueType) && valueType.nullableOf
            ? *valueType.nullableOf
            : unknownType();

        beginScope();
        const Binding binding = declareVariable(
            whileLetStmt->variable,
            bindingType,
            false,
            declarationIndex_.declaration(*whileLetStmt));
        declarationIndex_.recordWhileLetBinding(
            *whileLetStmt,
            BindingMetadataRecord{
                binding.resolvedName,
                binding.bindingId,
                ResolvedSymbol{binding.declarationId, binding.symbolId},
                binding.range});
        ++loopDepth_;
        checkStatement(*whileLetStmt->body);
        --loopDepth_;
        endScope();
        return;
    }

    if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
        beginScope();
        if (forStmt->initializer) {
            checkStatement(*forStmt->initializer);
        }
        if (forStmt->condition) {
            checkExpression(*forStmt->condition);
        }
        ++loopDepth_;
        checkStatement(*forStmt->body);
        if (forStmt->increment) {
            checkExpression(*forStmt->increment);
        }
        --loopDepth_;
        endScope();
        return;
    }

    if (const auto* forInStmt = dynamic_cast<const ForInStmt*>(&statement)) {
        const TypeInfo iterableType = checkExpression(*forInStmt->iterable);
        if (iterableType.kind != StaticType::Unknown
            && iterableType.kind != StaticType::Array
            && iterableType.kind != StaticType::Range
            && iterableType.kind != StaticType::Map) {
            throw TypeError(forInStmt->variable,
                "for-in expects array, range, or map, got " + typeInfoName(iterableType));
        }

        TypeInfo elementType = unknownType();
        if (iterableType.kind == StaticType::Array && iterableType.elementType) {
            elementType = *iterableType.elementType;
        } else if (iterableType.kind == StaticType::Range) {
            elementType = simpleType(StaticType::Number);
        } else if (iterableType.kind == StaticType::Map && iterableType.keyType) {
            elementType = *iterableType.keyType;
        }

        beginScope();
        const Binding itemBinding = declareVariable(
            forInStmt->variable,
            elementType,
            false,
            declarationIndex_.declaration(*forInStmt));
        declarationIndex_.recordForInBinding(
            *forInStmt,
            BindingMetadataRecord{
                itemBinding.resolvedName,
                itemBinding.bindingId,
                ResolvedSymbol{itemBinding.declarationId, itemBinding.symbolId},
                itemBinding.range});
        ++loopDepth_;
        if (const auto* body = dynamic_cast<const BlockStmt*>(forInStmt->body.get())) {
            for (const auto& bodyStatement : body->statements) {
                checkStatement(*bodyStatement);
            }
        } else {
            checkStatement(*forInStmt->body);
        }
        --loopDepth_;
        endScope();
        return;
    }

    if (const auto* let = dynamic_cast<const LetStmt*>(&statement)) {
        const CheckedExpression declared = checkLetInitializer(*let);
        declareVariable(*let, declared.type, let->typeName.has_value());
        return;
    }

    if (const auto* print = dynamic_cast<const PrintStmt*>(&statement)) {
        checkExpression(*print->expression);
        return;
    }

    if (const auto* expression = dynamic_cast<const ExpressionStmt*>(&statement)) {
        checkExpression(*expression->expression);
        return;
    }

    throw TypeError("unsupported statement node");
}
