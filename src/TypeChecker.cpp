#include "TypeChecker.hpp"

#include "NativeStdlib.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

TypeInfo logicalResultType(const TypeInfo& left, const TypeInfo& right)
{
    if (!SemanticTypes::isKnown(left) || !SemanticTypes::isKnown(right)) {
        return unknownType();
    }
    if (left.kind == right.kind) {
        return left;
    }
    return unknownType();
}

TypeInfo mergeReturnTypes(const TypeInfo& current, const TypeInfo& next)
{
    if (SemanticTypes::compatible(current, next) && SemanticTypes::compatible(next, current) && current.kind == next.kind) {
        if (current.kind != StaticType::Function || typeInfoName(current) == typeInfoName(next)) {
            return current;
        }
    }
    if (!SemanticTypes::isKnown(current) || !SemanticTypes::isKnown(next)) {
        return unknownType();
    }
    return unknownType();
}

TypeInfo copiedArrayType(const TypeInfo& source)
{
    if (source.kind == StaticType::Array && source.elementType) {
        return arrayType(*source.elementType);
    }
    return simpleType(StaticType::Array);
}

TypeInfo concatenatedArrayType(const TypeInfo& left, const TypeInfo& right)
{
    if (left.kind != StaticType::Array || right.kind != StaticType::Array
        || !left.elementType || !right.elementType) {
        return simpleType(StaticType::Array);
    }
    std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*left.elementType, *right.elementType);
    if (!merged) {
        return simpleType(StaticType::Array);
    }
    return arrayType(std::move(*merged));
}

std::optional<std::string> normalizedIntegerLiteral(const Expr& expression)
{
    const auto* literal = dynamic_cast<const LiteralExpr*>(&expression);
    if (!literal || literal->value.empty()
        || !std::all_of(
            literal->value.begin(),
            literal->value.end(),
            [](char value) { return std::isdigit(static_cast<unsigned char>(value)); })) {
        return std::nullopt;
    }

    const std::size_t firstNonZero = literal->value.find_first_not_of('0');
    if (firstNonZero == std::string::npos) {
        return std::string("0");
    }
    return literal->value.substr(firstNonZero);
}

TypeInfo mergedMapType(const TypeInfo& left, const TypeInfo& right)
{
    if (left.kind != StaticType::Map || right.kind != StaticType::Map
        || !left.keyType || !right.keyType || !left.valueType || !right.valueType) {
        return simpleType(StaticType::Map);
    }
    std::optional<TypeInfo> key = SemanticTypes::mergeArrayElementTypes(*left.keyType, *right.keyType);
    std::optional<TypeInfo> value = SemanticTypes::mergeArrayElementTypes(*left.valueType, *right.valueType);
    if (!key || !value) {
        return simpleType(StaticType::Map);
    }
    return mapType(std::move(*key), std::move(*value));
}

bool mapKeyTypeAllowed(const TypeInfo& type)
{
    if (!SemanticTypes::isKnown(type)) {
        return true;
    }
    if (SemanticTypes::isNullable(type)) {
        return type.nullableOf && mapKeyTypeAllowed(*type.nullableOf);
    }
    switch (type.kind) {
    case StaticType::Nil:
    case StaticType::Number:
    case StaticType::Bool:
    case StaticType::String:
    case StaticType::TypeParameter:
        return true;
    default:
        return false;
    }
}

bool isNativeCallbackName(const std::string& name)
{
    return name == "map"
        || name == "filter"
        || name == "flatMap"
        || name == "any"
        || name == "all"
        || name == "count"
        || name == "find"
        || name == "findIndex"
        || name == "reduce";
}

bool isPrimitiveMatchKind(StaticType kind)
{
    return kind == StaticType::Nil
        || kind == StaticType::Number
        || kind == StaticType::Bool
        || kind == StaticType::String;
}

const TypeInfo* primitiveMatchBaseType(const TypeInfo& type)
{
    const TypeInfo* base = SemanticTypes::isNullable(type) ? type.nullableOf.get() : &type;
    if (!base || !isPrimitiveMatchKind(base->kind)) {
        return nullptr;
    }
    return base;
}

TypeInfo literalPatternType(const Token& token)
{
    switch (token.type) {
    case TokenType::Nil:
        return simpleType(StaticType::Nil);
    case TokenType::True:
    case TokenType::False:
        return simpleType(StaticType::Bool);
    case TokenType::Number:
        return simpleType(StaticType::Number);
    case TokenType::String:
        return simpleType(StaticType::String);
    default:
        throw std::logic_error("unsupported literal pattern token");
    }
}

std::string recordPatternTypeName(const RecordPattern& pattern)
{
    if (pattern.qualifier) {
        return pattern.qualifier->lexeme + "." + pattern.name.lexeme;
    }
    return pattern.name.lexeme;
}

std::string unqualifiedStructName(const std::string& name)
{
    const std::size_t separator = name.rfind('.');
    return separator == std::string::npos ? name : name.substr(separator + 1);
}

std::string binaryTypesMessage(const BinaryExpr& expression, const TypeInfo& left, const TypeInfo& right)
{
    return "binary `" + expression.op.lexeme + "` expects numbers, got "
        + typeInfoName(left) + " and " + typeInfoName(right);
}

Token interfaceToken(const Token& anchor, const std::string& lexeme)
{
    Token token = anchor;
    token.type = TokenType::Identifier;
    token.lexeme = lexeme;
    return token;
}

ModuleValueExports valueExportsFromInterface(const ModuleInterface& interfaceInfo)
{
    ModuleValueExports exports;
    for (const ModuleInterfaceValue& value : interfaceInfo.values) {
        TypeBinding binding;
        binding.type = value.type;
        binding.resolvedName = value.resolvedName;
        exports.emplace(value.name, std::move(binding));
    }
    return exports;
}

ModuleStructExports structExportsFromInterface(const ModuleInterface& interfaceInfo, const Token& anchor)
{
    ModuleStructExports exports;
    for (const ModuleInterfaceStruct& source : interfaceInfo.structs) {
        StructTypeDecl declaration;
        declaration.name = interfaceToken(anchor, source.name);
        declaration.genericParameters = source.genericParameters;
        declaration.genericParameterConstraints = source.genericParameterConstraints;
        declaration.hasPrivateFields = source.hasPrivateFields;
        declaration.definingModuleId = source.definingModuleId.value_or(interfaceInfo.moduleId);
        for (const ModuleInterfaceField& field : source.fields) {
            declaration.fields.push_back(StructFieldType{interfaceToken(anchor, field.name), field.type, false});
        }
        exports.emplace(source.name, std::move(declaration));
    }
    return exports;
}

ModuleEnumExports enumExportsFromInterface(const ModuleInterface& interfaceInfo, const Token& anchor)
{
    ModuleEnumExports exports;
    for (const ModuleInterfaceEnum& source : interfaceInfo.enums) {
        EnumTypeDecl declaration;
        declaration.name = interfaceToken(anchor, source.name);
        declaration.genericParameters = source.genericParameters;
        declaration.genericParameterConstraints = source.genericParameterConstraints;
        for (const ModuleInterfaceVariant& variant : source.variants) {
            EnumVariantType converted;
            converted.name = interfaceToken(anchor, variant.name);
            converted.payloadTypes = variant.payloadTypes;
            for (const std::optional<std::string>& payloadName : variant.payloadNames) {
                converted.payloadNames.push_back(
                    payloadName ? std::optional<Token>(interfaceToken(anchor, *payloadName)) : std::nullopt);
            }
            declaration.variants.push_back(std::move(converted));
        }
        exports.emplace(source.name, std::move(declaration));
    }
    return exports;
}

ModuleMethodExports methodExportsFromInterface(const ModuleInterface& interfaceInfo)
{
    ModuleMethodExports exports;
    for (const ModuleInterfaceStruct& structInfo : interfaceInfo.structs) {
        for (const ModuleInterfaceMethod& method : structInfo.methods) {
            exports[structInfo.name].emplace(
                method.name,
                MethodSignature{
                    method.receiverType,
                    method.parameterTypes,
                    method.returnType,
                    method.resolvedName,
                    method.genericParameters,
                    method.genericParameterConstraints});
        }
    }
    return exports;
}

} // namespace

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

TypeErrorList::TypeErrorList(std::vector<FileDiagnosticError> errors)
    : errors_(std::move(errors))
{
}

const std::vector<FileDiagnosticError>& TypeErrorList::errors() const
{
    return errors_;
}

const char* TypeErrorList::what() const noexcept
{
    return "type errors";
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
    scopeIds_.clear();
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
    nextDeclarationId_ = 0;
    nextSymbolId_ = 0;
    nextScopeId_ = 0;
    functionDepth_ = 0;
    loopDepth_ = 0;
    returnContexts_.clear();
    flowFacts_.clear();

    bool hasModules = false;
    for (const auto& statement : program.statements) {
        if (dynamic_cast<const ModuleStmt*>(statement.get())) {
            hasModules = true;
            break;
        }
    }

    if (hasModules) {
        if (program.moduleGraph) {
            checkModulesInDependencyOrder(program);
            for (const auto& statement : program.statements) {
                if (!dynamic_cast<const ModuleStmt*>(statement.get())) {
                    checkStatement(*statement);
                }
            }
        } else {
            for (const auto& statement : program.statements) {
                if (const auto* module = dynamic_cast<const ModuleStmt*>(statement.get())) {
                    checkModule(*module);
                } else {
                    checkStatement(*statement);
                }
            }
        }
    } else {
        beginScope();
        checkStatementList(program.statements);
        endScope();
    }

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
    scopeIds_.push_back(ScopeId{nextScopeId_++});
}

void TypeChecker::endScope()
{
    if (scopes_.empty()) {
        throw TypeError("scope stack is empty");
    }
    scopes_.pop_back();
    if (scopeIds_.empty()) {
        throw TypeError("scope ID stack is empty");
    }
    scopeIds_.pop_back();
}

ScopeId TypeChecker::currentScopeId() const
{
    if (scopeIds_.empty()) {
        throw TypeError("scope ID stack is empty");
    }
    return scopeIds_.back();
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
        if (!parameter.constraint) {
            continue;
        }
        TypeInfo constraint = resolveTypeParameterConstraint(*parameter.constraint);
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

const TypeChecker::Binding* TypeChecker::findBinding(DeclarationId declarationId) const
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        for (const auto& entry : *scope) {
            if (entry.second.declarationId == declarationId) {
                return &entry.second;
            }
        }
    }
    return nullptr;
}

TypeChecker::Binding* TypeChecker::findSimpleVariableBinding(const Expr& expression)
{
    const auto* variable = dynamic_cast<const VariableExpr*>(&expression);
    if (!variable) {
        return nullptr;
    }
    return findVariable(variable->name.lexeme);
}

const TypeChecker::Binding* TypeChecker::findSimpleVariableBinding(const Expr& expression) const
{
    const auto* variable = dynamic_cast<const VariableExpr*>(&expression);
    if (!variable) {
        return nullptr;
    }
    return findVariable(variable->name.lexeme);
}

TypeChecker::Binding TypeChecker::declareVariable(
    const Token& name,
    TypeInfo type,
    bool explicitType)
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
    binding.declarationId = DeclarationId{nextDeclarationId_++};
    binding.symbolId = SymbolId{nextSymbolId_++};
    binding.scopeId = currentScopeId();
    binding.range = name.range;
    scope.emplace(name.lexeme, binding);
    return binding;
}

TypeChecker::Binding TypeChecker::declareVariable(
    const LetStmt& statement,
    TypeInfo type,
    bool explicitType)
{
    Binding binding = declareVariable(statement.name, std::move(type), explicitType);
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
    static_cast<void>(nextDeclarationId_++);
    static_cast<void>(nextSymbolId_++);

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
            static_cast<void>(nextDeclarationId_++);
            static_cast<void>(nextSymbolId_++);
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
        const BranchFlowFacts branchFacts = flowFacts_.factsForIfConditionTargets(
            *ifStmt->condition,
            [this](const Expr& target) {
                return nonNilNarrowingForTarget(target);
            });
        if (!ifStmt->elseBranch && !statementMayFallThrough(*ifStmt->thenBranch)) {
            const std::vector<FlowNarrowing> baseFacts = flowFacts_.activeNarrowings();
            std::vector<FlowNarrowing> terminatingBranchFacts = baseFacts;
            terminatingBranchFacts.insert(
                terminatingBranchFacts.end(),
                branchFacts.thenNarrowings.begin(),
                branchFacts.thenNarrowings.end());
            flowFacts_.withoutNarrowings([&]() {
                flowFacts_.withNarrowings(terminatingBranchFacts, [&]() {
                    checkStatement(*ifStmt->thenBranch);
                });
            });

            flowFacts_.appendNarrowings(branchFacts.elseNarrowings);
            return;
        }
        if (ifStmt->elseBranch) {
            const bool thenMayFallThrough = statementMayFallThrough(*ifStmt->thenBranch);
            const bool elseMayFallThrough = statementMayFallThrough(*ifStmt->elseBranch);
            const std::vector<FlowNarrowing> baseFacts = flowFacts_.activeNarrowings();
            const auto checkBranchInIsolation = [&](const Stmt& branch,
                                                    const std::vector<FlowNarrowing>& branchNarrowings) {
                std::vector<FlowNarrowing> result;
                std::vector<FlowNarrowing> branchFactsWithBase = baseFacts;
                branchFactsWithBase.insert(
                    branchFactsWithBase.end(),
                    branchNarrowings.begin(),
                    branchNarrowings.end());
                flowFacts_.withoutNarrowings([&]() {
                    flowFacts_.withNarrowings(branchFactsWithBase, [&]() {
                        checkStatement(branch);
                        result = flowFacts_.activeNarrowings();
                    });
                });
                return result;
            };

            const std::vector<FlowNarrowing> thenResult
                = checkBranchInIsolation(*ifStmt->thenBranch, branchFacts.thenNarrowings);
            const std::vector<FlowNarrowing> elseResult
                = checkBranchInIsolation(*ifStmt->elseBranch, branchFacts.elseNarrowings);

            if (thenMayFallThrough) {
                if (elseMayFallThrough) {
                    std::vector<FlowNarrowing> joinedFacts;
                    for (const FlowNarrowing& candidate : thenResult) {
                        const auto matching = std::find_if(
                            elseResult.begin(),
                            elseResult.end(),
                            [&candidate](const FlowNarrowing& other) {
                                return candidate.resolvedName == other.resolvedName
                                    && candidate.type.kind == other.type.kind
                                    && SemanticTypes::compatible(candidate.type, other.type)
                                    && SemanticTypes::compatible(other.type, candidate.type);
                            });
                        if (matching == elseResult.end()) {
                            continue;
                        }
                        const auto alreadyJoined = std::find_if(
                            joinedFacts.begin(),
                            joinedFacts.end(),
                            [&candidate](const FlowNarrowing& other) {
                                return candidate.resolvedName == other.resolvedName;
                            });
                        if (alreadyJoined == joinedFacts.end()) {
                            joinedFacts.push_back(candidate);
                        }
                    }
                    flowFacts_.clear();
                    flowFacts_.appendNarrowings(joinedFacts);
                } else {
                    flowFacts_.clear();
                    flowFacts_.appendNarrowings(thenResult);
                }
            } else if (elseMayFallThrough) {
                flowFacts_.clear();
                flowFacts_.appendNarrowings(elseResult);
            }
            return;
        }
        flowFacts_.withNarrowings(branchFacts.thenNarrowings, [&]() {
            checkStatement(*ifStmt->thenBranch);
        });
        return;
    }

    if (const auto* match = dynamic_cast<const MatchStmt*>(&statement)) {
        checkMatch(*match);
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
        checkExpression(*whileStmt->condition);
        const BranchFlowFacts branchFacts = flowFacts_.factsForIfConditionTargets(
            *whileStmt->condition,
            [this](const Expr& target) {
                return nonNilNarrowingForTarget(target);
            });
        const bool containsCurrentLoopBreak = statementContainsBreakForCurrentLoop(*whileStmt->body);
        ++loopDepth_;
        flowFacts_.withNarrowings(branchFacts.thenNarrowings, [&]() {
            checkStatement(*whileStmt->body);
        });
        --loopDepth_;
        if (!containsCurrentLoopBreak) {
            flowFacts_.appendNarrowings(branchFacts.elseNarrowings);
        }
        return;
    }

    if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
        beginScope();
        BranchFlowFacts branchFacts;
        if (forStmt->initializer) {
            checkStatement(*forStmt->initializer);
        }
        if (forStmt->condition) {
            checkExpression(*forStmt->condition);
            branchFacts = flowFacts_.factsForIfConditionTargets(
                *forStmt->condition,
                [this](const Expr& target) {
                    return nonNilNarrowingForTarget(target);
                });
        }
        const bool containsCurrentLoopBreak = statementContainsBreakForCurrentLoop(*forStmt->body);
        ++loopDepth_;
        flowFacts_.withNarrowings(branchFacts.thenNarrowings, [&]() {
            checkStatement(*forStmt->body);
            if (forStmt->increment) {
                checkExpression(*forStmt->increment);
            }
        });
        --loopDepth_;
        if (!containsCurrentLoopBreak) {
            flowFacts_.appendNarrowings(branchFacts.elseNarrowings);
        }
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
        const Binding itemBinding = declareVariable(forInStmt->variable, elementType, false);
        declarationIndex_.recordForInBinding(
            *forInStmt,
            BindingMetadataRecord{
                itemBinding.resolvedName,
                itemBinding.bindingId,
                ResolvedSymbol{itemBinding.declarationId, itemBinding.symbolId},
                itemBinding.range});
        ++loopDepth_;
        flowFacts_.withLoopBody([&]() {
            if (const auto* body = dynamic_cast<const BlockStmt*>(forInStmt->body.get())) {
                for (const auto& bodyStatement : body->statements) {
                    checkStatement(*bodyStatement);
                }
            } else {
                checkStatement(*forInStmt->body);
            }
        });
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

const ModuleStmt* TypeChecker::findModule(const Program& program, std::size_t moduleId) const
{
    for (const auto& statement : program.statements) {
        if (const auto* module = dynamic_cast<const ModuleStmt*>(statement.get())) {
            if (module->moduleId == moduleId) {
                return module;
            }
        }
    }
    return nullptr;
}

const ModuleInterface* TypeChecker::findModuleInterface(std::size_t moduleId) const
{
    const auto found = std::find_if(
        moduleInterfaces_.begin(),
        moduleInterfaces_.end(),
        [moduleId](const ModuleInterface& interfaceInfo) {
            return interfaceInfo.moduleId == moduleId;
        });
    return found == moduleInterfaces_.end() ? nullptr : &*found;
}


void TypeChecker::buildModuleInterface(const Program& program, const ModuleStmt& module)
{
    ModuleInterface interfaceInfo;
    interfaceInfo.moduleId = module.moduleId;
    interfaceInfo.sourceId = module.sourceId;
    interfaceInfo.path = module.path;
    interfaceInfo.isEntry = module.isEntry;
    interfaceInfo.resolvedNameNext = nextResolvedName_;
    if (program.moduleGraph) {
        const auto graphNode = std::find_if(
            program.moduleGraph->nodes.begin(),
            program.moduleGraph->nodes.end(),
            [&module](const ModuleGraphNode& node) { return node.moduleId == module.moduleId; });
        if (graphNode != program.moduleGraph->nodes.end()) {
            interfaceInfo.sourceId = graphNode->sourceId;
            interfaceInfo.canonicalPath = graphNode->canonicalPath;
        }
        for (const ModuleGraphEdge& edge : program.moduleGraph->edges) {
            if (edge.importingModuleId == module.moduleId) {
                interfaceInfo.dependencies.push_back(ModuleInterfaceDependency{
                    edge.importedModuleId,
                    edge.kind,
                    edge.requestedPath});
            }
        }
    }

    if (const ModuleValueExports* exports = moduleSymbols_.valueExports(module.moduleId)) {
        for (const auto& entry : *exports) {
            interfaceInfo.values.push_back(ModuleInterfaceValue{
                entry.first,
                entry.second.type,
                entry.second.resolvedName});
        }
    }

    if (const ModuleStructExports* structExports = moduleSymbols_.structExports(module.moduleId)) {
        for (const auto& entry : *structExports) {
            ModuleInterfaceStruct structInfo;
            structInfo.name = entry.first;
            structInfo.genericParameters = entry.second.genericParameters;
            structInfo.genericParameterConstraints = entry.second.genericParameterConstraints;
            structInfo.hasPrivateFields = entry.second.hasPrivateFields;
            structInfo.definingModuleId = entry.second.definingModuleId.value_or(module.moduleId);
            for (const StructFieldType& field : entry.second.fields) {
                if (field.isPrivate) {
                    continue;
                }
                structInfo.fields.push_back(ModuleInterfaceField{field.name.lexeme, field.type});
            }

            if (const ModuleMethodExports* methodExports = moduleSymbols_.methodExports(module.moduleId)) {
                const auto methodsForStruct = methodExports->find(entry.first);
                if (methodsForStruct != methodExports->end()) {
                    for (const auto& methodEntry : methodsForStruct->second) {
                        structInfo.methods.push_back(ModuleInterfaceMethod{
                            methodEntry.first,
                            methodEntry.second.parameterTypes,
                            methodEntry.second.returnType,
                            methodEntry.second.genericParameters,
                            methodEntry.second.genericParameterConstraints,
                            methodEntry.second.receiverType,
                            methodEntry.second.resolvedName});
                    }
                }
            }

            interfaceInfo.structs.push_back(std::move(structInfo));
        }
    }

    if (const ModuleEnumExports* enumExports = moduleSymbols_.enumExports(module.moduleId)) {
        for (const auto& entry : *enumExports) {
            ModuleInterfaceEnum enumInfo;
            enumInfo.name = entry.first;
            enumInfo.genericParameters = entry.second.genericParameters;
            for (const EnumVariantType& variant : entry.second.variants) {
                std::vector<std::optional<std::string>> payloadNames;
                payloadNames.reserve(variant.payloadNames.size());
                for (const std::optional<Token>& payloadName : variant.payloadNames) {
                    payloadNames.push_back(payloadName
                        ? std::optional<std::string>(payloadName->lexeme)
                        : std::nullopt);
                }
                enumInfo.variants.push_back(ModuleInterfaceVariant{
                    variant.name.lexeme,
                    variant.payloadTypes,
                    std::move(payloadNames)});
            }
            enumInfo.genericParameterConstraints = entry.second.genericParameterConstraints;
            interfaceInfo.enums.push_back(std::move(enumInfo));
        }
    }

    std::sort(
        interfaceInfo.values.begin(),
        interfaceInfo.values.end(),
        [](const ModuleInterfaceValue& left, const ModuleInterfaceValue& right) {
            return left.name < right.name;
        });
    std::sort(
        interfaceInfo.structs.begin(),
        interfaceInfo.structs.end(),
        [](const ModuleInterfaceStruct& left, const ModuleInterfaceStruct& right) {
            return left.name < right.name;
        });
    for (ModuleInterfaceStruct& structInfo : interfaceInfo.structs) {
        std::sort(
            structInfo.methods.begin(),
            structInfo.methods.end(),
            [](const ModuleInterfaceMethod& left, const ModuleInterfaceMethod& right) {
                return left.name < right.name;
            });
    }
    std::sort(
        interfaceInfo.enums.begin(),
        interfaceInfo.enums.end(),
        [](const ModuleInterfaceEnum& left, const ModuleInterfaceEnum& right) {
            return left.name < right.name;
        });

    const auto existing = std::find_if(
        moduleInterfaces_.begin(),
        moduleInterfaces_.end(),
        [&module](const ModuleInterface& current) {
            return current.moduleId == module.moduleId;
        });
    if (existing == moduleInterfaces_.end()) {
        moduleInterfaces_.push_back(std::move(interfaceInfo));
    } else {
        *existing = std::move(interfaceInfo);
    }
}

void TypeChecker::buildModuleInterfaces(const Program& program)
{
    for (const auto& statement : program.statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (module && !findModuleInterface(module->moduleId)) {
            buildModuleInterface(program, *module);
        }
    }

    std::sort(
        moduleInterfaces_.begin(),
        moduleInterfaces_.end(),
        [](const ModuleInterface& left, const ModuleInterface& right) {
            return left.moduleId < right.moduleId;
        });
}

std::size_t TypeChecker::validateModuleInterfaces(const Program& program) const
{
    std::size_t mismatches = 0;
    const auto mismatch = [&mismatches]() { ++mismatches; };

    std::unordered_set<std::size_t> expectedModuleIds;
    for (const auto& statement : program.statements) {
        if (const auto* module = dynamic_cast<const ModuleStmt*>(statement.get())) {
            if (!expectedModuleIds.insert(module->moduleId).second) {
                mismatch();
            }
        }
    }

    if (moduleInterfaces_.size() != expectedModuleIds.size()) {
        mismatch();
    }
    for (std::size_t index = 1; index < moduleInterfaces_.size(); ++index) {
        if (moduleInterfaces_[index - 1].moduleId >= moduleInterfaces_[index].moduleId) {
            mismatch();
        }
    }

    if (program.moduleGraph) {
        if (program.moduleGraph->nodes.size() != expectedModuleIds.size()) {
            mismatch();
        }
        std::unordered_set<std::size_t> graphModuleIds;
        for (const ModuleGraphNode& node : program.moduleGraph->nodes) {
            if (!graphModuleIds.insert(node.moduleId).second
                || !expectedModuleIds.count(node.moduleId)) {
                mismatch();
            }
        }
    }

    const auto checkCanonicalNames = [&mismatch](const auto& records, const auto& nameOf) {
        for (std::size_t index = 1; index < records.size(); ++index) {
            if (nameOf(records[index - 1]) >= nameOf(records[index])) {
                mismatch();
            }
        }
    };

    const auto checkValueNames = [&mismatch](
        const std::vector<ModuleInterfaceValue>& actual,
        const ModuleValueExports* expected) {
        if (!expected) {
            if (!actual.empty()) {
                mismatch();
            }
            return;
        }
        if (actual.size() != expected->size()) {
            mismatch();
        }
        for (const ModuleInterfaceValue& value : actual) {
            if (expected->find(value.name) == expected->end()) {
                mismatch();
            }
        }
    };

    const auto checkStructNames = [&mismatch](
        const std::vector<ModuleInterfaceStruct>& actual,
        const ModuleStructExports* expected) {
        if (!expected) {
            if (!actual.empty()) {
                mismatch();
            }
            return;
        }
        if (actual.size() != expected->size()) {
            mismatch();
        }
        for (const ModuleInterfaceStruct& structure : actual) {
            if (expected->find(structure.name) == expected->end()) {
                mismatch();
            }
        }
    };

    const auto checkStructFieldVisibility = [&mismatch](
        const std::vector<ModuleInterfaceStruct>& actual,
        const ModuleStructExports* expected) {
        if (!expected) {
            return;
        }
        for (const ModuleInterfaceStruct& structure : actual) {
            const auto expectedStructure = expected->find(structure.name);
            if (expectedStructure == expected->end()) {
                continue;
            }
            const StructTypeDecl& declaration = expectedStructure->second;
            if (structure.hasPrivateFields != declaration.hasPrivateFields) {
                mismatch();
            }

            std::size_t publicFieldCount = 0;
            for (const StructFieldType& field : declaration.fields) {
                if (!field.isPrivate) {
                    ++publicFieldCount;
                }
            }
            if (structure.fields.size() != publicFieldCount) {
                mismatch();
            }
            for (const ModuleInterfaceField& field : structure.fields) {
                const auto expectedField = std::find_if(
                    declaration.fields.begin(),
                    declaration.fields.end(),
                    [&field](const StructFieldType& candidate) {
                        return candidate.name.lexeme == field.name;
                    });
                if (expectedField == declaration.fields.end() || expectedField->isPrivate) {
                    mismatch();
                }
            }
        }
    };

    const auto checkEnumNames = [&mismatch](
        const std::vector<ModuleInterfaceEnum>& actual,
        const ModuleEnumExports* expected) {
        if (!expected) {
            if (!actual.empty()) {
                mismatch();
            }
            return;
        }
        if (actual.size() != expected->size()) {
            mismatch();
        }
        for (const ModuleInterfaceEnum& enumeration : actual) {
            if (expected->find(enumeration.name) == expected->end()) {
                mismatch();
            }
        }
    };

    const auto checkMethodNames = [&mismatch](
        const std::vector<ModuleInterfaceStruct>& actual,
        const ModuleMethodExports* expected) {
        for (const ModuleInterfaceStruct& structure : actual) {
            const StructMethodTable* expectedMethods = nullptr;
            if (expected) {
                const auto found = expected->find(structure.name);
                if (found != expected->end()) {
                    expectedMethods = &found->second;
                }
            }
            if (!expectedMethods) {
                if (!structure.methods.empty()) {
                    mismatch();
                }
                continue;
            }
            if (structure.methods.size() != expectedMethods->size()) {
                mismatch();
            }
            for (const ModuleInterfaceMethod& method : structure.methods) {
                if (expectedMethods->find(method.name) == expectedMethods->end()) {
                    mismatch();
                }
            }
        }
        if (!expected) {
            return;
        }
        for (const auto& entry : *expected) {
            const auto structure = std::find_if(
                actual.begin(),
                actual.end(),
                [&entry](const ModuleInterfaceStruct& candidate) {
                    return candidate.name == entry.first;
                });
            if (structure == actual.end()) {
                mismatch();
            }
        }
    };

    for (const ModuleInterface& interfaceInfo : moduleInterfaces_) {
        if (!expectedModuleIds.count(interfaceInfo.moduleId)) {
            mismatch();
            continue;
        }

        const ModuleStmt* module = findModule(program, interfaceInfo.moduleId);
        if (!module) {
            mismatch();
            continue;
        }
        if (interfaceInfo.sourceId != module->sourceId
            || interfaceInfo.path != module->path
            || interfaceInfo.isEntry != module->isEntry) {
            mismatch();
        }

        if (program.moduleGraph) {
            const auto graphNode = std::find_if(
                program.moduleGraph->nodes.begin(),
                program.moduleGraph->nodes.end(),
                [&interfaceInfo](const ModuleGraphNode& node) {
                    return node.moduleId == interfaceInfo.moduleId;
                });
            if (graphNode == program.moduleGraph->nodes.end()) {
                mismatch();
            } else if (interfaceInfo.sourceId != graphNode->sourceId
                || interfaceInfo.path != graphNode->path
                || interfaceInfo.canonicalPath != graphNode->canonicalPath
                || interfaceInfo.isEntry != graphNode->isEntry) {
                mismatch();
            }

            std::vector<const ModuleGraphEdge*> expectedEdges;
            for (const ModuleGraphEdge& edge : program.moduleGraph->edges) {
                if (edge.importingModuleId == interfaceInfo.moduleId) {
                    expectedEdges.push_back(&edge);
                }
            }
            if (interfaceInfo.dependencies.size() != expectedEdges.size()) {
                mismatch();
            }
            const std::size_t edgeCount = std::min(
                interfaceInfo.dependencies.size(),
                expectedEdges.size());
            for (std::size_t index = 0; index < edgeCount; ++index) {
                const ModuleInterfaceDependency& actual = interfaceInfo.dependencies[index];
                const ModuleGraphEdge& expected = *expectedEdges[index];
                if (actual.importedModuleId != expected.importedModuleId
                    || actual.kind != expected.kind
                    || actual.requestedPath != expected.requestedPath) {
                    mismatch();
                }
            }
        } else if (!interfaceInfo.canonicalPath.empty() || !interfaceInfo.dependencies.empty()) {
            mismatch();
        }

        checkCanonicalNames(
            interfaceInfo.values,
            [](const ModuleInterfaceValue& value) { return value.name; });
        checkCanonicalNames(
            interfaceInfo.structs,
            [](const ModuleInterfaceStruct& structure) { return structure.name; });
        checkCanonicalNames(
            interfaceInfo.enums,
            [](const ModuleInterfaceEnum& enumeration) { return enumeration.name; });
        for (const ModuleInterfaceStruct& structure : interfaceInfo.structs) {
            checkCanonicalNames(
                structure.methods,
                [](const ModuleInterfaceMethod& method) { return method.name; });
        }

        if (preloadedModuleIds_.find(interfaceInfo.moduleId) != preloadedModuleIds_.end()) {
            continue;
        }

        checkValueNames(interfaceInfo.values, moduleSymbols_.valueExports(interfaceInfo.moduleId));
        checkStructNames(interfaceInfo.structs, moduleSymbols_.structExports(interfaceInfo.moduleId));
        checkStructFieldVisibility(
            interfaceInfo.structs,
            moduleSymbols_.structExports(interfaceInfo.moduleId));
        checkEnumNames(interfaceInfo.enums, moduleSymbols_.enumExports(interfaceInfo.moduleId));
        checkMethodNames(interfaceInfo.structs, moduleSymbols_.methodExports(interfaceInfo.moduleId));
    }

    return mismatches;
}

void TypeChecker::checkModule(const ModuleStmt& module)
{
    if (checkedModules_.find(module.moduleId) != checkedModules_.end()) {
        return;
    }
    if (preloadedModuleIds_.find(module.moduleId) != preloadedModuleIds_.end()) {
        throw TypeError("internal error: preloaded module entered body-check path");
    }

    std::vector<Scope> savedScopes = std::move(scopes_);
    std::vector<ScopeId> savedScopeIds = std::move(scopeIds_);
    std::unordered_map<std::string, StructTypeDecl> savedStructTypes = std::move(structTypes_);
    std::unordered_map<std::string, const StructDeclStmt*> savedStructDeclarations = std::move(structDeclarations_);
    std::unordered_map<std::string, StructCheckState> savedStructCheckStates = std::move(structCheckStates_);
    std::unordered_map<std::string, EnumTypeDecl> savedEnumTypes = std::move(enumTypes_);
    std::unordered_map<std::string, const EnumDeclStmt*> savedEnumDeclarations = std::move(enumDeclarations_);
    std::unordered_map<std::string, EnumCheckState> savedEnumCheckStates = std::move(enumCheckStates_);
    MethodTable savedMethods = std::move(methods_);
    std::vector<std::unordered_map<std::string, TypeInfo>> savedTypeParameterScopes = std::move(typeParameterScopes_);
    const std::size_t savedFunctionDepth = functionDepth_;
    const std::size_t savedLoopDepth = loopDepth_;
    std::vector<FunctionReturnContext> savedReturnContexts = std::move(returnContexts_);
    ModuleSymbols savedModuleSymbols = moduleSymbols_;
    const FlowFacts savedFlowFacts = flowFacts_;
    const std::vector<std::size_t> savedModuleStack = moduleStack_;

    const auto restoreTransientState = [&]() {
        scopes_ = std::move(savedScopes);
        scopeIds_ = std::move(savedScopeIds);
        structTypes_ = std::move(savedStructTypes);
        structDeclarations_ = std::move(savedStructDeclarations);
        structCheckStates_ = std::move(savedStructCheckStates);
        enumTypes_ = std::move(savedEnumTypes);
        enumDeclarations_ = std::move(savedEnumDeclarations);
        enumCheckStates_ = std::move(savedEnumCheckStates);
        methods_ = std::move(savedMethods);
        typeParameterScopes_ = std::move(savedTypeParameterScopes);
        functionDepth_ = savedFunctionDepth;
        loopDepth_ = savedLoopDepth;
        returnContexts_ = std::move(savedReturnContexts);
        flowFacts_ = savedFlowFacts;
        moduleStack_ = savedModuleStack;
    };
    const auto restoreFailedState = [&]() {
        restoreTransientState();
        moduleSymbols_ = std::move(savedModuleSymbols);
    };

    scopes_.clear();
    scopeIds_.clear();
    structTypes_.clear();
    structDeclarations_.clear();
    structCheckStates_.clear();
    enumTypes_.clear();
    enumDeclarations_.clear();
    enumCheckStates_.clear();
    methods_.clear();
    typeParameterScopes_.clear();
    functionDepth_ = 0;
    loopDepth_ = 0;
    returnContexts_.clear();
    flowFacts_.clear();

    moduleStack_.push_back(module.moduleId);
    beginScope();
    try {
        checkStatementList(module.statements);
        endScope();
        moduleStack_.pop_back();
        buildModuleInterface(*currentProgram_, module);
        checkedModules_.insert(module.moduleId);
        checkedModuleBodyIds_.push_back(module.moduleId);
        restoreTransientState();
    } catch (const FileDiagnosticError&) {
        restoreFailedState();
        throw;
    } catch (const DiagnosticError& error) {
        if (error.location()) {
            FileDiagnosticError contextual(
                error,
                DiagnosticSourceContext{module.path, module.source, false});
            restoreFailedState();
            throw contextual;
        }
        restoreFailedState();
        throw;
    } catch (...) {
        restoreFailedState();
        throw;
    }
}

void TypeChecker::checkModulesInDependencyOrder(const Program& program)
{
    if (!program.moduleGraph) {
        throw TypeError("internal error: module dependency graph is unavailable");
    }

    enum class VisitState {
        Visiting,
        Checked,
        Failed,
        Skipped,
    };
    std::unordered_map<std::size_t, VisitState> states;
    std::vector<FileDiagnosticError> errors;
    const std::function<VisitState(std::size_t)> visit = [&](std::size_t moduleId) -> VisitState {
        const auto existing = states.find(moduleId);
        if (existing != states.end()) {
            if (existing->second == VisitState::Visiting) {
                throw TypeError("internal error: module dependency graph contains a cycle");
            }
            return existing->second;
        }

        states.emplace(moduleId, VisitState::Visiting);
        bool dependencyFailed = false;
        for (const ModuleGraphEdge& edge : program.moduleGraph->edges) {
            if (edge.importingModuleId == moduleId) {
                const VisitState dependencyState = visit(edge.importedModuleId);
                dependencyFailed = dependencyFailed
                    || dependencyState == VisitState::Failed
                    || dependencyState == VisitState::Skipped;
            }
        }

        if (dependencyFailed) {
            states[moduleId] = VisitState::Skipped;
            return VisitState::Skipped;
        }

        if (preloadedModuleIds_.find(moduleId) != preloadedModuleIds_.end()) {
            if (!findModuleInterface(moduleId)) {
                throw TypeError("internal error: preloaded module interface is unavailable");
            }
            // A validated sidecar is the semantic boundary for this module:
            // its public interface is already available, so do not enter the
            // source-body checker or create an empty replacement interface.
            states[moduleId] = VisitState::Checked;
            return VisitState::Checked;
        }

        const ModuleStmt* module = findModule(program, moduleId);
        if (!module) {
            throw TypeError("internal error: unresolved module graph node");
        }

        try {
            checkModule(*module);
        } catch (const FileDiagnosticError& error) {
            if (error.kind() != DiagnosticKind::Type || !error.location()) {
                throw;
            }
            errors.push_back(error);
            states[moduleId] = VisitState::Failed;
            return VisitState::Failed;
        } catch (const DiagnosticError& error) {
            if (error.kind() != DiagnosticKind::Type || !error.location()) {
                throw;
            }
            errors.emplace_back(
                error,
                DiagnosticSourceContext{module->path, module->source, false});
            states[moduleId] = VisitState::Failed;
            return VisitState::Failed;
        }
        states[moduleId] = VisitState::Checked;
        return VisitState::Checked;
    };

    for (const ModuleGraphNode& node : program.moduleGraph->nodes) {
        visit(node.moduleId);
    }

    // Keep the module graph as the scheduling authority while retaining a
    // defensive path for a snapshot whose statement set contains a module
    // that is not yet represented by a graph node.
    for (const auto& statement : program.statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (!module) {
            continue;
        }
        if (states.find(module->moduleId) == states.end()) {
            static_cast<void>(visit(module->moduleId));
        }
    }

    if (!errors.empty()) {
        throw TypeErrorList(std::move(errors));
    }
}

const NamespaceImport* TypeChecker::findNamespace(const std::string& alias) const
{
    if (moduleStack_.empty()) {
        return nullptr;
    }
    return moduleSymbols_.namespaceImport(moduleStack_.back(), alias);
}

std::string TypeChecker::qualifiedStructName(const Token& qualifier, const Token& name) const
{
    return qualifier.lexeme + "." + name.lexeme;
}

std::string TypeChecker::structConstructorTypeName(const StructConstructExpr& expression) const
{
    if (expression.qualifier) {
        return qualifiedStructName(*expression.qualifier, expression.name);
    }
    return expression.name.lexeme;
}

std::string TypeChecker::enumConstructorTypeName(const MemberCallExpr& expression) const
{
    const auto* receiver = dynamic_cast<const VariableExpr*>(expression.receiver.get());
    if (receiver && findEnumType(receiver->name.lexeme)) {
        return receiver->name.lexeme;
    }

    const auto* qualifiedReceiver = dynamic_cast<const FieldAccessExpr*>(expression.receiver.get());
    const auto* namespaceVariable = qualifiedReceiver
        ? dynamic_cast<const VariableExpr*>(qualifiedReceiver->object.get())
        : nullptr;
    if (namespaceVariable) {
        const NamespaceImport* namespaceImport = findNamespace(namespaceVariable->name.lexeme);
        if (namespaceImport
            && namespaceImport->enums.find(qualifiedReceiver->name.lexeme) != namespaceImport->enums.end()) {
            return namespaceVariable->name.lexeme + "." + qualifiedReceiver->name.lexeme;
        }
    }
    return {};
}

void TypeChecker::declareNamespaceAlias(const ImportStmt& statement, NamespaceImport imported)
{
    if (!statement.alias) {
        throw TypeError(statement.keyword, "internal error: namespace import without alias");
    }

    const Token& alias = *statement.alias;
    if (moduleStack_.empty()) {
        throw TypeError(statement.keyword, "namespace imports require a module context");
    }

    const std::size_t moduleId = moduleStack_.back();
    if (moduleSymbols_.hasNamespace(moduleId, alias.lexeme)
        || currentScope().find(alias.lexeme) != currentScope().end()
        || structTypes_.find(alias.lexeme) != structTypes_.end()
        || enumTypes_.find(alias.lexeme) != enumTypes_.end()) {
        throw TypeError(alias, "namespace alias `" + alias.lexeme + "` conflicts with an existing declaration");
    }

    for (const auto& entry : imported.structs) {
        StructTypeDecl qualified = entry.second;
        qualified.name.lexeme = alias.lexeme + "." + entry.first;
        for (std::shared_ptr<TypeInfo>& constraint : qualified.genericParameterConstraints) {
            if (constraint) {
                constraint = std::make_shared<TypeInfo>(
                    qualifyNamespaceType(*constraint, alias.lexeme, imported.structs, imported.enums));
            }
        }
        for (StructFieldType& field : qualified.fields) {
            field.type = qualifyNamespaceType(field.type, alias.lexeme, imported.structs, imported.enums);
        }
        structTypes_.emplace(qualified.name.lexeme, std::move(qualified));
    }

    for (const auto& entry : imported.enums) {
        EnumTypeDecl qualified = entry.second;
        qualified.name.lexeme = alias.lexeme + "." + entry.first;
        for (std::shared_ptr<TypeInfo>& constraint : qualified.genericParameterConstraints) {
            if (constraint) {
                constraint = std::make_shared<TypeInfo>(
                    qualifyNamespaceType(*constraint, alias.lexeme, imported.structs, imported.enums));
            }
        }
        for (EnumVariantType& variant : qualified.variants) {
            for (TypeInfo& payload : variant.payloadTypes) {
                payload = qualifyNamespaceType(payload, alias.lexeme, imported.structs, imported.enums);
            }
        }
        enumTypes_.emplace(qualified.name.lexeme, std::move(qualified));
    }

    for (auto& entry : imported.values) {
        entry.second.type = qualifyNamespaceType(entry.second.type, alias.lexeme, imported.structs, imported.enums);
    }

    importMethodExports(alias, imported.methods, &alias.lexeme, &imported.structs, &imported.enums);
    moduleSymbols_.recordNamespace(moduleId, alias.lexeme, std::move(imported));
}

void TypeChecker::checkImport(const ImportStmt& statement)
{
    if (moduleStack_.empty()) {
        throw TypeError(statement.keyword, "import declarations must be loaded before parsing");
    }

    const std::size_t currentModuleId = moduleStack_.back();
    if (!statement.alias && !moduleSymbols_.markDirectImport(currentModuleId, statement.resolvedModuleId)) {
        return;
    }

    const ModuleInterface* importedInterface = findModuleInterface(statement.resolvedModuleId);
    if (!importedInterface) {
        throw TypeError(statement.keyword, "internal error: unresolved imported module interface");
    }

    if (statement.alias) {
        NamespaceImport namespaceImport;
        namespaceImport.values = valueExportsFromInterface(*importedInterface);
        namespaceImport.structs = structExportsFromInterface(*importedInterface, statement.keyword);
        namespaceImport.enums = enumExportsFromInterface(*importedInterface, statement.keyword);
        namespaceImport.methods = methodExportsFromInterface(*importedInterface);
        declareNamespaceAlias(statement, std::move(namespaceImport));
        return;
    }

    const ModuleValueExports values = valueExportsFromInterface(*importedInterface);
    for (const auto& entry : values) {
        Token name{TokenType::Identifier, entry.first, statement.keyword.line, statement.keyword.column};
        declareImportedVariable(name, entry.second);
    }

    const ModuleStructExports structs = structExportsFromInterface(*importedInterface, statement.keyword);
    for (const auto& entry : structs) {
        if (structTypes_.find(entry.first) != structTypes_.end()) {
            Token name{TokenType::Identifier, entry.first, statement.keyword.line, statement.keyword.column};
            throw TypeError(name, "duplicate struct `" + entry.first + "`");
        }
        structTypes_.emplace(entry.first, entry.second);
    }

    const ModuleEnumExports enums = enumExportsFromInterface(*importedInterface, statement.keyword);
    for (const auto& entry : enums) {
        if (enumTypes_.find(entry.first) != enumTypes_.end()
            || structTypes_.find(entry.first) != structTypes_.end()) {
            Token name{TokenType::Identifier, entry.first, statement.keyword.line, statement.keyword.column};
            throw TypeError(name, "duplicate type " + entry.first);
        }
        enumTypes_.emplace(entry.first, entry.second);
    }

    const ModuleMethodExports methods = methodExportsFromInterface(*importedInterface);
    importMethodExports(statement.keyword, methods);
}

std::string TypeChecker::sourcePathLabel(const Token& path) const
{
    if (path.lexeme.size() >= 2 && path.lexeme.front() == '"' && path.lexeme.back() == '"') {
        return path.lexeme.substr(1, path.lexeme.size() - 2);
    }
    return path.lexeme;
}

void TypeChecker::ensureExportNameAvailable(std::size_t moduleId, const Token& name) const
{
    if (moduleSymbols_.hasAnyExport(moduleId, name.lexeme)) {
        throw TypeError(name, "duplicate export `" + name.lexeme + "`");
    }
}

void TypeChecker::forwardStructMethodExports(
    const ModuleMethodExports& targetExports,
    std::size_t currentModuleId,
    const std::string& structName)
{
    const auto found = targetExports.find(structName);
    if (found == targetExports.end()) {
        return;
    }
    moduleSymbols_.recordMethodExports(currentModuleId, structName, found->second);
}

void TypeChecker::checkReExport(const ExportStmt& statement)
{
    if (moduleStack_.empty()) {
        throw TypeError(statement.keyword, "re-export declarations require a module context");
    }
    if (!currentProgram_) {
        throw TypeError(statement.keyword, "internal error: re-export without program");
    }
    if (!statement.sourcePath) {
        throw TypeError(statement.keyword, "internal error: re-export without source path");
    }

    const std::size_t currentModuleId = moduleStack_.back();
    const ModuleInterface* targetInterface = findModuleInterface(statement.resolvedModuleId);
    if (!targetInterface) {
        throw TypeError(statement.keyword, "internal error: unresolved re-export module interface");
    }

    const ModuleValueExports valueExports = valueExportsFromInterface(*targetInterface);
    const ModuleStructExports structExports = structExportsFromInterface(*targetInterface, statement.keyword);
    const ModuleEnumExports enumExports = enumExportsFromInterface(*targetInterface, statement.keyword);
    const ModuleMethodExports methodExports = methodExportsFromInterface(*targetInterface);

    for (const Token& name : statement.names) {
        ensureExportNameAvailable(currentModuleId, name);

        bool exported = false;
        const auto value = valueExports.find(name.lexeme);
        if (value != valueExports.end()) {
            moduleSymbols_.recordValueExport(currentModuleId, name.lexeme, value->second);
            exported = true;
        }

        const auto structure = structExports.find(name.lexeme);
        if (structure != structExports.end()) {
            moduleSymbols_.recordStructExport(currentModuleId, name.lexeme, structure->second);
            forwardStructMethodExports(methodExports, currentModuleId, name.lexeme);
            exported = true;
        }

        const auto enumeration = enumExports.find(name.lexeme);
        if (enumeration != enumExports.end()) {
            moduleSymbols_.recordEnumExport(currentModuleId, name.lexeme, enumeration->second);
            exported = true;
        }

        if (!exported) {
            throw TypeError(name,
                "module `" + sourcePathLabel(*statement.sourcePath) + "` has no exported name `" + name.lexeme + "`");
        }
    }
}

void TypeChecker::checkExport(const ExportStmt& statement)
{
    if (statement.sourcePath) {
        checkReExport(statement);
        return;
    }

    const bool inModule = !moduleStack_.empty();
    const std::size_t moduleId = inModule ? moduleStack_.back() : 0;

    for (const auto& name : statement.names) {
        bool exported = false;

        if (inModule) {
            ensureExportNameAvailable(moduleId, name);
        }

        if (Binding* binding = findVariable(name.lexeme)) {
            if (binding->scopeDepth == 0 && !binding->imported) {
                if (inModule) {
                    moduleSymbols_.recordValueExport(moduleId, name.lexeme, *binding);
                }
                exported = true;
            }
        }

        if (inModule) {
            if (moduleSymbols_.isLocalStruct(moduleId, name.lexeme)) {
                if (const StructTypeDecl* structType = findStructType(name.lexeme)) {
                    moduleSymbols_.recordStructExport(moduleId, name.lexeme, *structType);
                    recordStructMethodExports(moduleId, name.lexeme);
                    exported = true;
                }
            }
            if (moduleSymbols_.isLocalEnum(moduleId, name.lexeme)) {
                if (const EnumTypeDecl* enumType = findEnumType(name.lexeme)) {
                    moduleSymbols_.recordEnumExport(moduleId, name.lexeme, *enumType);
                    exported = true;
                }
            }
        } else if (findStructType(name.lexeme)) {
            exported = true;
        } else if (findEnumType(name.lexeme)) {
            exported = true;
        }

        if (!exported) {
            throw TypeError(name, "cannot export undefined name `" + name.lexeme + "`");
        }
    }
}

void TypeChecker::recordReturn(const Token& keyword, TypeInfo type)
{
    if (returnContexts_.empty()) {
        throw TypeError("return context stack is empty");
    }

    FunctionReturnContext& context = returnContexts_.back();
    if (context.expectedReturnType && !SemanticTypes::compatible(*context.expectedReturnType, type)) {
        throw TypeError(keyword, "cannot return " + typeInfoName(type)
            + " from function returning " + typeInfoName(*context.expectedReturnType));
    }
    if (!context.expectedReturnType && type.kind == StaticType::Function) {
        type = functionWithoutSignature();
    }

    if (!context.sawReturn) {
        context.sawReturn = true;
        context.returnType = type;
        return;
    }

    context.returnType = mergeReturnTypes(context.returnType, type);
}

bool TypeChecker::bodyMayFallThrough(const std::vector<StmtPtr>& body) const
{
    if (body.empty()) {
        return true;
    }
    return statementMayFallThrough(*body.back());
}

bool TypeChecker::statementMayFallThrough(const Stmt& statement) const
{
    const Stmt& last = statement;
    if (dynamic_cast<const ReturnStmt*>(&last)
        || dynamic_cast<const BreakStmt*>(&last)
        || dynamic_cast<const ContinueStmt*>(&last)) {
        return false;
    }
    if (const auto* block = dynamic_cast<const BlockStmt*>(&last)) {
        return bodyMayFallThrough(block->statements);
    }
    if (const auto* match = dynamic_cast<const MatchStmt*>(&last)) {
        if (match->arms.empty()) {
            return true;
        }
        for (const MatchArm& arm : match->arms) {
            const auto* armBlock = dynamic_cast<const BlockStmt*>(arm.body.get());
            if (!armBlock || bodyMayFallThrough(armBlock->statements)) {
                return true;
            }
        }
        // checkMatch has already enforced exhaustive coverage before function
        // return analysis reaches this helper. Once every arm body returns,
        // guards only affect which arm runs, not whether the match can fall
        // through to the following statement.
        return false;
    }
    return true;
}

bool TypeChecker::statementContainsBreakForCurrentLoop(const Stmt& statement) const
{
    if (dynamic_cast<const BreakStmt*>(&statement)) {
        return true;
    }
    if (dynamic_cast<const WhileStmt*>(&statement)
        || dynamic_cast<const ForStmt*>(&statement)
        || dynamic_cast<const ForInStmt*>(&statement)
        || dynamic_cast<const FunctionStmt*>(&statement)
        || dynamic_cast<const ImplStmt*>(&statement)) {
        return false;
    }
    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
        for (const auto& child : block->statements) {
            if (statementContainsBreakForCurrentLoop(*child)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
        return statementContainsBreakForCurrentLoop(*ifStmt->thenBranch)
            || (ifStmt->elseBranch && statementContainsBreakForCurrentLoop(*ifStmt->elseBranch));
    }
    if (const auto* match = dynamic_cast<const MatchStmt*>(&statement)) {
        for (const MatchArm& arm : match->arms) {
            if (statementContainsBreakForCurrentLoop(*arm.body)) {
                return true;
            }
        }
    }
    return false;
}

void TypeChecker::checkImplicitNilReturn(
    const Token& functionToken,
    const std::string& functionLabel,
    const TypeInfo& expectedReturnType) const
{
    if (!SemanticTypes::compatible(expectedReturnType, simpleType(StaticType::Nil))) {
        throw TypeError(functionToken,
            "function `" + functionLabel + "` may return nil but is annotated " + typeInfoName(expectedReturnType));
    }
}

TypeInfo TypeChecker::checkFunctionBody(
    const std::vector<StmtPtr>& body,
    std::optional<TypeInfo> expectedReturnType,
    const Token& functionToken,
    const std::string& functionLabel)
{
    std::optional<TypeInfo> result;
    flowFacts_.withoutNarrowings([&]() {
        returnContexts_.push_back(FunctionReturnContext{false, simpleType(StaticType::Nil), expectedReturnType});

        for (const auto& child : body) {
            checkStatement(*child);
        }

        const FunctionReturnContext context = returnContexts_.back();
        returnContexts_.pop_back();

        if (expectedReturnType) {
            if (bodyMayFallThrough(body)) {
                checkImplicitNilReturn(functionToken, functionLabel, *expectedReturnType);
            }
            result = *expectedReturnType;
            return;
        }

        result = context.sawReturn ? context.returnType : simpleType(StaticType::Nil);
    });
    return *result;
}

const TypeChecker::StructTypeDecl* TypeChecker::findStructType(const std::string& name) const
{
    const auto found = structTypes_.find(name);
    if (found == structTypes_.end()) {
        return nullptr;
    }
    return &found->second;
}

const TypeChecker::EnumTypeDecl* TypeChecker::findEnumType(const std::string& name) const
{
    const auto found = enumTypes_.find(name);
    if (found == enumTypes_.end()) {
        return nullptr;
    }
    return &found->second;
}

const TypeChecker::EnumVariantType* TypeChecker::findEnumVariant(
    const EnumTypeDecl& enumType,
    const std::string& name) const
{
    for (const EnumVariantType& variant : enumType.variants) {
        if (variant.name.lexeme == name) {
            return &variant;
        }
    }
    return nullptr;
}

TypeInfo TypeChecker::resolveNamedStructAnnotation(
    const TypeAnnotation& typeName,
    std::string structName,
    const StructTypeDecl& structType) const
{
    if (structType.genericParameters.empty()) {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "struct `" + structName + "` is not generic");
        }
        return namedStructType(std::move(structName));
    }

    if (typeName.typeArguments.size() != structType.genericParameters.size()) {
        throw TypeError(typeName.token,
            "struct `" + structName + "` expects "
                + std::to_string(structType.genericParameters.size())
                + " type arguments but got "
                + std::to_string(typeName.typeArguments.size()));
    }

    std::vector<TypeInfo> arguments;
    arguments.reserve(typeName.typeArguments.size());
    for (const TypeAnnotation& argument : typeName.typeArguments) {
        arguments.push_back(resolveAnnotation(argument));
    }

    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints
        = structType.genericParameterConstraints;
    const std::size_t namespaceSeparator = structName.find('.');
    if (namespaceSeparator != std::string::npos) {
        const std::string alias = structName.substr(0, namespaceSeparator);
        if (const NamespaceImport* namespaceImport = findNamespace(alias)) {
            for (std::shared_ptr<TypeInfo>& constraint : genericParameterConstraints) {
                if (constraint) {
                    constraint = std::make_shared<TypeInfo>(
                        qualifyNamespaceType(
                            *constraint,
                            alias,
                            namespaceImport->structs,
                            namespaceImport->enums));
                }
            }
        }
    }

    TypeSubstitutions substitutions;
    for (std::size_t i = 0; i < structType.genericParameters.size(); ++i) {
        substitutions.emplace(structType.genericParameters[i], arguments[i]);
    }
    validateGenericTypeArguments(
        structType.genericParameters,
        genericParameterConstraints,
        substitutions,
        typeName.token,
        "struct " + structName);
    return namedStructType(std::move(structName), std::move(arguments));
}

TypeInfo TypeChecker::resolveNamedEnumAnnotation(
    const TypeAnnotation& typeName,
    std::string enumName,
    const EnumTypeDecl& enumType) const
{
    if (enumType.genericParameters.empty()) {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "enum `" + enumName + "` is not generic");
        }
        return namedEnumType(std::move(enumName));
    }

    if (typeName.typeArguments.size() != enumType.genericParameters.size()) {
        throw TypeError(typeName.token,
            "enum `" + enumName + "` expects "
                + std::to_string(enumType.genericParameters.size())
                + " type arguments but got "
                + std::to_string(typeName.typeArguments.size()));
    }

    std::vector<TypeInfo> arguments;
    arguments.reserve(typeName.typeArguments.size());
    for (const TypeAnnotation& argument : typeName.typeArguments) {
        arguments.push_back(resolveAnnotation(argument));
    }
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints
        = enumType.genericParameterConstraints;
    const std::size_t namespaceSeparator = enumName.find('.');
    if (namespaceSeparator != std::string::npos) {
        const std::string alias = enumName.substr(0, namespaceSeparator);
        if (const NamespaceImport* namespaceImport = findNamespace(alias)) {
            for (std::shared_ptr<TypeInfo>& constraint : genericParameterConstraints) {
                if (constraint) {
                    constraint = std::make_shared<TypeInfo>(
                        qualifyNamespaceType(
                            *constraint,
                            alias,
                            namespaceImport->structs,
                            namespaceImport->enums));
                }
            }
        }
    }

    TypeSubstitutions substitutions;
    for (std::size_t i = 0; i < enumType.genericParameters.size(); ++i) {
        substitutions.emplace(enumType.genericParameters[i], arguments[i]);
    }
    validateGenericTypeArguments(
        enumType.genericParameters,
        genericParameterConstraints,
        substitutions,
        typeName.token,
        "enum " + enumName);
    return namedEnumType(std::move(enumName), std::move(arguments));
}

TypeInfo TypeChecker::resolveStructFieldAnnotation(const StructFieldDecl& field)
{
    return resolveStructFieldAnnotation(field.typeName, field.name);
}

TypeInfo TypeChecker::resolveStructFieldAnnotation(const TypeAnnotation& typeName, const Token& fieldName)
{
    if (typeName.kind == TypeAnnotation::Kind::Nullable) {
        return nullableType(resolveStructFieldAnnotation(*typeName.innerType, fieldName));
    }

    if (typeName.kind == TypeAnnotation::Kind::Array) {
        return arrayType(resolveStructFieldAnnotation(*typeName.elementType, fieldName));
    }

    if (typeName.kind == TypeAnnotation::Kind::Map) {
        TypeInfo keyType = resolveStructFieldAnnotation(*typeName.keyType, fieldName);
        if (!mapKeyTypeAllowed(keyType)) {
            throw TypeError(typeName.token, "map key must be nil, number, bool, or string");
        }
        return mapType(std::move(keyType), resolveStructFieldAnnotation(*typeName.valueType, fieldName));
    }

    if (typeName.kind == TypeAnnotation::Kind::Function) {
        std::vector<TypeInfo> parameterTypes;
        parameterTypes.reserve(typeName.parameterTypes.size());
        for (const TypeAnnotation& parameter : typeName.parameterTypes) {
            parameterTypes.push_back(resolveStructFieldAnnotation(parameter, fieldName));
        }
        return functionType(std::move(parameterTypes), resolveStructFieldAnnotation(*typeName.returnType, fieldName));
    }

    return resolveSimpleStructFieldAnnotation(typeName, fieldName);
}

TypeInfo TypeChecker::resolveSimpleStructFieldAnnotation(const TypeAnnotation& typeName, const Token& fieldName)
{
    if (typeName.kind == TypeAnnotation::Kind::Qualified) {
        return resolveAnnotation(typeName);
    }

    if (typeName.token.lexeme == "number") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `number` is not generic");
        }
        return simpleType(StaticType::Number);
    }
    if (typeName.token.lexeme == "bool") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `bool` is not generic");
        }
        return simpleType(StaticType::Bool);
    }
    if (typeName.token.lexeme == "string") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `string` is not generic");
        }
        return simpleType(StaticType::String);
    }
    if (typeName.token.lexeme == "nil") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `nil` is not generic");
        }
        return simpleType(StaticType::Nil);
    }

    if (const TypeInfo* typeParameter = findTypeParameter(typeName.token.lexeme)) {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token,
                "type parameter `" + typeName.token.lexeme + "` is not generic");
        }
        return *typeParameter;
    }

    const auto state = structCheckStates_.find(typeName.token.lexeme);
    if (state != structCheckStates_.end()) {
        if (state->second == StructCheckState::Checking) {
            throw TypeError(typeName.token,
                "recursive struct field `" + fieldName.lexeme + "` references `" + typeName.token.lexeme + "`");
        }
        if (state->second == StructCheckState::Declared) {
            const auto declaration = structDeclarations_.find(typeName.token.lexeme);
            if (declaration == structDeclarations_.end()) {
                throw TypeError(typeName.token, "unknown type `" + typeName.token.lexeme + "`");
            }
            checkStructDeclaration(*declaration->second);
        }
        const StructTypeDecl* structType = findStructType(typeName.token.lexeme);
        if (!structType) {
            throw TypeError(typeName.token, "unknown type `" + typeName.token.lexeme + "`");
        }
        return resolveNamedStructAnnotation(
            typeName, typeName.token.lexeme, *structType);
    }

    if (const StructTypeDecl* structType = findStructType(typeName.token.lexeme)) {
        return resolveNamedStructAnnotation(
            typeName, typeName.token.lexeme, *structType);
    }
    if (const EnumTypeDecl* enumType = findEnumType(typeName.token.lexeme)) {
        return resolveNamedEnumAnnotation(typeName, typeName.token.lexeme, *enumType);
    }

    throw TypeError(typeName.token, "unknown type `" + typeName.token.lexeme + "`");
}

void TypeChecker::checkStructDeclaration(const StructDeclStmt& statement)
{
    const auto state = structCheckStates_.find(statement.name.lexeme);
    if (state != structCheckStates_.end() && state->second == StructCheckState::Checked) {
        return;
    }
    if (state == structCheckStates_.end()) {
        predeclareStructDeclaration(statement);
    }

    structCheckStates_[statement.name.lexeme] = StructCheckState::Checking;
    beginTypeParameterScope(statement.typeParameters);
    StructTypeDecl declaration{
        statement.name,
        {},
        typeParameterNames(statement.typeParameters),
        typeParameterConstraints(statement.typeParameters),
        false,
        std::nullopt};
    if (!moduleStack_.empty()) {
        declaration.definingModuleId = moduleStack_.back();
    }
    std::unordered_map<std::string, Token> fieldNames;
    for (const StructFieldDecl& field : statement.fields) {
        if (fieldNames.find(field.name.lexeme) != fieldNames.end()) {
            throw TypeError(field.name,
                "duplicate field `" + field.name.lexeme + "` in struct `" + statement.name.lexeme + "`");
        }
        fieldNames.emplace(field.name.lexeme, field.name);
        declaration.hasPrivateFields = declaration.hasPrivateFields || field.isPrivate;
        declaration.fields.push_back(StructFieldType{
            field.name,
            resolveStructFieldAnnotation(field),
            field.isPrivate});
    }

    endTypeParameterScope();
    structTypes_[statement.name.lexeme] = std::move(declaration);
    structCheckStates_[statement.name.lexeme] = StructCheckState::Checked;
}

void TypeChecker::checkEnumDeclaration(const EnumDeclStmt& statement)
{
    const auto state = enumCheckStates_.find(statement.name.lexeme);
    if (state != enumCheckStates_.end() && state->second == EnumCheckState::Checked) {
        return;
    }
    if (state == enumCheckStates_.end()) {
        enumTypes_.emplace(
            statement.name.lexeme,
            EnumTypeDecl{statement.name, typeParameterNames(statement.typeParameters), {}, {}});
        enumDeclarations_.emplace(statement.name.lexeme, &statement);
        enumCheckStates_.emplace(statement.name.lexeme, EnumCheckState::Declared);
    }

    enumCheckStates_[statement.name.lexeme] = EnumCheckState::Checking;
    beginTypeParameterScope(statement.typeParameters);
    EnumTypeDecl declaration{
        statement.name,
        typeParameterNames(statement.typeParameters),
        typeParameterConstraints(statement.typeParameters),
        {}};
    std::unordered_map<std::string, Token> variantNames;
    for (const EnumVariantDecl& variant : statement.variants) {
        if (variantNames.find(variant.name.lexeme) != variantNames.end()) {
            throw TypeError(variant.name,
                "duplicate enum variant " + variant.name.lexeme + " in enum " + statement.name.lexeme);
        }
        variantNames.emplace(variant.name.lexeme, variant.name);

        EnumVariantType checkedVariant{variant.name, {}, {}};
        bool hasNamedPayload = false;
        bool hasPositionalPayload = false;
        std::unordered_set<std::string> payloadNames;
        checkedVariant.payloadNames.resize(variant.payloadTypes.size());
        for (std::size_t i = 0; i < variant.payloadTypes.size(); ++i) {
            if (i < variant.payloadNames.size() && variant.payloadNames[i]) {
                hasNamedPayload = true;
                const Token& payloadName = *variant.payloadNames[i];
                if (!payloadNames.insert(payloadName.lexeme).second) {
                    throw TypeError(payloadName,
                        "duplicate enum payload field " + payloadName.lexeme
                            + " in variant " + statement.name.lexeme + "." + variant.name.lexeme);
                }
                checkedVariant.payloadNames[i] = payloadName;
            } else {
                hasPositionalPayload = true;
            }
        }
        if (hasNamedPayload && hasPositionalPayload) {
            throw TypeError(variant.name,
                "enum variant " + statement.name.lexeme + "." + variant.name.lexeme
                    + " must use either all named or all positional payloads");
        }
        for (const TypeAnnotation& payloadType : variant.payloadTypes) {
            checkedVariant.payloadTypes.push_back(resolveAnnotation(payloadType));
        }
        declaration.variants.push_back(std::move(checkedVariant));
    }

    enumTypes_[statement.name.lexeme] = std::move(declaration);
    endTypeParameterScope();
    enumCheckStates_[statement.name.lexeme] = EnumCheckState::Checked;
}

bool TypeChecker::isBuiltinMemberName(const std::string& name) const
{
    return name == "push" || name == "pop" || name == "remove" || name == "clear" || name == "merge" || name == "keys" || name == "values" || name == "len"
        || name == "substr" || name == "charAt"
        || name == "contains" || name == "slice" || name == "copy" || name == "concat"
        || name == "map" || name == "filter" || name == "flatMap" || name == "any" || name == "all" || name == "count" || name == "find" || name == "findIndex" || name == "reduce";
}

std::vector<TypeInfo> TypeChecker::resolveParameterTypes(const std::vector<Parameter>& parameters)
{
    std::vector<TypeInfo> parameterTypes;
    parameterTypes.reserve(parameters.size());
    for (const Parameter& parameter : parameters) {
        parameterTypes.push_back(parameter.typeName
            ? resolveAnnotation(*parameter.typeName)
            : unknownType());
    }
    return parameterTypes;
}

std::optional<TypeInfo> TypeChecker::resolveOptionalReturnType(const std::optional<TypeAnnotation>& returnTypeName)
{
    if (!returnTypeName) {
        return std::nullopt;
    }
    return resolveAnnotation(*returnTypeName);
}

std::vector<std::string> TypeChecker::typeParameterNames(const std::vector<TypeParameter>& parameters) const
{
    std::vector<std::string> names;
    names.reserve(parameters.size());
    for (const TypeParameter& parameter : parameters) {
        names.push_back(parameter.name.lexeme);
    }
    return names;
}

std::vector<std::shared_ptr<TypeInfo>> TypeChecker::typeParameterConstraints(
    const std::vector<TypeParameter>& parameters) const
{
    std::vector<std::shared_ptr<TypeInfo>> constraints;
    constraints.reserve(parameters.size());
    for (const TypeParameter& parameter : parameters) {
        const TypeInfo* type = findTypeParameter(parameter.name.lexeme);
        if (type && type->typeParameterConstraint) {
            constraints.push_back(std::make_shared<TypeInfo>(*type->typeParameterConstraint));
        } else {
            constraints.push_back(nullptr);
        }
    }
    return constraints;
}

bool TypeChecker::hasEscapingTypeParameter(
    const TypeInfo& type,
    const std::unordered_set<std::string>& allowed) const
{
    if (type.kind == StaticType::TypeParameter && type.typeParameterName) {
        return allowed.find(*type.typeParameterName) == allowed.end();
    }
    if (type.elementType && hasEscapingTypeParameter(*type.elementType, allowed)) {
        return true;
    }
    if (type.keyType && hasEscapingTypeParameter(*type.keyType, allowed)) {
        return true;
    }
    if (type.valueType && hasEscapingTypeParameter(*type.valueType, allowed)) {
        return true;
    }
    if (type.nullableOf && hasEscapingTypeParameter(*type.nullableOf, allowed)) {
        return true;
    }
    if (type.returnType && hasEscapingTypeParameter(*type.returnType, allowed)) {
        return true;
    }
    for (const TypeInfo& parameter : type.parameterTypes) {
        if (hasEscapingTypeParameter(parameter, allowed)) {
            return true;
        }
    }
    for (const TypeInfo& argument : type.typeArguments) {
        if (hasEscapingTypeParameter(argument, allowed)) {
            return true;
        }
    }
    return false;
}

const TypeChecker::MethodInfo* TypeChecker::findMethod(const std::string& structName, const std::string& methodName) const
{
    const auto structFound = methods_.find(structName);
    if (structFound == methods_.end()) {
        return nullptr;
    }
    const auto methodFound = structFound->second.find(methodName);
    return methodFound == structFound->second.end() ? nullptr : &methodFound->second;
}

MethodSignature TypeChecker::methodSignatureFromInfo(const MethodInfo& method) const
{
    MethodSignature signature;
    signature.receiverType = method.receiverType;
    signature.parameterTypes = method.parameterTypes;
    signature.returnType = method.returnType;
    signature.resolvedName = method.resolvedName;
    signature.genericParameters = method.genericParameters;
    signature.genericParameterConstraints = method.genericParameterConstraints;
    return signature;
}

TypeChecker::MethodInfo TypeChecker::methodInfoFromSignature(const MethodSignature& signature) const
{
    MethodInfo info;
    info.receiverType = signature.receiverType;
    info.parameterTypes = signature.parameterTypes;
    info.returnType = signature.returnType;
    info.resolvedName = signature.resolvedName;
    info.genericParameters = signature.genericParameters;
    info.genericParameterConstraints = signature.genericParameterConstraints;
    return info;
}

TypeInfo TypeChecker::qualifyNamespaceType(
    const TypeInfo& type,
    const std::string& alias,
    const ModuleStructExports& structs,
    const ModuleEnumExports& enums) const
{
    TypeInfo result = type;
    if (result.kind == StaticType::TypeParameter && result.typeParameterConstraint) {
        result.typeParameterConstraint = std::make_shared<TypeInfo>(
            qualifyNamespaceType(*result.typeParameterConstraint, alias, structs, enums));
        return result;
    }
    if (result.kind == StaticType::Struct) {
        for (TypeInfo& argument : result.typeArguments) {
            argument = qualifyNamespaceType(argument, alias, structs, enums);
        }
        if (result.structName && structs.find(*result.structName) != structs.end()) {
            result.structName = alias + "." + *result.structName;
        }
        return result;
    }
    if (result.kind == StaticType::Enum && result.enumName && enums.find(*result.enumName) != enums.end()) {
        result.enumName = alias + "." + *result.enumName;
        for (TypeInfo& argument : result.typeArguments) {
            argument = qualifyNamespaceType(argument, alias, structs, enums);
        }
        return result;
    }
    if (result.kind == StaticType::Array && result.elementType) {
        result.elementType = std::make_shared<TypeInfo>(
            qualifyNamespaceType(*result.elementType, alias, structs, enums));
        return result;
    }
    if (result.kind == StaticType::Map) {
        if (result.keyType) {
            result.keyType = std::make_shared<TypeInfo>(
                qualifyNamespaceType(*result.keyType, alias, structs, enums));
        }
        if (result.valueType) {
            result.valueType = std::make_shared<TypeInfo>(
                qualifyNamespaceType(*result.valueType, alias, structs, enums));
        }
        return result;
    }
    if (SemanticTypes::isNullable(result) && result.nullableOf) {
        result.nullableOf = std::make_shared<TypeInfo>(
            qualifyNamespaceType(*result.nullableOf, alias, structs, enums));
        return result;
    }
    if (result.kind == StaticType::Function && result.returnType) {
        for (TypeInfo& parameter : result.parameterTypes) {
            parameter = qualifyNamespaceType(parameter, alias, structs, enums);
        }
        result.returnType = std::make_shared<TypeInfo>(
            qualifyNamespaceType(*result.returnType, alias, structs, enums));
        for (std::shared_ptr<TypeInfo>& constraint : result.genericParameterConstraints) {
            if (constraint) {
                constraint = std::make_shared<TypeInfo>(
                    qualifyNamespaceType(*constraint, alias, structs, enums));
            }
        }
    }
    return result;
}

MethodSignature TypeChecker::qualifyNamespaceMethodSignature(
    const MethodSignature& signature,
    const std::string& alias,
    const ModuleStructExports& structs,
    const ModuleEnumExports& enums) const
{
    MethodSignature result = signature;
    result.receiverType = qualifyNamespaceType(result.receiverType, alias, structs, enums);
    for (TypeInfo& parameter : result.parameterTypes) {
        parameter = qualifyNamespaceType(parameter, alias, structs, enums);
    }
    result.returnType = qualifyNamespaceType(result.returnType, alias, structs, enums);
    for (std::shared_ptr<TypeInfo>& constraint : result.genericParameterConstraints) {
        if (constraint) {
            constraint = std::make_shared<TypeInfo>(
                qualifyNamespaceType(*constraint, alias, structs, enums));
        }
    }
    return result;
}

void TypeChecker::importMethodExports(
    const Token& diagnosticToken,
    const ModuleMethodExports& methodExports,
    const std::string* namespaceAlias,
    const ModuleStructExports* namespaceStructs,
    const ModuleEnumExports* namespaceEnums)
{
    for (const auto& structEntry : methodExports) {
        std::string structName = structEntry.first;
        if (namespaceAlias) {
            structName = *namespaceAlias + "." + structName;
        }

        auto& table = methods_[structName];
        for (const auto& methodEntry : structEntry.second) {
            MethodSignature signature = methodEntry.second;
            if (namespaceAlias && namespaceStructs && namespaceEnums) {
                signature = qualifyNamespaceMethodSignature(
                    signature, *namespaceAlias, *namespaceStructs, *namespaceEnums);
            }
            if (table.find(methodEntry.first) != table.end()) {
                throw TypeError(diagnosticToken,
                    "duplicate method `" + methodEntry.first + "` for struct `" + structName + "`");
            }
            table.emplace(methodEntry.first, methodInfoFromSignature(signature));
        }
    }
}

void TypeChecker::recordStructMethodExports(std::size_t moduleId, const std::string& structName)
{
    const auto methods = methods_.find(structName);
    if (methods == methods_.end()) {
        return;
    }
    for (const auto& method : methods->second) {
        moduleSymbols_.recordMethodExport(moduleId, structName, method.first, methodSignatureFromInfo(method.second));
    }
}

void TypeChecker::checkMethodNameAvailable(const StructTypeDecl& structType, const ImplStmt& statement, const MethodDecl& method) const
{
    if (findMethod(statement.typeName.lexeme, method.name.lexeme)) {
        throw TypeError(method.name, "duplicate method `" + method.name.lexeme + "` for struct `" + statement.typeName.lexeme + "`");
    }
    if (findStructField(structType, method.name.lexeme)) {
        throw TypeError(method.name,
            "method `" + method.name.lexeme + "` conflicts with field `" + method.name.lexeme + "` on struct `" + statement.typeName.lexeme + "`");
    }
}

void TypeChecker::registerMethodSignature(const StructTypeDecl& structType, const ImplStmt& statement, const MethodDecl& method)
{
    checkMethodNameAvailable(structType, statement, method);

    for (const std::string& receiverParameter : structType.genericParameters) {
        for (const TypeParameter& methodParameter : method.typeParameters) {
            if (receiverParameter == methodParameter.name.lexeme) {
                throw TypeError(methodParameter.name,
                    "method type parameter `" + methodParameter.name.lexeme
                        + "` conflicts with receiver type parameter `"
                        + receiverParameter + "`");
            }
        }
    }

    auto& structMethods = methods_[statement.typeName.lexeme];
    beginTypeParameterScope(method.typeParameters);
    std::vector<TypeInfo> parameterTypes = resolveParameterTypes(method.parameters);
    std::optional<TypeInfo> expectedReturnType = resolveOptionalReturnType(method.returnTypeName);
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints
        = typeParameterConstraints(method.typeParameters);
    endTypeParameterScope();
    MethodInfo info;
    info.declaration = &method;
    std::vector<TypeInfo> receiverTypeArguments;
    receiverTypeArguments.reserve(structType.genericParameters.size());
    for (const std::string& receiverParameter : structType.genericParameters) {
        const TypeInfo* type = findTypeParameter(receiverParameter);
        receiverTypeArguments.push_back(type ? *type : typeParameterType(receiverParameter));
    }
    info.receiverType = namedStructType(
        statement.typeName.lexeme, std::move(receiverTypeArguments));
    info.parameterTypes = std::move(parameterTypes);
    info.returnType = expectedReturnType ? *expectedReturnType : unknownType();
    info.resolvedName = makeResolvedName("__method_" + statement.typeName.lexeme + "_" + method.name.lexeme);
    info.genericParameters = typeParameterNames(method.typeParameters);
    info.genericParameterConstraints = std::move(genericParameterConstraints);
    static_cast<void>(nextDeclarationId_++);
    static_cast<void>(nextSymbolId_++);
    structMethods.emplace(method.name.lexeme, std::move(info));
}

void TypeChecker::checkMethodBody(const std::string& structName, const MethodInfo& method)
{
    const MethodDecl& declaration = *method.declaration;

    beginTypeParameterScope(declaration.typeParameters);
    beginScope();
    ++functionDepth_;
    const std::size_t enclosingLoopDepth = loopDepth_;
    loopDepth_ = 0;

    std::vector<std::string> parameterNames;
    Token thisToken{TokenType::Identifier, "this", declaration.name.line, declaration.name.column};
    Binding thisBinding = declareVariable(thisToken, method.receiverType, true);
    parameterNames.push_back(thisBinding.resolvedName);

    for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
        const Parameter& parameter = declaration.parameters[i];
        Binding parameterBinding = declareVariable(parameter.name, method.parameterTypes[i], parameter.typeName.has_value());
        parameterNames.push_back(parameterBinding.resolvedName);
    }
    declarationIndex_.recordFunctionMetadata(
        declaration,
        FunctionMetadataRecord{
            method.resolvedName,
            declaration.name.lexeme,
            parameterNames});

    std::optional<TypeInfo> expectedReturnType;
    if (declaration.returnTypeName) {
        expectedReturnType = method.returnType;
    }

    const TypeInfo returnType = checkFunctionBody(
        declaration.body,
        expectedReturnType,
        declaration.name,
        structName + "." + declaration.name.lexeme);

    loopDepth_ = enclosingLoopDepth;
    --functionDepth_;
    endScope();

    auto& stored = methods_[structName][declaration.name.lexeme];
    stored.returnType = returnType;
    std::vector<TypeInfo> signatureParameters;
    signatureParameters.reserve(1 + stored.parameterTypes.size());
    signatureParameters.push_back(stored.receiverType);
    signatureParameters.insert(
        signatureParameters.end(),
        stored.parameterTypes.begin(),
        stored.parameterTypes.end());
    if (const DeclarationRecord* record = declarationIndex_.declaration(declaration)) {
        declarationIndex_.recordResolvedSignature(
            record->declarationId,
            functionType(
                std::move(signatureParameters),
                stored.returnType,
                stored.genericParameters,
                stored.genericParameterConstraints));
    }
    endTypeParameterScope();
}

void TypeChecker::checkImpl(const ImplStmt& statement)
{
    const StructTypeDecl* structType = findStructType(statement.typeName.lexeme);
    if (!structType) {
        throw TypeError(statement.typeName, "unknown struct type `" + statement.typeName.lexeme + "` in impl");
    }

    if (structType->genericParameters.empty()) {
        if (!statement.typeParameters.empty()) {
            throw TypeError(statement.typeName,
                "impl for non-generic struct `" + statement.typeName.lexeme
                    + "` cannot declare type parameters");
        }
    } else {
        if (statement.typeParameters.size() != structType->genericParameters.size()) {
            throw TypeError(statement.typeName,
                "impl for generic struct `" + statement.typeName.lexeme + "` expects "
                    + std::to_string(structType->genericParameters.size())
                    + " type parameters but got "
                    + std::to_string(statement.typeParameters.size()));
        }
        for (std::size_t i = 0; i < statement.typeParameters.size(); ++i) {
            if (statement.typeParameters[i].name.lexeme != structType->genericParameters[i]) {
                throw TypeError(statement.typeParameters[i].name,
                    "impl type parameter `" + statement.typeParameters[i].name.lexeme
                        + "` must bind struct type parameter `"
                        + structType->genericParameters[i] + "`");
            }
            if (!statement.typeParameters[i].constraint) {
                continue;
            }
            const TypeInfo headerConstraint = resolveTypeParameterConstraint(
                *statement.typeParameters[i].constraint);
            const std::shared_ptr<TypeInfo>& declaredConstraint
                = i < structType->genericParameterConstraints.size()
                ? structType->genericParameterConstraints[i]
                : nullptr;
            if (!declaredConstraint
                || !SemanticTypes::compatible(*declaredConstraint, headerConstraint)
                || !SemanticTypes::compatible(headerConstraint, *declaredConstraint)) {
                throw TypeError(statement.typeParameters[i].name,
                    "impl constraint for type parameter `"
                        + statement.typeParameters[i].name.lexeme
                        + "` does not match the struct declaration");
            }
        }

        beginTypeParameterScope(statement.typeParameters);
        for (std::size_t i = 0; i < structType->genericParameters.size(); ++i) {
            const auto found = typeParameterScopes_.back().find(
                structType->genericParameters[i]);
            if (found != typeParameterScopes_.back().end()
                && i < structType->genericParameterConstraints.size()
                && structType->genericParameterConstraints[i]) {
                found->second.typeParameterConstraint
                    = std::make_shared<TypeInfo>(
                        *structType->genericParameterConstraints[i]);
            }
        }
    }

    auto& structMethods = methods_[statement.typeName.lexeme];
    for (const MethodDecl& method : statement.methods) {
        registerMethodSignature(*structType, statement, method);
    }
    for (const MethodDecl& method : statement.methods) {
        const auto info = structMethods.find(method.name.lexeme);
        if (info == structMethods.end()) {
            throw TypeError(method.name, "internal error: missing method signature");
        }
        checkMethodBody(statement.typeName.lexeme, info->second);
    }

    if (!structType->genericParameters.empty()) {
        endTypeParameterScope();
    }
}

void TypeChecker::checkFunction(const FunctionStmt& statement)
{
    const bool nestedFunction = functionDepth_ > 0;
    beginTypeParameterScope(statement.typeParameters);

    std::vector<TypeInfo> declaredParameterTypes;
    declaredParameterTypes.reserve(statement.parameters.size());
    for (const Parameter& parameter : statement.parameters) {
        declaredParameterTypes.push_back(parameter.typeName
            ? resolveAnnotation(*parameter.typeName)
            : unknownType());
    }

    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints
        = typeParameterConstraints(statement.typeParameters);

    std::optional<TypeInfo> expectedReturnType;
    if (statement.returnTypeName) {
        expectedReturnType = resolveAnnotation(*statement.returnTypeName);
    }

    Binding functionBinding = declareVariable(
        statement.name,
        functionType(
            declaredParameterTypes,
            expectedReturnType ? *expectedReturnType : unknownType(),
            typeParameterNames(statement.typeParameters),
        genericParameterConstraints),
        statement.returnTypeName.has_value());

    beginScope();
    ++functionDepth_;
    const std::size_t enclosingLoopDepth = loopDepth_;
    loopDepth_ = 0;

    std::vector<std::string> parameterNames;
    for (std::size_t i = 0; i < statement.parameters.size(); ++i) {
        const Parameter& parameter = statement.parameters[i];
        Binding parameterBinding = declareVariable(parameter.name, declaredParameterTypes[i], parameter.typeName.has_value());
        parameterNames.push_back(parameterBinding.resolvedName);
    }
    declarationIndex_.recordFunctionMetadata(
        statement,
        FunctionMetadataRecord{
            functionBinding.resolvedName,
            statement.name.lexeme,
            parameterNames});

    const TypeInfo returnType = checkFunctionBody(
        statement.body,
        expectedReturnType,
        statement.name,
        statement.name.lexeme);

    std::unordered_set<std::string> allowedTypeParameters;
    for (const TypeParameter& parameter : statement.typeParameters) {
        allowedTypeParameters.insert(parameter.name.lexeme);
    }
    if (nestedFunction) {
        for (const TypeInfo& parameterType : declaredParameterTypes) {
            if (hasEscapingTypeParameter(parameterType, allowedTypeParameters)) {
                throw TypeError(statement.name,
                    "type parameter escapes nested function");
            }
        }
        if (hasEscapingTypeParameter(returnType, allowedTypeParameters)) {
            throw TypeError(statement.name,
                "type parameter escapes nested function");
        }
        if (expectedReturnType
            && hasEscapingTypeParameter(*expectedReturnType, allowedTypeParameters)) {
            throw TypeError(statement.name,
                "type parameter escapes nested function");
        }
    }

    loopDepth_ = enclosingLoopDepth;
    --functionDepth_;
    endScope();

    Binding* storedFunction = findVariable(statement.name.lexeme);
    if (!storedFunction) {
        throw TypeError(statement.name, "undefined function `" + statement.name.lexeme + "`");
    }
    storedFunction->type = functionType(
        std::move(declaredParameterTypes),
        returnType,
        typeParameterNames(statement.typeParameters),
        std::move(genericParameterConstraints));
    if (const DeclarationRecord* record = declarationIndex_.declaration(statement)) {
        declarationIndex_.recordResolvedSignature(record->declarationId, storedFunction->type);
    }
    endTypeParameterScope();
}

void TypeChecker::inferTypeArguments(
    const TypeInfo& expected,
    const TypeInfo& actual,
    TypeSubstitutions& substitutions,
    const Token& callToken) const
{
    if (const auto conflict = SemanticTypes::inferTypeArguments(expected, actual, substitutions)) {
        throw TypeError(callToken,
            "type parameter " + conflict->parameterName + " inferred as "
                + typeInfoName(conflict->first) + " and " + typeInfoName(conflict->second));
    }
}

void TypeChecker::validateGenericTypeArguments(
    const std::vector<std::string>& parameters,
    const std::vector<std::shared_ptr<TypeInfo>>& constraints,
    const TypeSubstitutions& substitutions,
    const Token& callToken,
    const std::string& context) const
{
    if (const auto violation = SemanticTypes::validateTypeParameterConstraints(
            parameters, constraints, substitutions)) {
        const std::string prefix = context.empty() ? "" : context + ": ";
        throw TypeError(callToken,
            prefix + "type parameter " + violation->parameterName + " must satisfy "
                + typeInfoName(violation->constraint) + ", got "
                + typeInfoName(violation->actual));
    }
}

TypeInfo TypeChecker::specializeGenericCallback(
    const Token& callToken,
    const TypeInfo& callbackType,
    const std::vector<TypeInfo>& argumentTypes,
    const std::string& functionName) const
{
    if (callbackType.genericParameters.empty()) {
        return callbackType;
    }

    TypeSubstitutions substitutions;
    for (std::size_t index = 0; index < callbackType.parameterTypes.size(); ++index) {
        inferTypeArguments(
            callbackType.parameterTypes[index], argumentTypes[index], substitutions, callToken);
    }
    for (const std::string& parameter : callbackType.genericParameters) {
        if (substitutions.find(parameter) == substitutions.end()) {
            throw TypeError(callToken,
                functionName + " cannot infer type parameter " + parameter);
        }
    }
    validateGenericTypeArguments(
        callbackType.genericParameters,
        callbackType.genericParameterConstraints,
        substitutions,
        callToken,
        functionName);

    TypeInfo specialized = SemanticTypes::substituteTypeParameters(callbackType, substitutions);
    specialized.genericParameters.clear();
    specialized.genericParameterConstraints.clear();
    return specialized;
}

TypeChecker::CheckedExpression TypeChecker::checkFunctionCall(
    const Token& callToken,
    const TypeInfo& calleeType,
    const std::vector<TypeAnnotation>& typeArguments,
    const std::vector<ExprPtr>& arguments)
{
    const bool explicitTypes = !typeArguments.empty();
    if (calleeType.kind != StaticType::Unknown && calleeType.kind != StaticType::Function) {
        throw TypeError(callToken, "can only call functions");
    }
    if (calleeType.kind != StaticType::Function || !SemanticTypes::hasFunctionSignature(calleeType)) {
        if (explicitTypes) {
            throw TypeError(callToken, "explicit type arguments require a known function signature");
        }
        for (const auto& argument : arguments) {
            checkExpressionInfo(*argument);
        }
        return CheckedExpression{unknownType()};
    }

    const bool generic = !calleeType.genericParameters.empty();
    if (explicitTypes && !generic) {
        throw TypeError(callToken, "function is not generic");
    }
    if (explicitTypes && typeArguments.size() != calleeType.genericParameters.size()) {
        throw TypeError(callToken,
            "expected " + std::to_string(calleeType.genericParameters.size())
                + " type arguments but got " + std::to_string(typeArguments.size()));
    }

    if (calleeType.parameterTypes.size() != arguments.size()) {
        throw TypeError(callToken,
            "expected " + std::to_string(calleeType.parameterTypes.size())
                + " arguments but got " + std::to_string(arguments.size()));
    }

    std::vector<CheckedExpression> checkedArguments;
    checkedArguments.reserve(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        checkedArguments.push_back(generic
            ? checkExpressionInfo(*arguments[i])
            : checkExpressionInfo(*arguments[i], &calleeType.parameterTypes[i]));
    }

    TypeSubstitutions substitutions;
    if (generic) {
        if (explicitTypes) {
            for (std::size_t i = 0; i < typeArguments.size(); ++i) {
                substitutions.emplace(
                    calleeType.genericParameters[i],
                    resolveAnnotation(typeArguments[i]));
            }
        } else {
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                inferTypeArguments(
                    calleeType.parameterTypes[i], checkedArguments[i].type,
                    substitutions, callToken);
            }
        }
        for (const std::string& parameter : calleeType.genericParameters) {
            if (substitutions.find(parameter) == substitutions.end()) {
                throw TypeError(callToken, "cannot infer type parameter " + parameter);
            }
        }
        validateGenericTypeArguments(
            calleeType.genericParameters,
            calleeType.genericParameterConstraints,
            substitutions,
            callToken,
            "function call");
    }

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const TypeInfo expected = generic
            ? SemanticTypes::substituteTypeParameters(calleeType.parameterTypes[i], substitutions)
            : calleeType.parameterTypes[i];
        const TypeInfo& actual = checkedArguments[i].type;
        if (!SemanticTypes::compatible(expected, actual)) {
            throw TypeError(callToken,
                "argument " + std::to_string(i + 1) + " expects "
                    + typeInfoName(expected) + ", got " + typeInfoName(actual));
        }
    }

    const TypeInfo returnType = generic
        ? SemanticTypes::substituteTypeParameters(*calleeType.returnType, substitutions)
        : *calleeType.returnType;
    return CheckedExpression{returnType};
}

const TypeInfo* TypeChecker::contextualFunctionType(const TypeInfo* expectedType) const
{
    if (!expectedType || expectedType->kind != StaticType::Function || !SemanticTypes::hasFunctionSignature(*expectedType)) {
        return nullptr;
    }
    return expectedType;
}

TypeChecker::CheckedExpression TypeChecker::checkFunctionExpression(const FunctionExpr& expression, const TypeInfo* expectedType)
{
    const bool nestedFunction = functionDepth_ > 0;
    beginTypeParameterScope(expression.typeParameters);

    const TypeInfo* context = contextualFunctionType(expectedType);
    const TypeInfo* contextualSignature = expression.typeParameters.empty() ? context : nullptr;
    if (context && context->parameterTypes.size() != expression.parameters.size()) {
        throw TypeError(expression.keyword,
            "expected " + std::to_string(context->parameterTypes.size())
                + " parameters but got " + std::to_string(expression.parameters.size()));
    }

    std::vector<TypeInfo> declaredParameterTypes;
    declaredParameterTypes.reserve(expression.parameters.size());
    for (std::size_t i = 0; i < expression.parameters.size(); ++i) {
        const Parameter& parameter = expression.parameters[i];
        TypeInfo parameterType = parameter.typeName
            ? resolveAnnotation(*parameter.typeName)
            : (contextualSignature ? contextualSignature->parameterTypes[i] : unknownType());

        if (contextualSignature && parameter.typeName
            && !SemanticTypes::compatible(parameterType, contextualSignature->parameterTypes[i])) {
            throw TypeError(parameter.name,
                "parameter `" + parameter.name.lexeme + "` expects " + typeInfoName(contextualSignature->parameterTypes[i])
                    + ", got " + typeInfoName(parameterType));
        }

        declaredParameterTypes.push_back(std::move(parameterType));
    }

    std::optional<TypeInfo> expectedReturnType;
    if (expression.returnTypeName) {
        expectedReturnType = resolveAnnotation(*expression.returnTypeName);
    }
    if (contextualSignature && contextualSignature->returnType) {
        if (expectedReturnType && !SemanticTypes::compatible(*contextualSignature->returnType, *expectedReturnType)) {
            throw TypeError(expression.returnTypeName->token,
                "function `<lambda>` expects return " + typeInfoName(*contextualSignature->returnType)
                    + ", got " + typeInfoName(*expectedReturnType));
        }
        if (!expectedReturnType) {
            expectedReturnType = *contextualSignature->returnType;
        }
    }

    beginScope();
    ++functionDepth_;
    const std::size_t enclosingLoopDepth = loopDepth_;
    loopDepth_ = 0;

    std::vector<std::string> parameterNames;
    for (std::size_t i = 0; i < expression.parameters.size(); ++i) {
        const Parameter& parameter = expression.parameters[i];
        Binding parameterBinding = declareVariable(
            parameter.name,
            declaredParameterTypes[i],
            parameter.typeName.has_value() || contextualSignature != nullptr);
        parameterNames.push_back(parameterBinding.resolvedName);
    }
    declarationIndex_.recordFunctionMetadata(
        expression,
        FunctionMetadataRecord{"<lambda>", "<lambda>", parameterNames});

    const TypeInfo returnType = checkFunctionBody(
        expression.body,
        expectedReturnType,
        expression.keyword,
        "<lambda>");

    std::unordered_set<std::string> allowedTypeParameters;
    for (const TypeParameter& parameter : expression.typeParameters) {
        allowedTypeParameters.insert(parameter.name.lexeme);
    }
    if (nestedFunction) {
        for (const TypeInfo& parameterType : declaredParameterTypes) {
            if (hasEscapingTypeParameter(parameterType, allowedTypeParameters)) {
                throw TypeError(expression.keyword,
                    "type parameter escapes nested function");
            }
        }
        if (hasEscapingTypeParameter(returnType, allowedTypeParameters)) {
            throw TypeError(expression.keyword,
                "type parameter escapes nested function");
        }
        if (expectedReturnType
            && hasEscapingTypeParameter(*expectedReturnType, allowedTypeParameters)) {
            throw TypeError(expression.keyword,
                "type parameter escapes nested function");
        }
    }

    loopDepth_ = enclosingLoopDepth;
    --functionDepth_;
    endScope();

    const TypeInfo result = functionType(
        std::move(declaredParameterTypes),
        returnType,
        typeParameterNames(expression.typeParameters),
        typeParameterConstraints(expression.typeParameters));
    endTypeParameterScope();
    return CheckedExpression{result};
}

TypeChecker::CheckedExpression TypeChecker::checkLetInitializer(const LetStmt& statement)
{
    if (!statement.typeName) {
        return checkExpressionInfo(*statement.initializer);
    }

    const TypeInfo declared = resolveAnnotation(*statement.typeName);
    const CheckedExpression initializer = checkExpressionInfo(*statement.initializer, &declared);
    if (declared.kind == StaticType::Function
        && declared.genericParameters.empty()
        && initializer.type.kind == StaticType::Function
        && !initializer.type.genericParameters.empty()) {
        throw TypeError(statement.name,
            "cannot assign generic function to monomorphic function type");
    }
    checkAssignable(
        statement.name,
        "cannot initialize `" + statement.name.lexeme + "` of type " + typeInfoName(declared)
            + " with " + typeInfoName(initializer.type),
        declared,
        initializer.type);
    return CheckedExpression{declared};
}

const TypeChecker::StructFieldType* TypeChecker::findStructField(
    const StructTypeDecl& structType,
    const std::string& name) const
{
    for (const StructFieldType& field : structType.fields) {
        if (field.name.lexeme == name) {
            return &field;
        }
    }
    return nullptr;
}

bool TypeChecker::canAccessPrivateFields(const StructTypeDecl& structType) const
{
    if (!structType.hasPrivateFields) {
        return true;
    }

    // A direct input (including the established ordered multi-file entry
    // path) is one compilation unit, so all of its declarations share the
    // same module visibility boundary.
    if (moduleStack_.empty()) {
        return true;
    }

    return structType.definingModuleId
        && *structType.definingModuleId == moduleStack_.back();
}

TypeInfo TypeChecker::structFieldTypeForValue(
    const TypeInfo& objectType,
    const StructTypeDecl& structType,
    const StructFieldType& field) const
{
    TypeSubstitutions substitutions;
    for (std::size_t i = 0; i < structType.genericParameters.size(); ++i) {
        if (i < objectType.typeArguments.size()) {
            substitutions.emplace(
                structType.genericParameters[i], objectType.typeArguments[i]);
        }
    }
    return SemanticTypes::substituteTypeParameters(field.type, substitutions);
}

TypeChecker::CheckedExpression TypeChecker::checkNamedStructFields(
    const Token& diagnosticToken,
    const TypeInfo& declared,
    const std::vector<StructField>& fields)
{
    const StructTypeDecl* structType = declared.structName ? findStructType(*declared.structName) : nullptr;
    if (!structType) {
        throw TypeError(diagnosticToken, "unknown struct type `" + typeInfoName(declared) + "`");
    }

    if (structType->hasPrivateFields && !canAccessPrivateFields(*structType)) {
        throw TypeError(diagnosticToken,
            "struct `" + unqualifiedStructName(structType->name.lexeme)
                + "` has private fields and cannot be constructed outside its defining module");
    }

    std::unordered_map<std::string, const StructField*> literalFields;
    for (const StructField& field : fields) {
        if (literalFields.find(field.name.lexeme) != literalFields.end()) {
            throw TypeError(field.name, "duplicate field `" + field.name.lexeme + "` in struct literal");
        }
        literalFields.emplace(field.name.lexeme, &field);
    }

    for (const StructFieldType& expectedField : structType->fields) {
        const auto found = literalFields.find(expectedField.name.lexeme);
        if (found == literalFields.end()) {
            throw TypeError(diagnosticToken,
                "missing field `" + expectedField.name.lexeme + "` for struct `" + structType->name.lexeme + "`");
        }
        const TypeInfo expectedFieldType = structFieldTypeForValue(
            declared, *structType, expectedField);
        const CheckedExpression actual = checkExpressionInfo(
            *found->second->value, &expectedFieldType);
        if (!SemanticTypes::compatible(expectedFieldType, actual.type)) {
            throw TypeError(found->second->name,
                "field `" + expectedField.name.lexeme + "` expects " + typeInfoName(expectedFieldType)
                    + ", got " + typeInfoName(actual.type));
        }
    }

    for (const StructField& field : fields) {
        if (!findStructField(*structType, field.name.lexeme)) {
            throw TypeError(field.name,
                "extra field `" + field.name.lexeme + "` for struct `" + structType->name.lexeme + "`");
        }
    }

    return CheckedExpression{declared};
}

TypeChecker::CheckedExpression TypeChecker::checkStructConstructor(
    const StructConstructExpr& expression,
    const TypeInfo* expectedType)
{
    if (expression.qualifier) {
        const NamespaceImport* namespaceImport = findNamespace(expression.qualifier->lexeme);
        if (!namespaceImport) {
            throw TypeError(*expression.qualifier, "unknown module namespace `" + expression.qualifier->lexeme + "`");
        }
        if (namespaceImport->structs.find(expression.name.lexeme) == namespaceImport->structs.end()) {
            throw TypeError(expression.name,
                "module namespace `" + expression.qualifier->lexeme + "` has no exported type `" + expression.name.lexeme + "`");
        }
    }

    const std::string typeName = structConstructorTypeName(expression);
    const StructTypeDecl* structType = findStructType(typeName);
    if (!structType) {
        throw TypeError(expression.name, "unknown struct type `" + typeName + "`");
    }

    const bool generic = !structType->genericParameters.empty();
    if (!generic && !expression.typeArguments.empty()) {
        throw TypeError(expression.name, "struct `" + typeName + "` is not generic");
    }

    const TypeInfo* expectedStructType = expectedType;
    if (expectedStructType && SemanticTypes::isNullable(*expectedStructType)) {
        expectedStructType = expectedStructType->nullableOf.get();
    }
    const bool expectedMatches = expectedStructType
        && expectedStructType->kind == StaticType::Struct
        && expectedStructType->structName
        && *expectedStructType->structName == typeName;

    TypeSubstitutions substitutions;
    if (generic && !expression.typeArguments.empty()) {
        if (expression.typeArguments.size() != structType->genericParameters.size()) {
            throw TypeError(expression.name,
                "struct `" + typeName + "` expects "
                    + std::to_string(structType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(expression.typeArguments.size()));
        }
        for (std::size_t i = 0; i < expression.typeArguments.size(); ++i) {
            substitutions.emplace(
                structType->genericParameters[i],
                resolveAnnotation(expression.typeArguments[i]));
        }
    }

    if (generic && expression.typeArguments.empty() && expectedMatches) {
        if (expectedStructType->typeArguments.size() != structType->genericParameters.size()) {
            throw TypeError(expression.name,
                "struct `" + typeName + "` expects "
                    + std::to_string(structType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(expectedStructType->typeArguments.size()));
        }
        for (std::size_t i = 0; i < structType->genericParameters.size(); ++i) {
            substitutions.emplace(
                structType->genericParameters[i], expectedStructType->typeArguments[i]);
        }
    }

    std::unordered_map<std::string, const StructField*> literalFields;
    for (const StructField& field : expression.fields) {
        if (!literalFields.emplace(field.name.lexeme, &field).second) {
            throw TypeError(field.name,
                "duplicate field `" + field.name.lexeme + "` in struct literal");
        }
    }

    for (const StructFieldType& field : structType->fields) {
        const auto found = literalFields.find(field.name.lexeme);
        if (found == literalFields.end()) {
            continue;
        }
        const TypeInfo expectedFieldType = SemanticTypes::substituteTypeParameters(field.type, substitutions);
        const CheckedExpression actual = checkExpressionInfo(
            *found->second->value, &expectedFieldType);
        if (!generic || !hasEscapingTypeParameter(expectedFieldType, {})) {
            if (!SemanticTypes::compatible(expectedFieldType, actual.type)) {
                throw TypeError(found->second->name,
                    "field `" + field.name.lexeme + "` expects "
                        + typeInfoName(expectedFieldType) + ", got "
                        + typeInfoName(actual.type));
            }
        }
        if (generic && expression.typeArguments.empty() && !expectedMatches) {
            inferTypeArguments(field.type, actual.type, substitutions, expression.name);
        }
    }

    std::vector<TypeInfo> typeArguments;
    if (generic) {
        for (const std::string& parameter : structType->genericParameters) {
            if (substitutions.find(parameter) == substitutions.end()) {
                throw TypeError(expression.name,
                    "cannot infer type parameter " + parameter + " for struct " + typeName);
            }
        }
        validateGenericTypeArguments(
            structType->genericParameters,
            structType->genericParameterConstraints,
            substitutions,
            expression.name,
            "struct " + typeName);
        typeArguments.reserve(structType->genericParameters.size());
        for (const std::string& parameter : structType->genericParameters) {
            typeArguments.push_back(substitutions.at(parameter));
        }
    }

    const TypeInfo declared = namedStructType(typeName, std::move(typeArguments));
    if (expectedMatches && !SemanticTypes::compatible(*expectedStructType, declared)) {
        throw TypeError(expression.name,
            "struct constructor produces " + typeInfoName(declared)
                + ", expected " + typeInfoName(*expectedStructType));
    }
    return checkNamedStructFields(expression.name, declared, expression.fields);
}

TypeChecker::CheckedExpression TypeChecker::checkVariantConstructor(
    const MemberCallExpr& expression,
    const TypeInfo* expectedType)
{
    const std::string enumName = enumConstructorTypeName(expression);
    const EnumTypeDecl* enumType = findEnumType(enumName);
    if (!enumType) {
        throw TypeError(expression.name, "unknown enum type " + enumName);
    }

    const EnumVariantType* variant = findEnumVariant(*enumType, expression.name.lexeme);
    if (!variant) {
        throw TypeError(expression.name,
            "enum " + enumName + " has no variant " + expression.name.lexeme);
    }
    if (variant->payloadTypes.size() != expression.arguments.size()) {
        throw TypeError(expression.paren,
            "variant " + enumName + "." + expression.name.lexeme + " expects "
                + std::to_string(variant->payloadTypes.size()) + " arguments but got "
                + std::to_string(expression.arguments.size()));
    }

    const bool generic = !enumType->genericParameters.empty();
    if (!generic && !expression.typeArguments.empty()) {
        throw TypeError(expression.paren, "function is not generic");
    }

    TypeSubstitutions substitutions;
    if (generic && !expression.typeArguments.empty()) {
        if (expression.typeArguments.size() != enumType->genericParameters.size()) {
            throw TypeError(expression.paren,
                "enum `" + enumName + "` expects "
                    + std::to_string(enumType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(expression.typeArguments.size()));
        }
        for (std::size_t i = 0; i < enumType->genericParameters.size(); ++i) {
            substitutions.emplace(
                enumType->genericParameters[i],
                resolveAnnotation(expression.typeArguments[i]));
        }
    }

    const TypeInfo* expectedEnumType = expectedType;
    if (expectedEnumType && SemanticTypes::isNullable(*expectedEnumType)) {
        expectedEnumType = expectedEnumType->nullableOf.get();
    }
    const bool expectedMatches = generic
        && expectedEnumType
        && expectedEnumType->kind == StaticType::Enum
        && expectedEnumType->enumName
        && *expectedEnumType->enumName == enumName;
    if (expectedMatches && expression.typeArguments.empty()) {
        if (expectedEnumType->typeArguments.size() != enumType->genericParameters.size()) {
            throw TypeError(expression.paren,
                "enum `" + enumName + "` expects "
                    + std::to_string(enumType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(expectedEnumType->typeArguments.size()));
        }
        for (std::size_t i = 0; i < enumType->genericParameters.size(); ++i) {
            substitutions.emplace(
                enumType->genericParameters[i], expectedEnumType->typeArguments[i]);
        }
    }

    std::vector<CheckedExpression> arguments;
    arguments.reserve(expression.arguments.size());
    for (std::size_t i = 0; i < expression.arguments.size(); ++i) {
        const TypeInfo payloadType = SemanticTypes::substituteTypeParameters(
            variant->payloadTypes[i], substitutions);
        const CheckedExpression argument = checkExpressionInfo(
            *expression.arguments[i],
            (!generic || !substitutions.empty()) ? &payloadType : nullptr);
        arguments.push_back(argument);
        if (generic && expression.typeArguments.empty() && !expectedMatches) {
            inferTypeArguments(
                variant->payloadTypes[i], argument.type, substitutions, expression.paren);
        }
    }

    if (generic) {
        for (const std::string& parameter : enumType->genericParameters) {
            if (substitutions.find(parameter) == substitutions.end()) {
                throw TypeError(expression.paren,
                    "cannot infer type parameter " + parameter + " for enum " + enumName);
            }
        }
        validateGenericTypeArguments(
            enumType->genericParameters,
            enumType->genericParameterConstraints,
            substitutions,
            expression.paren,
            "enum " + enumName);
    }

    std::vector<TypeInfo> typeArguments;
    typeArguments.reserve(enumType->genericParameters.size());
    for (const std::string& parameter : enumType->genericParameters) {
        typeArguments.push_back(substitutions.at(parameter));
    }

    std::vector<TypeInfo> resolvedPayloadTypes;
    resolvedPayloadTypes.reserve(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const TypeInfo payloadType = SemanticTypes::substituteTypeParameters(
            variant->payloadTypes[i], substitutions);
        resolvedPayloadTypes.push_back(payloadType);
        if (!SemanticTypes::compatible(payloadType, arguments[i].type)) {
            throw TypeError(expression.paren,
                "variant argument " + std::to_string(i + 1) + " expects "
                    + typeInfoName(payloadType) + ", got "
                    + typeInfoName(arguments[i].type));
        }
    }

    std::string runtimeEnumName = enumName;
    const std::size_t namespaceSeparator = enumName.find('.');
    if (namespaceSeparator != std::string::npos) {
        const std::string alias = enumName.substr(0, namespaceSeparator);
        const std::string localName = enumName.substr(namespaceSeparator + 1);
        if (const NamespaceImport* namespaceImport = findNamespace(alias)) {
            const auto found = namespaceImport->enums.find(localName);
            if (found != namespaceImport->enums.end()) {
                runtimeEnumName = found->second.name.lexeme;
            }
        }
    }
    TypeInfo resultType = namedEnumType(enumName, std::move(typeArguments));
    declarationIndex_.recordVariantConstructor(
        expression,
        runtimeEnumName,
        expression.name.lexeme,
        resultType,
        std::move(resolvedPayloadTypes));
    return CheckedExpression{std::move(resultType)};
}

bool TypeChecker::checkPattern(
    const Pattern& pattern,
    const TypeInfo& expectedType,
    std::unordered_set<std::string>& coveredVariants,
    std::unordered_set<std::string>& coveredLiterals,
    bool& coversNil,
    bool& coversStruct,
    PatternBindings* deferredBindings)
{
    if (dynamic_cast<const WildcardPattern*>(&pattern)) {
        coversStruct = true;
        return true;
    }

    if (const auto* variable = dynamic_cast<const VariablePattern*>(&pattern)) {
        if (deferredBindings) {
            if (deferredBindings->find(variable->name.lexeme) != deferredBindings->end()) {
                throw TypeError(variable->name,
                    "duplicate pattern binding `" + variable->name.lexeme + "` in OR pattern");
            }
            deferredBindings->emplace(
                variable->name.lexeme,
                PatternBindingInfo{variable->name, expectedType, {variable}});
            coversStruct = true;
            return true;
        }
        const Binding binding = declareVariable(variable->name, expectedType, false);
        declarationIndex_.recordPatternBinding(
            *variable,
            PatternBindingRecord{
                variable->name.lexeme,
                binding.resolvedName,
                binding.type,
                binding.range,
                binding.bindingId,
                ResolvedSymbol{binding.declarationId, binding.symbolId}});
        coversStruct = true;
        return true;
    }

    if (const auto* recordPattern = dynamic_cast<const RecordPattern*>(&pattern)) {
        const TypeInfo* structExpectedType = SemanticTypes::isNullable(expectedType)
            ? expectedType.nullableOf.get()
            : &expectedType;
        if (!structExpectedType
            || structExpectedType->kind != StaticType::Struct
            || !structExpectedType->structName) {
            throw TypeError(recordPattern->name, "record pattern expects struct value");
        }

        const std::string patternTypeName = recordPatternTypeName(*recordPattern);
        if (patternTypeName != *structExpectedType->structName) {
            throw TypeError(recordPattern->name,
                "record pattern belongs to struct " + patternTypeName
                    + ", expected " + *structExpectedType->structName);
        }

        const StructTypeDecl* structType = findStructType(patternTypeName);
        if (!structType) {
            throw TypeError(recordPattern->name, "unknown struct type " + patternTypeName);
        }

        std::unordered_set<std::string> usedFields;
        bool universal = true;
        std::vector<std::string> resolvedFieldNames;
        std::vector<TypeInfo> resolvedFieldTypes;
        resolvedFieldNames.reserve(recordPattern->fields.size());
        resolvedFieldTypes.reserve(recordPattern->fields.size());
        for (const RecordPatternField& field : recordPattern->fields) {
            if (!usedFields.insert(field.name.lexeme).second) {
                throw TypeError(field.name,
                    "duplicate field `" + field.name.lexeme
                        + "` in record pattern for struct `" + patternTypeName + "`");
            }
            const StructFieldType* structField = findStructField(*structType, field.name.lexeme);
            if (!structField) {
                throw TypeError(field.name,
                    "struct `" + unqualifiedStructName(patternTypeName)
                        + (structType->hasPrivateFields ? "` has no accessible field `" : "` has no field `")
                        + field.name.lexeme + "` in record pattern");
            }
            if (structField->isPrivate && !canAccessPrivateFields(*structType)) {
                throw TypeError(field.name,
                    "struct `" + unqualifiedStructName(patternTypeName) + "` has no accessible field `"
                        + field.name.lexeme + "` in record pattern");
            }

            std::unordered_set<std::string> nestedCoverage;
            std::unordered_set<std::string> nestedLiterals;
            bool nestedCoversNil = false;
            bool nestedCoversStruct = false;
            const TypeInfo fieldType = structFieldTypeForValue(
                *structExpectedType, *structType, *structField);
            resolvedFieldNames.push_back(field.name.lexeme);
            resolvedFieldTypes.push_back(fieldType);
            const bool fieldUniversal = checkPattern(
                *field.pattern,
                fieldType,
                nestedCoverage,
                nestedLiterals,
                nestedCoversNil,
                nestedCoversStruct,
                deferredBindings);
            universal = universal && fieldUniversal;
        }

        declarationIndex_.recordRecordPattern(
            *recordPattern,
            RecordPatternRecord{
                *structExpectedType,
                std::move(resolvedFieldNames),
                std::move(resolvedFieldTypes)});
        coversStruct = universal;
        return SemanticTypes::isNullable(expectedType) ? false : universal;
    }

    if (const auto* orPattern = dynamic_cast<const OrPattern*>(&pattern)) {
        if (orPattern->alternatives.size() < 2) {
            throw TypeError(orPattern->pipe, "OR pattern requires at least two alternatives");
        }

        PatternBindings mergedBindings;
        std::unordered_set<std::string> bindingNames;
        bool firstAlternative = true;
        bool mergedCoversNil = false;
        bool mergedCoversStruct = false;
        bool mergedCoversAll = false;
        for (const PatternPtr& alternative : orPattern->alternatives) {
            std::unordered_set<std::string> alternativeVariants;
            std::unordered_set<std::string> alternativeLiterals;
            bool alternativeCoversNil = false;
            PatternBindings alternativeBindings;
            bool alternativeCoversStruct = false;
            const bool alternativeCoversAll = checkPattern(
                *alternative,
                expectedType,
                alternativeVariants,
                alternativeLiterals,
                alternativeCoversNil,
                alternativeCoversStruct,
                &alternativeBindings);

            coveredVariants.insert(alternativeVariants.begin(), alternativeVariants.end());
            coveredLiterals.insert(alternativeLiterals.begin(), alternativeLiterals.end());
            mergedCoversNil = mergedCoversNil || alternativeCoversNil;
            mergedCoversStruct = mergedCoversStruct || alternativeCoversStruct;
            mergedCoversAll = mergedCoversAll || alternativeCoversAll;

            if (firstAlternative) {
                firstAlternative = false;
                for (auto& entry : alternativeBindings) {
                    bindingNames.insert(entry.first);
                    mergedBindings.emplace(entry.first, std::move(entry.second));
                }
                continue;
            }

            if (alternativeBindings.size() != bindingNames.size()) {
                throw TypeError(orPattern->pipe,
                    "OR pattern alternatives must bind the same names");
            }
            for (const std::string& name : bindingNames) {
                const auto alternativeBinding = alternativeBindings.find(name);
                if (alternativeBinding == alternativeBindings.end()) {
                    throw TypeError(orPattern->pipe,
                        "OR pattern alternatives must bind the same names");
                }
                PatternBindingInfo& merged = mergedBindings.at(name);
                if (!SemanticTypes::compatible(merged.type, alternativeBinding->second.type)
                    || !SemanticTypes::compatible(alternativeBinding->second.type, merged.type)) {
                    throw TypeError(orPattern->pipe,
                        "OR pattern binding `" + name + "` has incompatible types: "
                            + typeInfoName(merged.type) + " and "
                            + typeInfoName(alternativeBinding->second.type));
                }
                merged.occurrences.insert(
                    merged.occurrences.end(),
                    alternativeBinding->second.occurrences.begin(),
                    alternativeBinding->second.occurrences.end());
            }
        }

        OrPatternRecord patternRecord;
        patternRecord.bindingNames.reserve(mergedBindings.size());
        patternRecord.bindingTypes.reserve(mergedBindings.size());
        for (const auto& entry : mergedBindings) {
            patternRecord.bindingNames.push_back(entry.first);
            patternRecord.bindingTypes.push_back(entry.second.type);
        }
        declarationIndex_.recordOrPattern(*orPattern, std::move(patternRecord));

        if (deferredBindings) {
            for (auto& entry : mergedBindings) {
                if (deferredBindings->find(entry.first) != deferredBindings->end()) {
                    throw TypeError(orPattern->pipe,
                        "duplicate pattern binding `" + entry.first + "` in OR pattern");
                }
                deferredBindings->emplace(entry.first, std::move(entry.second));
            }
        } else {
            for (auto& entry : mergedBindings) {
                const Binding binding = declareVariable(
                    entry.second.token,
                    entry.second.type,
                    false);
                for (const VariablePattern* occurrence : entry.second.occurrences) {
                    declarationIndex_.recordPatternBinding(
                        *occurrence,
                        PatternBindingRecord{
                            occurrence->name.lexeme,
                            binding.resolvedName,
                            binding.type,
                            binding.range,
                            binding.bindingId,
                            ResolvedSymbol{binding.declarationId, binding.symbolId}});
                }
            }
        }

        coversNil = coversNil || mergedCoversNil;
        coversStruct = coversStruct || mergedCoversStruct;
        if (SemanticTypes::isNullable(expectedType)
            && expectedType.nullableOf
            && expectedType.nullableOf->kind == StaticType::Struct
            && mergedCoversNil && mergedCoversStruct) {
            mergedCoversAll = true;
        }
        return mergedCoversAll;
    }

    if (const auto* literal = dynamic_cast<const LiteralPattern*>(&pattern)) {
        const TypeInfo literalType = literalPatternType(literal->value);
        if (literal->value.type == TokenType::Nil) {
            if (expectedType.kind == StaticType::Nil) {
                declarationIndex_.recordLiteralPattern(
                    *literal,
                    LiteralPatternRecord{literal->value.lexeme, literalType});
                return true;
            }
            if (SemanticTypes::isNullable(expectedType)) {
                coversNil = true;
                declarationIndex_.recordLiteralPattern(
                    *literal,
                    LiteralPatternRecord{literal->value.lexeme, literalType});
                return false;
            }
            if (expectedType.kind == StaticType::Enum) {
                throw TypeError(literal->value, "literal patterns are not valid for enum values");
            }
        }

        const TypeInfo* valueType = SemanticTypes::isNullable(expectedType)
            ? expectedType.nullableOf.get()
            : &expectedType;
        if (!valueType || !SemanticTypes::compatible(*valueType, literalType)) {
            throw TypeError(literal->value,
                "literal pattern expects " + typeInfoName(expectedType)
                    + ", got " + typeInfoName(literalType));
        }
        declarationIndex_.recordLiteralPattern(
            *literal,
            LiteralPatternRecord{literal->value.lexeme, literalType});
        if (literal->value.type == TokenType::True
            || literal->value.type == TokenType::False) {
            coveredLiterals.insert(literal->value.lexeme);
        }
        return false;
    }

    const auto* variantPattern = dynamic_cast<const VariantPattern*>(&pattern);
    if (!variantPattern) {
        throw TypeError("unsupported pattern node");
    }
    const TypeInfo* enumExpectedType = &expectedType;
    if (SemanticTypes::isNullable(expectedType)) {
        enumExpectedType = expectedType.nullableOf.get();
    }
    if (!enumExpectedType
        || enumExpectedType->kind != StaticType::Enum
        || !enumExpectedType->enumName) {
        throw TypeError(variantPattern->name, "variant pattern expects enum value");
    }
    if (!variantPattern->qualifier
        || variantPattern->qualifier->lexeme != *enumExpectedType->enumName) {
        throw TypeError(variantPattern->name,
            "variant pattern belongs to enum "
                + (variantPattern->qualifier ? variantPattern->qualifier->lexeme : std::string("<unknown>"))
                + ", expected " + *enumExpectedType->enumName);
    }

    const EnumTypeDecl* enumType = findEnumType(*enumExpectedType->enumName);
    const EnumVariantType* variant = enumType
        ? findEnumVariant(*enumType, variantPattern->name.lexeme)
        : nullptr;
    if (!variant) {
        throw TypeError(variantPattern->name,
            "enum " + *enumExpectedType->enumName + " has no variant "
                + variantPattern->name.lexeme);
    }
    TypeSubstitutions substitutions;
    if (!enumType->genericParameters.empty()) {
        if (enumExpectedType->typeArguments.size() != enumType->genericParameters.size()) {
            throw TypeError(variantPattern->name,
                "enum `" + *enumExpectedType->enumName + "` expects "
                    + std::to_string(enumType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(enumExpectedType->typeArguments.size()));
        }
        for (std::size_t i = 0; i < enumType->genericParameters.size(); ++i) {
            substitutions.emplace(
                enumType->genericParameters[i], enumExpectedType->typeArguments[i]);
        }
    }
    if (variant->payloadTypes.size() != variantPattern->arguments.size()) {
        throw TypeError(variantPattern->name,
            "variant pattern " + *enumExpectedType->enumName + "." + variantPattern->name.lexeme
                + " expects " + std::to_string(variant->payloadTypes.size())
                + " patterns but got " + std::to_string(variantPattern->arguments.size()));
    }

    std::vector<std::size_t> payloadIndices;
    payloadIndices.reserve(variantPattern->arguments.size());
    bool hasNamedPattern = false;
    bool hasPositionalPattern = false;
    for (std::size_t i = 0; i < variantPattern->arguments.size(); ++i) {
        if (i < variantPattern->argumentNames.size() && variantPattern->argumentNames[i]) {
            hasNamedPattern = true;
        } else {
            hasPositionalPattern = true;
        }
    }
    if (hasNamedPattern && hasPositionalPattern) {
        throw TypeError(variantPattern->name,
            "variant pattern " + *enumExpectedType->enumName + "." + variantPattern->name.lexeme
                + " must use either all named or all positional payloads");
    }

    if (!hasNamedPattern) {
        for (std::size_t i = 0; i < variantPattern->arguments.size(); ++i) {
            payloadIndices.push_back(i);
        }
    } else {
        std::unordered_map<std::string, std::size_t> declaredPayloads;
        for (std::size_t i = 0; i < variant->payloadTypes.size(); ++i) {
            if (i >= variant->payloadNames.size() || !variant->payloadNames[i]) {
                throw TypeError(variantPattern->name,
                    "variant " + *enumExpectedType->enumName + "." + variantPattern->name.lexeme
                        + " has no named payload fields");
            }
            declaredPayloads.emplace(variant->payloadNames[i]->lexeme, i);
        }

        std::unordered_set<std::string> usedPayloads;
        for (std::size_t i = 0; i < variantPattern->arguments.size(); ++i) {
            const Token& payloadName = *variantPattern->argumentNames[i];
            const auto found = declaredPayloads.find(payloadName.lexeme);
            if (found == declaredPayloads.end()) {
                throw TypeError(payloadName,
                    "variant " + *enumExpectedType->enumName + "." + variantPattern->name.lexeme
                        + " has no payload field " + payloadName.lexeme);
            }
            if (!usedPayloads.insert(payloadName.lexeme).second) {
                throw TypeError(payloadName,
                    "duplicate payload field " + payloadName.lexeme
                        + " in variant pattern " + *enumExpectedType->enumName
                        + "." + variantPattern->name.lexeme);
            }
            payloadIndices.push_back(found->second);
        }
    }
    coveredVariants.insert(variantPattern->name.lexeme);

    std::string runtimeEnumName = *enumExpectedType->enumName;
    const std::size_t namespaceSeparator = runtimeEnumName.find('.');
    if (namespaceSeparator != std::string::npos) {
        const std::string alias = runtimeEnumName.substr(0, namespaceSeparator);
        const std::string localName = runtimeEnumName.substr(namespaceSeparator + 1);
        if (const NamespaceImport* namespaceImport = findNamespace(alias)) {
            const auto found = namespaceImport->enums.find(localName);
            if (found != namespaceImport->enums.end()) {
                runtimeEnumName = found->second.name.lexeme;
            }
        }
    }
    std::vector<TypeInfo> resolvedPayloadTypes;
    resolvedPayloadTypes.reserve(variantPattern->arguments.size());
    for (std::size_t i = 0; i < variantPattern->arguments.size(); ++i) {
        std::unordered_set<std::string> nestedCoverage;
        std::unordered_set<std::string> nestedLiterals;
        bool nestedCoversNil = false;
        bool nestedCoversStruct = false;
        const TypeInfo payloadType = SemanticTypes::substituteTypeParameters(
            variant->payloadTypes[payloadIndices[i]], substitutions);
        resolvedPayloadTypes.push_back(payloadType);
        checkPattern(
            *variantPattern->arguments[i],
            payloadType,
            nestedCoverage,
            nestedLiterals,
            nestedCoversNil,
            nestedCoversStruct,
            deferredBindings);
    }
    declarationIndex_.recordVariantPattern(
        *variantPattern,
        VariantPatternRecord{
            std::move(runtimeEnumName),
            variantPattern->name.lexeme,
            *enumExpectedType,
            std::move(payloadIndices),
            std::move(resolvedPayloadTypes)});
    return false;
}

void TypeChecker::checkMatch(const MatchStmt& statement)
{
    const TypeInfo scrutineeType = checkExpression(*statement.value);
    const bool nullableEnum = SemanticTypes::isNullable(scrutineeType)
        && scrutineeType.nullableOf->kind == StaticType::Enum
        && scrutineeType.nullableOf->enumName;
    const bool nullableStruct = SemanticTypes::isNullable(scrutineeType)
        && scrutineeType.nullableOf->kind == StaticType::Struct
        && scrutineeType.nullableOf->structName;
    const bool structValue = scrutineeType.kind == StaticType::Struct
        && scrutineeType.structName;
    const TypeInfo* primitiveType = primitiveMatchBaseType(scrutineeType);
    if ((scrutineeType.kind != StaticType::Enum || !scrutineeType.enumName)
        && !nullableEnum && !structValue && !nullableStruct && !primitiveType) {
        throw TypeError(statement.value && statement.value->span
                ? Token{TokenType::Match, "match", statement.value->span->line, statement.value->span->column}
                : Token{TokenType::Match, "match", 0, 0},
            "match expects enum, struct, bool, number, string, or nil value, got "
                + typeInfoName(scrutineeType));
    }

    std::string enumName;
    const EnumTypeDecl* enumType = nullptr;
    if (scrutineeType.kind == StaticType::Enum || nullableEnum) {
        enumName = nullableEnum
            ? *scrutineeType.nullableOf->enumName
            : *scrutineeType.enumName;
        enumType = findEnumType(enumName);
        if (!enumType) {
            throw TypeError("unknown enum type " + enumName);
        }
    }
    std::string structName;
    const StructTypeDecl* structType = nullptr;
    if (structValue || nullableStruct) {
        structName = nullableStruct
            ? *scrutineeType.nullableOf->structName
            : *scrutineeType.structName;
        structType = findStructType(structName);
        if (!structType) {
            throw TypeError("unknown struct type " + structName);
        }
    }

    std::unordered_set<std::string> coveredVariants;
    std::unordered_set<std::string> coveredLiterals;
    bool coveredNil = false;
    bool coveredStruct = false;
    bool coversAll = false;
    for (const MatchArm& arm : statement.arms) {
        beginScope();
        std::unordered_set<std::string> armCoveredVariants;
        std::unordered_set<std::string> armCoveredLiterals;
        bool armCoversNil = false;
        bool armCoversStruct = false;
        const bool armCoversAll = checkPattern(
            *arm.pattern,
            scrutineeType,
            armCoveredVariants,
            armCoveredLiterals,
            armCoversNil,
            armCoversStruct);
        if (!arm.guard) {
            coveredVariants.insert(armCoveredVariants.begin(), armCoveredVariants.end());
            coveredLiterals.insert(armCoveredLiterals.begin(), armCoveredLiterals.end());
            coveredNil = coveredNil || armCoversNil;
            coveredStruct = coveredStruct || armCoversStruct;
            coversAll = coversAll || armCoversAll;
        }
        if (arm.guard) {
            const TypeInfo guardType = checkExpression(*arm.guard);
            declarationIndex_.recordPatternGuard(
                *arm.guard,
                PatternGuardRecord{guardType});
        }
        checkStatement(*arm.body);
        endScope();
    }

    if (!coversAll) {
        if (SemanticTypes::isNullable(scrutineeType) && !coveredNil) {
            throw TypeError(
                Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                    statement.value->span ? statement.value->span->column : 0},
                "non-exhaustive match: missing nil");
        }
        if (enumType) {
            for (const EnumVariantType& variant : enumType->variants) {
                if (coveredVariants.find(variant.name.lexeme) == coveredVariants.end()) {
                    throw TypeError(
                        Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                            statement.value->span ? statement.value->span->column : 0},
                        "non-exhaustive match: missing " + enumName + "."
                            + variant.name.lexeme);
                }
            }
        } else if (primitiveType && primitiveType->kind == StaticType::Bool) {
            if (coveredLiterals.find("true") == coveredLiterals.end()) {
                throw TypeError(
                    Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                        statement.value->span ? statement.value->span->column : 0},
                    "non-exhaustive match: missing true");
            }
            if (coveredLiterals.find("false") == coveredLiterals.end()) {
                throw TypeError(
                    Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                        statement.value->span ? statement.value->span->column : 0},
                    "non-exhaustive match: missing false");
            }
        } else if (primitiveType && primitiveType->kind == StaticType::Nil && !coveredNil) {
            throw TypeError(
                Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                    statement.value->span ? statement.value->span->column : 0},
                "non-exhaustive match: missing nil");
        } else if (primitiveType
            && (primitiveType->kind == StaticType::Number
            || primitiveType->kind == StaticType::String)) {
            throw TypeError(
                Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                    statement.value->span ? statement.value->span->column : 0},
                "non-exhaustive match: missing wildcard or binding pattern");
        } else if (structType && !coveredStruct) {
            throw TypeError(
                Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                    statement.value->span ? statement.value->span->column : 0},
                "non-exhaustive match: missing wildcard, binding, or complete record pattern");
        }
    }

    MatchCoverageRecord coverage;
    coverage.scrutineeType = scrutineeType;
    coverage.nullable = SemanticTypes::isNullable(scrutineeType);
    coverage.coversNil = coveredNil;
    coverage.coversStruct = coveredStruct;
    coverage.coversAll = coversAll;
    coverage.exhaustive = true;
    coverage.coveredVariants.assign(coveredVariants.begin(), coveredVariants.end());
    coverage.coveredLiterals.assign(coveredLiterals.begin(), coveredLiterals.end());
    std::sort(coverage.coveredVariants.begin(), coverage.coveredVariants.end());
    std::sort(coverage.coveredLiterals.begin(), coverage.coveredLiterals.end());
    declarationIndex_.recordMatchCoverage(statement, std::move(coverage));
}

TypeInfo TypeChecker::checkExpression(const Expr& expression)
{
    return checkExpressionInfo(expression).type;
}

TypeChecker::CheckedExpression TypeChecker::checkExpressionInfo(const Expr& expression)
{
    return checkExpressionInfo(expression, nullptr);
}

TypeInfo TypeChecker::variableType(const Binding& binding) const
{
    if (std::optional<TypeInfo> narrowed = flowFacts_.narrowedTypeFor(binding.resolvedName)) {
        return *narrowed;
    }
    return binding.type;
}

std::optional<FlowNarrowing> TypeChecker::nonNilNarrowingForVariable(const VariableExpr& variable) const
{
    const Binding* binding = findVariable(variable.name.lexeme);
    if (!binding || !SemanticTypes::isNullable(binding->type)) {
        return std::nullopt;
    }
    return FlowNarrowing{binding->resolvedName, *binding->type.nullableOf};
}

std::optional<std::string> TypeChecker::fieldFlowFactName(const Expr& object, const Token& name) const
{
    std::optional<std::string> parentFactName;
    TypeInfo objectType = unknownType();

    const auto* variable = dynamic_cast<const VariableExpr*>(&object);
    if (variable) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            return std::nullopt;
        }
        parentFactName = binding->resolvedName;
        objectType = variableType(*binding);
    } else {
        const auto* field = dynamic_cast<const FieldAccessExpr*>(&object);
        if (!field) {
            return std::nullopt;
        }
        parentFactName = fieldFlowFactName(*field->object, field->name);
        if (!parentFactName) {
            return std::nullopt;
        }
        const TypedExpressionRecord* typedObject = declarationIndex_.typedExpression(object);
        if (!typedObject) {
            return std::nullopt;
        }
        objectType = typedObject->type;
    }

    if (objectType.kind != StaticType::Struct || !objectType.structName) {
        return std::nullopt;
    }

    const StructTypeDecl* structType = findStructType(*objectType.structName);
    if (!structType || !findStructField(*structType, name.lexeme)) {
        return std::nullopt;
    }

    return *parentFactName + "." + name.lexeme;
}

std::optional<FlowNarrowing> TypeChecker::nonNilNarrowingForField(const FieldAccessExpr& field) const
{
    const std::optional<std::string> factName = fieldFlowFactName(*field.object, field.name);
    if (!factName) {
        return std::nullopt;
    }

    TypeInfo objectType = unknownType();
    if (const auto* variable = dynamic_cast<const VariableExpr*>(field.object.get())) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            return std::nullopt;
        }
        objectType = variableType(*binding);
    } else if (const TypedExpressionRecord* typedObject = declarationIndex_.typedExpression(*field.object)) {
        objectType = typedObject->type;
    } else {
        return std::nullopt;
    }

    const StructTypeDecl* structType = objectType.structName
        ? findStructType(*objectType.structName)
        : nullptr;
    const StructFieldType* structField = structType
        ? findStructField(*structType, field.name.lexeme)
        : nullptr;
    if (!structType || !structField) {
        return std::nullopt;
    }

    const TypeInfo fieldType = structFieldTypeForValue(objectType, *structType, *structField);
    if (!SemanticTypes::isNullable(fieldType)) {
        return std::nullopt;
    }
    return FlowNarrowing{*factName, *fieldType.nullableOf};
}

std::optional<std::string> TypeChecker::indexFlowFactName(
    const Expr& collection,
    const Expr& index) const
{
    std::optional<std::string> indexFactName = normalizedIntegerLiteral(index);
    if (!indexFactName) {
        const auto* variable = dynamic_cast<const VariableExpr*>(&index);
        if (!variable) {
            return std::nullopt;
        }

        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding || variableType(*binding).kind != StaticType::Number) {
            return std::nullopt;
        }
        indexFactName = binding->resolvedName;
    }

    if (indexFactName->empty()) {
        return std::nullopt;
    }

    std::optional<std::string> parentFactName;
    TypeInfo collectionType = unknownType();
    if (const auto* variable = dynamic_cast<const VariableExpr*>(&collection)) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            return std::nullopt;
        }
        parentFactName = binding->resolvedName;
        collectionType = variableType(*binding);
    } else if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&collection)) {
        parentFactName = fieldFlowFactName(*field->object, field->name);
        const TypedExpressionRecord* typedCollection = declarationIndex_.typedExpression(collection);
        if (!parentFactName || !typedCollection) {
            return std::nullopt;
        }
        collectionType = typedCollection->type;
    } else if (const auto* nestedIndex = dynamic_cast<const IndexExpr*>(&collection)) {
        parentFactName = indexFlowFactName(*nestedIndex->collection, *nestedIndex->index);
        const TypedExpressionRecord* typedCollection = declarationIndex_.typedExpression(collection);
        if (!parentFactName || !typedCollection) {
            return std::nullopt;
        }
        collectionType = typedCollection->type;
    } else {
        return std::nullopt;
    }

    if (collectionType.kind != StaticType::Array || !collectionType.elementType) {
        return std::nullopt;
    }

    return *parentFactName + "[" + *indexFactName + "]";
}

std::optional<FlowNarrowing> TypeChecker::nonNilNarrowingForIndex(const IndexExpr& index) const
{
    const std::optional<std::string> factName = indexFlowFactName(
        *index.collection,
        *index.index);
    if (!factName) {
        return std::nullopt;
    }

    TypeInfo collectionType = unknownType();
    if (const auto* variable = dynamic_cast<const VariableExpr*>(index.collection.get())) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            return std::nullopt;
        }
        collectionType = variableType(*binding);
    } else if (const TypedExpressionRecord* typedCollection = declarationIndex_.typedExpression(*index.collection)) {
        collectionType = typedCollection->type;
    } else {
        return std::nullopt;
    }

    if (collectionType.kind != StaticType::Array || !collectionType.elementType
        || !SemanticTypes::isNullable(*collectionType.elementType)) {
        return std::nullopt;
    }

    return FlowNarrowing{*factName, *collectionType.elementType->nullableOf};
}

std::optional<FlowNarrowing> TypeChecker::nonNilNarrowingForTarget(const Expr& target) const
{
    if (const auto* variable = dynamic_cast<const VariableExpr*>(&target)) {
        return nonNilNarrowingForVariable(*variable);
    }
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&target)) {
        return nonNilNarrowingForField(*field);
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(&target)) {
        return nonNilNarrowingForIndex(*index);
    }
    return std::nullopt;
}

TypeInfo TypeChecker::inferArrayElementType(const ArrayExpr& expression)
{
    std::optional<TypeInfo> current;
    for (const auto& element : expression.elements) {
        TypeInfo elementType = checkExpression(*element);
        if (!SemanticTypes::isKnown(elementType)) {
            return unknownType();
        }
        if (!current) {
            current = std::move(elementType);
            continue;
        }
        std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*current, elementType);
        if (!merged) {
            return unknownType();
        }
        current = std::move(*merged);
    }
    return current ? *current : unknownType();
}

void TypeChecker::refineArrayBindingFromMutation(Binding& target, const TypeInfo& valueType)
{
    if (target.explicitType) {
        return;
    }

    if (!SemanticTypes::isKnown(valueType)) {
        target.type = simpleType(StaticType::Array);
        return;
    }

    if (!SemanticTypes::isKnown(target.type) || (target.type.kind == StaticType::Array && !target.type.elementType)) {
        target.type = arrayType(valueType);
        return;
    }

    if (target.type.kind != StaticType::Array) {
        return;
    }

    if (!target.type.elementType) {
        target.type = arrayType(valueType);
        return;
    }

    std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*target.type.elementType, valueType);
    if (!merged) {
        target.type = simpleType(StaticType::Array);
        return;
    }

    target.type = arrayType(std::move(*merged));
}

TypeChecker::CheckedExpression TypeChecker::checkArrayLiteral(const ArrayExpr& expression, const TypeInfo* expectedType)
{
    if (expectedType && expectedType->kind == StaticType::Array && expectedType->elementType) {
        for (const auto& element : expression.elements) {
            const CheckedExpression actual = checkExpressionInfo(*element, expectedType->elementType.get());
            if (!SemanticTypes::compatible(*expectedType->elementType, actual.type)) {
                throw TypeError(expression.bracket,
                    "array element expects " + typeInfoName(*expectedType->elementType)
                        + ", got " + typeInfoName(actual.type));
            }
        }
        return CheckedExpression{*expectedType};
    }

    const TypeInfo element = inferArrayElementType(expression);
    if (SemanticTypes::isKnown(element)) {
        return CheckedExpression{arrayType(element)};
    }
    return CheckedExpression{simpleType(StaticType::Array)};
}

TypeInfo TypeChecker::inferMapType(const MapExpr& expression)
{
    std::optional<TypeInfo> keyType;
    std::optional<TypeInfo> valueType;
    bool hasUnknownComponent = false;

    for (const MapEntry& entry : expression.entries) {
        const TypeInfo currentKey = checkExpression(*entry.key);
        if (SemanticTypes::isKnown(currentKey) && !mapKeyTypeAllowed(currentKey)) {
            throw TypeError(entry.colon, "map key must be nil, number, bool, or string");
        }
        const TypeInfo currentValue = checkExpression(*entry.value);

        if (!SemanticTypes::isKnown(currentKey) || !SemanticTypes::isKnown(currentValue)) {
            hasUnknownComponent = true;
            continue;
        }

        if (!keyType) {
            keyType = currentKey;
        } else {
            std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*keyType, currentKey);
            if (!merged) {
                hasUnknownComponent = true;
            } else {
                keyType = std::move(*merged);
            }
        }

        if (!valueType) {
            valueType = currentValue;
        } else {
            std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*valueType, currentValue);
            if (!merged) {
                hasUnknownComponent = true;
            } else {
                valueType = std::move(*merged);
            }
        }
    }

    if (hasUnknownComponent || !keyType || !valueType) {
        return simpleType(StaticType::Map);
    }
    return mapType(std::move(*keyType), std::move(*valueType));
}

TypeChecker::CheckedExpression TypeChecker::checkMapLiteral(
    const MapExpr& expression,
    const TypeInfo* expectedType)
{
    if (expectedType && expectedType->kind == StaticType::Map
        && expectedType->keyType && expectedType->valueType) {
        if (!mapKeyTypeAllowed(*expectedType->keyType)) {
            throw TypeError(expression.brace, "map key must be nil, number, bool, or string");
        }
        for (const MapEntry& entry : expression.entries) {
            const CheckedExpression key = checkExpressionInfo(*entry.key, expectedType->keyType.get());
            if (SemanticTypes::isKnown(key.type) && !mapKeyTypeAllowed(key.type)) {
                throw TypeError(entry.colon, "map key must be nil, number, bool, or string");
            }
            if (!SemanticTypes::compatible(*expectedType->keyType, key.type)) {
                throw TypeError(entry.colon, "map key is incompatible with map key type");
            }

            const CheckedExpression value = checkExpressionInfo(*entry.value, expectedType->valueType.get());
            if (!SemanticTypes::compatible(*expectedType->valueType, value.type)) {
                throw TypeError(entry.colon, "map value is incompatible with map value type");
            }
        }
        return CheckedExpression{*expectedType};
    }

    return CheckedExpression{inferMapType(expression)};
}

TypeChecker::CheckedExpression TypeChecker::checkExpressionInfo(const Expr& expression, const TypeInfo* expectedType)
{
    if (const auto* literal = dynamic_cast<const LiteralExpr*>(&expression)) {
        if (literal->value == "nil") {
            return CheckedExpression{simpleType(StaticType::Nil)};
        }
        if (literal->value == "true" || literal->value == "false") {
            return CheckedExpression{simpleType(StaticType::Bool)};
        }
        if (literal->value.size() >= 2 && literal->value.front() == '"' && literal->value.back() == '"') {
            return CheckedExpression{simpleType(StaticType::String)};
        }
        return CheckedExpression{simpleType(StaticType::Number)};
    }

    if (const auto* function = dynamic_cast<const FunctionExpr*>(&expression)) {
        CheckedExpression result = checkFunctionExpression(*function, expectedType);
        declarationIndex_.recordTypedExpression(*function, result.type);
        return result;
    }

    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            if (findNamespace(variable->name.lexeme)) {
                throw TypeError(variable->name, "namespace alias `" + variable->name.lexeme + "` is not a value");
            }
            throw TypeError(variable->name, "undefined variable `" + variable->name.lexeme + "`");
        }
        declarationIndex_.recordVariableBinding(
            *variable,
            BindingMetadataRecord{
                binding->resolvedName,
                binding->bindingId,
                ResolvedSymbol{binding->declarationId, binding->symbolId},
                binding->range});
        CheckedExpression result{variableType(*binding)};
        declarationIndex_.recordTypedExpression(*variable, result.type);
        return result;
    }

    if (const auto* assign = dynamic_cast<const AssignExpr*>(&expression)) {
        Binding* target = findVariable(assign->name.lexeme);
        if (!target) {
            if (findNamespace(assign->name.lexeme)) {
                throw TypeError(assign->name, "cannot assign to namespace alias `" + assign->name.lexeme + "`");
            }
            throw TypeError(assign->name, "undefined variable `" + assign->name.lexeme + "`");
        }

        const CheckedExpression value = checkExpressionInfo(*assign->value, &target->type);

        if (target->type.kind == StaticType::Function && value.type.kind == StaticType::Function) {
            if (target->explicitType
                && target->type.genericParameters.empty()
                && !value.type.genericParameters.empty()) {
                throw TypeError(assign->name,
                    "cannot assign generic function to monomorphic function type");
            }
            if (SemanticTypes::hasFunctionSignature(target->type) && SemanticTypes::hasFunctionSignature(value.type)
                && target->type.parameterTypes.size() != value.type.parameterTypes.size()) {
                throw TypeError(assign->name,
                    "cannot assign function with " + std::to_string(value.type.parameterTypes.size())
                        + " parameters to `" + assign->name.lexeme
                        + "` of type function with " + std::to_string(target->type.parameterTypes.size()) + " parameters");
            }

            if (target->explicitType && !SemanticTypes::compatible(target->type, value.type)) {
                throw TypeError(assign->name, "cannot assign " + typeInfoName(value.type) + " to `" + assign->name.lexeme
                    + "` of type " + typeInfoName(target->type));
            }

            if (!target->explicitType) {
                target->type = value.type;
            }
        } else if (!SemanticTypes::compatible(target->type, value.type)) {
            const std::string targetTypeName = target->type.kind == StaticType::Function && !target->explicitType
                ? staticTypeName(StaticType::Function)
                : typeInfoName(target->type);
            throw TypeError(assign->name, "cannot assign " + typeInfoName(value.type) + " to `" + assign->name.lexeme
                + "` of type " + targetTypeName);
        } else if (!SemanticTypes::isKnown(target->type)) {
            target->type = value.type;
        }

        flowFacts_.invalidate(target->resolvedName);

        declarationIndex_.recordAssignmentBinding(
            *assign,
            BindingMetadataRecord{
                target->resolvedName,
                target->bindingId,
                ResolvedSymbol{target->declarationId, target->symbolId},
                target->range});
        CheckedExpression result{target->type};
        declarationIndex_.recordTypedExpression(*assign, result.type);
        return result;
    }

    if (const auto* compound = dynamic_cast<const CompoundAssignExpr*>(&expression)) {
        Binding* target = findVariable(compound->name.lexeme);
        if (!target) {
            if (findNamespace(compound->name.lexeme)) {
                throw TypeError(compound->name, "cannot assign to namespace alias `" + compound->name.lexeme + "`");
            }
            throw TypeError(compound->name, "undefined variable `" + compound->name.lexeme + "`");
        }

        const CheckedExpression value = checkExpressionInfo(*compound->value);
        checkKnownNumber(compound->op, target->type, "`" + compound->op.lexeme + "` expects number variable, got ");
        checkKnownNumber(compound->op, value.type, "`" + compound->op.lexeme + "` expects number value, got ");

        if (!SemanticTypes::isKnown(target->type)) {
            target->type = simpleType(StaticType::Number);
        }
        flowFacts_.invalidate(target->resolvedName);
        declarationIndex_.recordCompoundAssignmentBinding(
            *compound,
            BindingMetadataRecord{
                target->resolvedName,
                target->bindingId,
                ResolvedSymbol{target->declarationId, target->symbolId},
                target->range});
        CheckedExpression result{simpleType(StaticType::Number)};
        declarationIndex_.recordTypedExpression(*compound, result.type);
        return result;
    }

    if (const auto* grouping = dynamic_cast<const GroupingExpr*>(&expression)) {
        return checkExpressionInfo(*grouping->expression);
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        return CheckedExpression{checkUnary(*unary)};
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        return CheckedExpression{checkBinary(*binary)};
    }

    if (const auto* logical = dynamic_cast<const LogicalExpr*>(&expression)) {
        const TypeInfo left = checkExpression(*logical->left);
        const TypeInfo right = checkExpression(*logical->right);
        return CheckedExpression{logicalResultType(left, right)};
    }

    if (const auto* match = dynamic_cast<const MatchExpr*>(&expression)) {
        return checkMatchExpression(*match, expectedType);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
        CheckedExpression result = checkCall(*call);
        if (isNativeStdlibCall(*call)) {
            const auto* variable = dynamic_cast<const VariableExpr*>(call->callee.get());
            if (variable) {
                declarationIndex_.recordNativeCall(*call, variable->name.lexeme);
            }
        }
        declarationIndex_.recordTypedExpression(*call, result.type);
        return result;
    }

    if (const auto* memberCall = dynamic_cast<const MemberCallExpr*>(&expression)) {
        CheckedExpression result = checkMemberCall(*memberCall, expectedType);
        invalidateStructMethodEffects(*memberCall);
        if (isNativeCallbackName(memberCall->name.lexeme)
            && !declarationIndex_.memberCallMetadata(*memberCall)
            && !declarationIndex_.variantConstructor(*memberCall)) {
            flowFacts_.invalidateAll();
        }
        if (isNativeStdlibName(memberCall->name.lexeme)
            && !declarationIndex_.memberCallMetadata(*memberCall)
            && !declarationIndex_.variantConstructor(*memberCall)) {
            declarationIndex_.recordNativeCall(*memberCall, memberCall->name.lexeme);
        }
        declarationIndex_.recordTypedExpression(*memberCall, result.type);
        return result;
    }

    if (const auto* array = dynamic_cast<const ArrayExpr*>(&expression)) {
        CheckedExpression result = checkArrayLiteral(*array, expectedType);
        declarationIndex_.recordTypedExpression(*array, result.type);
        return result;
    }

    if (const auto* map = dynamic_cast<const MapExpr*>(&expression)) {
        CheckedExpression result = checkMapLiteral(*map, expectedType);
        declarationIndex_.recordTypedExpression(*map, result.type);
        return result;
    }

    if (const auto* construct = dynamic_cast<const StructConstructExpr*>(&expression)) {
        CheckedExpression result = checkStructConstructor(*construct, expectedType);
        declarationIndex_.recordTypedExpression(*construct, result.type);
        std::vector<std::string> fieldNames;
        fieldNames.reserve(construct->fields.size());
        for (const StructField& field : construct->fields) {
            fieldNames.push_back(field.name.lexeme);
        }
        declarationIndex_.recordStructConstructor(
            *construct,
            StructConstructorRecord{result.type, std::move(fieldNames)});
        return result;
    }

    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expression)) {
        if (const auto* variable = dynamic_cast<const VariableExpr*>(field->object.get())) {
            if (const NamespaceImport* namespaceImport = findNamespace(variable->name.lexeme)) {
                const auto found = namespaceImport->values.find(field->name.lexeme);
                if (found == namespaceImport->values.end()) {
                    throw TypeError(field->name,
                        "module namespace `" + variable->name.lexeme + "` has no exported member `" + field->name.lexeme + "`");
                }
                CheckedExpression result{found->second.type};
                declarationIndex_.recordTypedExpression(*field, result.type);
                declarationIndex_.recordFieldOperation(
                    *field,
                    FieldOperationRecord{
                        FieldOperationKind::Read,
                        field->name.lexeme,
                        result.type,
                        result.type,
                        found->second.resolvedName});
                return result;
            }
        }
        const TypeInfo object = checkExpression(*field->object);
        if (object.kind != StaticType::Unknown && object.kind != StaticType::Struct) {
            throw TypeError(field->name, "can only access fields on structs");
        }
        if (object.kind == StaticType::Struct && object.structName) {
            const StructTypeDecl* structType = findStructType(*object.structName);
            const StructFieldType* structField = structType ? findStructField(*structType, field->name.lexeme) : nullptr;
            if (!structField) {
                throw TypeError(field->name,
                    "struct `" + unqualifiedStructName(*object.structName)
                        + (structType && structType->hasPrivateFields
                                ? "` has no accessible field `"
                                : "` has no field `")
                        + field->name.lexeme + "`");
            }
            if (structField->isPrivate && !canAccessPrivateFields(*structType)) {
                throw TypeError(field->name,
                    "struct `" + unqualifiedStructName(*object.structName) + "` has no accessible field `"
                        + field->name.lexeme + "`");
            }
            const TypeInfo declaredFieldType = structFieldTypeForValue(object, *structType, *structField);
            TypeInfo resultType = declaredFieldType;
            if (const std::optional<std::string> factName = fieldFlowFactName(*field->object, field->name)) {
                if (const std::optional<TypeInfo> narrowed = flowFacts_.narrowedTypeFor(*factName)) {
                    resultType = *narrowed;
                }
            }
            CheckedExpression result{resultType};
            declarationIndex_.recordTypedExpression(*field, result.type);
            declarationIndex_.recordFieldOperation(
                *field,
                FieldOperationRecord{
                    FieldOperationKind::Read,
                    field->name.lexeme,
                    declaredFieldType,
                    result.type,
                    std::nullopt});
            return result;
        }
        CheckedExpression result{unknownType()};
        declarationIndex_.recordTypedExpression(*field, result.type);
        declarationIndex_.recordFieldOperation(
            *field,
            FieldOperationRecord{
                FieldOperationKind::Read,
                field->name.lexeme,
                unknownType(),
                result.type,
                std::nullopt});
        return result;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignExpr*>(&expression)) {
        CheckedExpression result = checkFieldAssignment(*fieldAssign);
        declarationIndex_.recordTypedExpression(*fieldAssign, result.type);
        return result;
    }

    if (const auto* fieldCompound = dynamic_cast<const FieldCompoundAssignExpr*>(&expression)) {
        CheckedExpression result = checkFieldCompoundAssignment(*fieldCompound);
        declarationIndex_.recordTypedExpression(*fieldCompound, result.type);
        return result;
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
        CheckedExpression result{checkIndex(*index)};
        declarationIndex_.recordTypedExpression(*index, result.type);
        return result;
    }

    if (const auto* indexAssign = dynamic_cast<const IndexAssignExpr*>(&expression)) {
        CheckedExpression result = checkIndexAssignment(*indexAssign);
        declarationIndex_.recordTypedExpression(*indexAssign, result.type);
        return result;
    }

    if (const auto* indexCompound = dynamic_cast<const IndexCompoundAssignExpr*>(&expression)) {
        CheckedExpression result = checkIndexCompoundAssignment(*indexCompound);
        declarationIndex_.recordTypedExpression(*indexCompound, result.type);
        return result;
    }

    throw TypeError("unsupported expression node");
}

TypeChecker::CheckedExpression TypeChecker::checkMatchExpression(
    const MatchExpr& expression,
    const TypeInfo* expectedType)
{
    const TypeInfo scrutineeType = checkExpression(*expression.value);
    const bool nullableEnum = SemanticTypes::isNullable(scrutineeType)
        && scrutineeType.nullableOf->kind == StaticType::Enum
        && scrutineeType.nullableOf->enumName;
    const bool nullableStruct = SemanticTypes::isNullable(scrutineeType)
        && scrutineeType.nullableOf->kind == StaticType::Struct
        && scrutineeType.nullableOf->structName;
    const bool structValue = scrutineeType.kind == StaticType::Struct
        && scrutineeType.structName;
    const TypeInfo* primitiveType = primitiveMatchBaseType(scrutineeType);
    if ((scrutineeType.kind != StaticType::Enum || !scrutineeType.enumName)
        && !nullableEnum && !structValue && !nullableStruct && !primitiveType) {
        throw TypeError(expression.keyword,
            "match expects enum, struct, bool, number, string, or nil value, got "
                + typeInfoName(scrutineeType));
    }

    std::string enumName;
    const EnumTypeDecl* enumType = nullptr;
    if (scrutineeType.kind == StaticType::Enum || nullableEnum) {
        enumName = nullableEnum
            ? *scrutineeType.nullableOf->enumName
            : *scrutineeType.enumName;
        enumType = findEnumType(enumName);
        if (!enumType) {
            throw TypeError(expression.keyword, "unknown enum type " + enumName);
        }
    }
    std::string structName;
    const StructTypeDecl* structType = nullptr;
    if (structValue || nullableStruct) {
        structName = nullableStruct
            ? *scrutineeType.nullableOf->structName
            : *scrutineeType.structName;
        structType = findStructType(structName);
        if (!structType) {
            throw TypeError(expression.keyword, "unknown struct type " + structName);
        }
    }

    std::unordered_set<std::string> coveredVariants;
    std::unordered_set<std::string> coveredLiterals;
    bool coveredNil = false;
    bool coveredStruct = false;
    bool coversAll = false;
    std::optional<TypeInfo> resultType;
    for (const MatchExprArm& arm : expression.arms) {
        beginScope();
        std::unordered_set<std::string> armCoveredVariants;
        std::unordered_set<std::string> armCoveredLiterals;
        bool armCoversNil = false;
        bool armCoversStruct = false;
        const bool armCoversAll = checkPattern(
            *arm.pattern,
            scrutineeType,
            armCoveredVariants,
            armCoveredLiterals,
            armCoversNil,
            armCoversStruct);
        if (!arm.guard) {
            coveredVariants.insert(armCoveredVariants.begin(), armCoveredVariants.end());
            coveredLiterals.insert(armCoveredLiterals.begin(), armCoveredLiterals.end());
            coveredNil = coveredNil || armCoversNil;
            coveredStruct = coveredStruct || armCoversStruct;
            coversAll = coversAll || armCoversAll;
        }
        if (arm.guard) {
            const TypeInfo guardType = checkExpression(*arm.guard);
            declarationIndex_.recordPatternGuard(
                *arm.guard,
                PatternGuardRecord{guardType});
        }

        const CheckedExpression result = checkExpressionInfo(*arm.value, expectedType);
        if (expectedType && !SemanticTypes::compatible(*expectedType, result.type)) {
            throw TypeError(arm.arrow,
                "match arm result expects " + typeInfoName(*expectedType)
                    + ", got " + typeInfoName(result.type));
        }
        if (!resultType) {
            resultType = result.type;
        } else if (SemanticTypes::isKnown(*resultType) && SemanticTypes::isKnown(result.type)
            && (!SemanticTypes::compatible(*resultType, result.type) || !SemanticTypes::compatible(result.type, *resultType))) {
            throw TypeError(arm.arrow,
                "match arm result expects " + typeInfoName(*resultType)
                    + ", got " + typeInfoName(result.type));
        } else {
            resultType = mergeReturnTypes(*resultType, result.type);
        }
        endScope();
    }

    if (!coversAll) {
        if (SemanticTypes::isNullable(scrutineeType) && !coveredNil) {
            throw TypeError(expression.keyword, "non-exhaustive match: missing nil");
        }
        if (enumType) {
            for (const EnumVariantType& variant : enumType->variants) {
                if (coveredVariants.find(variant.name.lexeme) == coveredVariants.end()) {
                    throw TypeError(expression.keyword,
                        "non-exhaustive match: missing " + enumName
                            + "." + variant.name.lexeme);
                }
            }
        } else if (primitiveType && primitiveType->kind == StaticType::Bool) {
            if (coveredLiterals.find("true") == coveredLiterals.end()) {
                throw TypeError(expression.keyword,
                    "non-exhaustive match: missing true");
            }
            if (coveredLiterals.find("false") == coveredLiterals.end()) {
                throw TypeError(expression.keyword,
                    "non-exhaustive match: missing false");
            }
        } else if (primitiveType && primitiveType->kind == StaticType::Nil && !coveredNil) {
            throw TypeError(expression.keyword, "non-exhaustive match: missing nil");
        } else if (primitiveType
            && (primitiveType->kind == StaticType::Number
            || primitiveType->kind == StaticType::String)) {
            throw TypeError(
                expression.keyword,
                "non-exhaustive match: missing wildcard or binding pattern");
        } else if (structType && !coveredStruct) {
            throw TypeError(
                expression.keyword,
                "non-exhaustive match: missing wildcard, binding, or complete record pattern");
        }
    }

    MatchCoverageRecord coverage;
    coverage.scrutineeType = scrutineeType;
    coverage.nullable = SemanticTypes::isNullable(scrutineeType);
    coverage.coversNil = coveredNil;
    coverage.coversStruct = coveredStruct;
    coverage.coversAll = coversAll;
    coverage.exhaustive = true;
    coverage.coveredVariants.assign(coveredVariants.begin(), coveredVariants.end());
    coverage.coveredLiterals.assign(coveredLiterals.begin(), coveredLiterals.end());
    std::sort(coverage.coveredVariants.begin(), coverage.coveredVariants.end());
    std::sort(coverage.coveredLiterals.begin(), coverage.coveredLiterals.end());
    declarationIndex_.recordMatchCoverage(expression, std::move(coverage));

    return CheckedExpression{expectedType
        ? *expectedType
        : (resultType ? *resultType : unknownType())};
}

bool TypeChecker::isBuiltinLenCall(const CallExpr& expression) const
{
    const auto* variable = dynamic_cast<const VariableExpr*>(expression.callee.get());
    return variable && variable->name.lexeme == "len" && findVariable("len") == nullptr;
}

TypeChecker::CheckedExpression TypeChecker::checkBuiltinLenCall(const CallExpr& expression)
{
    if (!expression.typeArguments.empty()) {
        throw TypeError(expression.paren, "function is not generic");
    }
    if (expression.arguments.size() != 1) {
        throw TypeError(expression.paren, "expected 1 arguments but got " + std::to_string(expression.arguments.size()));
    }

    const CheckedExpression argument = checkExpressionInfo(*expression.arguments.front());
    if (SemanticTypes::isKnown(argument.type)
        && argument.type.kind != StaticType::Array
        && argument.type.kind != StaticType::String
        && argument.type.kind != StaticType::Map
        && argument.type.kind != StaticType::Range) {
        throw TypeError(expression.paren, "len expects array, string, map, or range, got " + typeInfoName(argument.type));
    }

    return CheckedExpression{simpleType(StaticType::Number)};
}

bool TypeChecker::isNativeStdlibCall(const CallExpr& expression) const
{
    const auto* variable = dynamic_cast<const VariableExpr*>(expression.callee.get());
    return variable && isNativeStdlibName(variable->name.lexeme) && findVariable(variable->name.lexeme) == nullptr;
}

TypeChecker::CheckedExpression TypeChecker::checkArrayMap(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& callbackExpression)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken, "map expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const TypeInfo expectedCallback = functionType({elementType}, unknownType());
    const CheckedExpression callback = checkExpressionInfo(callbackExpression, &expectedCallback);
    if (callback.type.kind != StaticType::Unknown && callback.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            "map expects function as second argument, got " + typeInfoName(callback.type));
    }
    if (callback.type.kind != StaticType::Function || !SemanticTypes::hasFunctionSignature(callback.type)) {
        return CheckedExpression{simpleType(StaticType::Array)};
    }
    if (callback.type.parameterTypes.size() != 1) {
        throw TypeError(callToken, "map expects callback with 1 argument");
    }
    const TypeInfo callbackType = specializeGenericCallback(
        callToken, callback.type, {elementType}, "map");
    if (elementType.kind != StaticType::Unknown
        && !SemanticTypes::compatible(callbackType.parameterTypes.front(), elementType)) {
        throw TypeError(callToken,
            "map callback expects " + typeInfoName(elementType)
                + ", got " + typeInfoName(callbackType.parameterTypes.front()));
    }
    if (callbackType.returnType && SemanticTypes::isKnown(*callbackType.returnType)) {
        return CheckedExpression{arrayType(*callbackType.returnType)};
    }
    return CheckedExpression{simpleType(StaticType::Array)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayFilter(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken, "filter expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const TypeInfo expectedPredicate = functionType({elementType}, simpleType(StaticType::Bool));
    const CheckedExpression predicate = checkExpressionInfo(predicateExpression, &expectedPredicate);
    if (predicate.type.kind != StaticType::Unknown && predicate.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            "filter expects function as second argument, got " + typeInfoName(predicate.type));
    }
    if (predicate.type.kind == StaticType::Function && SemanticTypes::hasFunctionSignature(predicate.type)) {
        if (predicate.type.parameterTypes.size() != 1) {
            throw TypeError(callToken, "filter expects callback with 1 argument");
        }
        const TypeInfo predicateType = specializeGenericCallback(
            callToken, predicate.type, {elementType}, "filter");
        if (elementType.kind != StaticType::Unknown
            && !SemanticTypes::compatible(predicateType.parameterTypes.front(), elementType)) {
            throw TypeError(callToken,
                "filter callback expects " + typeInfoName(elementType)
                    + ", got " + typeInfoName(predicateType.parameterTypes.front()));
        }
        if (predicateType.returnType
            && SemanticTypes::isKnown(*predicateType.returnType)
            && !SemanticTypes::compatible(simpleType(StaticType::Bool), *predicateType.returnType)) {
            throw TypeError(callToken,
                "filter expects callback to return bool, got " + typeInfoName(*predicateType.returnType));
        }
    }

    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        return CheckedExpression{arrayType(*arrayTypeInfo.elementType)};
    }
    return CheckedExpression{simpleType(StaticType::Array)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayFlatMap(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& callbackExpression)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken, "flatMap expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const TypeInfo expectedCallback = functionType({elementType}, simpleType(StaticType::Array));
    const CheckedExpression callback = checkExpressionInfo(callbackExpression, &expectedCallback);
    if (callback.type.kind != StaticType::Unknown && callback.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            "flatMap expects function as second argument, got " + typeInfoName(callback.type));
    }
    if (callback.type.kind != StaticType::Function || !SemanticTypes::hasFunctionSignature(callback.type)) {
        return CheckedExpression{simpleType(StaticType::Array)};
    }
    if (callback.type.parameterTypes.size() != 1) {
        throw TypeError(callToken, "flatMap expects callback with 1 argument");
    }
    const TypeInfo callbackType = specializeGenericCallback(
        callToken, callback.type, {elementType}, "flatMap");
    if (elementType.kind != StaticType::Unknown
        && !SemanticTypes::compatible(callbackType.parameterTypes.front(), elementType)) {
        throw TypeError(callToken,
            "flatMap callback expects " + typeInfoName(elementType)
                + ", got " + typeInfoName(callbackType.parameterTypes.front()));
    }
    if (callbackType.returnType && SemanticTypes::isKnown(*callbackType.returnType)) {
        if (callbackType.returnType->kind != StaticType::Array) {
            throw TypeError(callToken,
                "flatMap expects callback to return array, got "
                    + typeInfoName(*callbackType.returnType));
        }
        if (callbackType.returnType->elementType) {
            return CheckedExpression{arrayType(*callbackType.returnType->elementType)};
        }
    }
    return CheckedExpression{simpleType(StaticType::Array)};
}

void TypeChecker::checkArrayPredicate(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression,
    const std::string& functionName)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken,
            functionName + " expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const TypeInfo expectedPredicate = functionType({elementType}, simpleType(StaticType::Bool));
    const CheckedExpression predicate = checkExpressionInfo(predicateExpression, &expectedPredicate);
    if (predicate.type.kind != StaticType::Unknown && predicate.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            functionName + " expects function as second argument, got " + typeInfoName(predicate.type));
    }
    if (predicate.type.kind == StaticType::Function && SemanticTypes::hasFunctionSignature(predicate.type)) {
        if (predicate.type.parameterTypes.size() != 1) {
            throw TypeError(callToken, functionName + " expects callback with 1 argument");
        }
        const TypeInfo predicateType = specializeGenericCallback(
            callToken, predicate.type, {elementType}, functionName);
        if (elementType.kind != StaticType::Unknown
            && !SemanticTypes::compatible(predicateType.parameterTypes.front(), elementType)) {
            throw TypeError(callToken,
                functionName + " callback expects " + typeInfoName(elementType)
                    + ", got " + typeInfoName(predicateType.parameterTypes.front()));
        }
        if (predicateType.returnType
            && SemanticTypes::isKnown(*predicateType.returnType)
            && !SemanticTypes::compatible(simpleType(StaticType::Bool), *predicateType.returnType)) {
            throw TypeError(callToken,
                functionName + " expects callback to return bool, got "
                    + typeInfoName(*predicateType.returnType));
        }
    }

}

TypeChecker::CheckedExpression TypeChecker::checkArrayAnyAll(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression,
    const std::string& functionName)
{
    checkArrayPredicate(callToken, arrayTypeInfo, predicateExpression, functionName);
    return CheckedExpression{simpleType(StaticType::Bool)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayCount(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression)
{
    checkArrayPredicate(callToken, arrayTypeInfo, predicateExpression, "count");
    return CheckedExpression{simpleType(StaticType::Number)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayFind(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression)
{
    checkArrayPredicate(callToken, arrayTypeInfo, predicateExpression, "find");
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        if (SemanticTypes::isNullable(*arrayTypeInfo.elementType)) {
            return CheckedExpression{*arrayTypeInfo.elementType};
        }
        return CheckedExpression{nullableType(*arrayTypeInfo.elementType)};
    }
    return CheckedExpression{unknownType()};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayFindIndex(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression)
{
    checkArrayPredicate(callToken, arrayTypeInfo, predicateExpression, "findIndex");
    return CheckedExpression{simpleType(StaticType::Number)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayReduce(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& initialExpression,
    const Expr& callbackExpression)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken, "reduce expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const CheckedExpression initial = checkExpressionInfo(initialExpression);
    const TypeInfo expectedCallback = functionType(
        {initial.type, elementType}, initial.type);
    const CheckedExpression callback = checkExpressionInfo(callbackExpression, &expectedCallback);
    if (callback.type.kind != StaticType::Unknown && callback.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            "reduce expects function as third argument, got " + typeInfoName(callback.type));
    }
    if (callback.type.kind == StaticType::Function && SemanticTypes::hasFunctionSignature(callback.type)) {
        if (callback.type.parameterTypes.size() != 2) {
            throw TypeError(callToken, "reduce expects callback with 2 arguments");
        }
        const TypeInfo callbackType = specializeGenericCallback(
            callToken, callback.type, {initial.type, elementType}, "reduce");
        if (initial.type.kind != StaticType::Unknown
            && !SemanticTypes::compatible(callbackType.parameterTypes.front(), initial.type)) {
            throw TypeError(callToken,
                "reduce callback accumulator expects " + typeInfoName(initial.type)
                    + ", got " + typeInfoName(callbackType.parameterTypes.front()));
        }
        if (elementType.kind != StaticType::Unknown
            && !SemanticTypes::compatible(callbackType.parameterTypes[1], elementType)) {
            throw TypeError(callToken,
                "reduce callback element expects " + typeInfoName(elementType)
                    + ", got " + typeInfoName(callbackType.parameterTypes[1]));
        }
        if (callbackType.returnType
            && SemanticTypes::isKnown(*callbackType.returnType)
            && !SemanticTypes::compatible(initial.type, *callbackType.returnType)) {
            throw TypeError(callToken,
                "reduce expects callback to return " + typeInfoName(initial.type)
                    + ", got " + typeInfoName(*callbackType.returnType));
        }
    }

    return CheckedExpression{initial.type};
}

TypeChecker::CheckedExpression TypeChecker::checkMapMerge(
    const Token& callToken,
    const TypeInfo& leftType,
    const TypeInfo& rightType)
{
    if (leftType.kind != StaticType::Unknown && leftType.kind != StaticType::Map) {
        throw TypeError(callToken, "merge expects map as first argument, got " + typeInfoName(leftType));
    }
    if (rightType.kind != StaticType::Unknown && rightType.kind != StaticType::Map) {
        throw TypeError(callToken, "merge expects map as second argument, got " + typeInfoName(rightType));
    }
    return CheckedExpression{mergedMapType(leftType, rightType)};
}

TypeChecker::CheckedExpression TypeChecker::checkNativeStdlibCall(const CallExpr& expression)
{
    if (!expression.typeArguments.empty()) {
        throw TypeError(expression.paren, "function is not generic");
    }
    const auto* variable = dynamic_cast<const VariableExpr*>(expression.callee.get());
    if (!variable) {
        throw TypeError("native stdlib call missing variable callee");
    }

    const NativeFunctionSignature* function = findNativeStdlibFunction(variable->name.lexeme);
    if (!function) {
        throw TypeError(variable->name, "unknown native stdlib function `" + variable->name.lexeme + "`");
    }
    const bool validArity = function->kind == NativeFunctionKind::Range
        ? expression.arguments.size() >= function->arity
            && expression.arguments.size() <= function->maxArity
        : expression.arguments.size() == function->arity;
    if (!validArity) {
        std::string expectedArity = std::to_string(function->arity);
        if (function->maxArity != 0) {
            expectedArity += " to " + std::to_string(function->maxArity);
        }
        throw TypeError(expression.paren,
            "expected " + expectedArity + " arguments but got " + std::to_string(expression.arguments.size()));
    }

    switch (function->kind) {
    case NativeFunctionKind::Push: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        if (arrayArgument.type.kind != StaticType::Unknown && arrayArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "push expects array as first argument, got " + typeInfoName(arrayArgument.type));
        }

        Binding* target = findSimpleVariableBinding(*expression.arguments[0]);
        const bool strictElementCheck = target == nullptr || target->explicitType;
        const TypeInfo* expectedElement = strictElementCheck ? arrayArgument.type.elementType.get() : nullptr;
        const CheckedExpression valueArgument = checkExpressionInfo(*expression.arguments[1], expectedElement);
        if (strictElementCheck && expectedElement && !SemanticTypes::compatible(*expectedElement, valueArgument.type)) {
            throw TypeError(expression.paren,
                "push value expects " + typeInfoName(*expectedElement)
                    + ", got " + typeInfoName(valueArgument.type));
        }
        if (target && target->type.kind == StaticType::Array) {
            refineArrayBindingFromMutation(*target, valueArgument.type);
        }
        return CheckedExpression{simpleType(StaticType::Nil)};
    }
    case NativeFunctionKind::Pop: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        if (arrayArgument.type.kind != StaticType::Unknown && arrayArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "pop expects array as first argument, got " + typeInfoName(arrayArgument.type));
        }
        if (arrayArgument.type.kind == StaticType::Array && arrayArgument.type.elementType) {
            return CheckedExpression{*arrayArgument.type.elementType};
        }
        return CheckedExpression{unknownType()};
    }
    case NativeFunctionKind::Remove: {
        const CheckedExpression mapArgument = checkExpressionInfo(*expression.arguments[0]);
        if (mapArgument.type.kind != StaticType::Unknown && mapArgument.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                "remove expects map as first argument, got " + typeInfoName(mapArgument.type));
        }

        const TypeInfo* expectedKey = mapArgument.type.kind == StaticType::Map
            ? mapArgument.type.keyType.get()
            : nullptr;
        const CheckedExpression keyArgument = checkExpressionInfo(*expression.arguments[1], expectedKey);
        if (mapArgument.type.kind == StaticType::Map
            && SemanticTypes::isKnown(keyArgument.type)
            && !mapKeyTypeAllowed(keyArgument.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !SemanticTypes::compatible(*expectedKey, keyArgument.type)) {
            throw TypeError(expression.paren, "map key is incompatible with map key type");
        }
        if (mapArgument.type.kind == StaticType::Map && mapArgument.type.valueType) {
            return CheckedExpression{*mapArgument.type.valueType};
        }
        return CheckedExpression{unknownType()};
    }
    case NativeFunctionKind::Clear: {
        const CheckedExpression mapArgument = checkExpressionInfo(*expression.arguments[0]);
        if (mapArgument.type.kind != StaticType::Unknown && mapArgument.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                "clear expects map as first argument, got " + typeInfoName(mapArgument.type));
        }
        return CheckedExpression{simpleType(StaticType::Nil)};
    }
    case NativeFunctionKind::Merge: {
        const CheckedExpression leftArgument = checkExpressionInfo(*expression.arguments[0]);
        const CheckedExpression rightArgument = checkExpressionInfo(*expression.arguments[1]);
        return checkMapMerge(expression.paren, leftArgument.type, rightArgument.type);
    }
    case NativeFunctionKind::Keys:
    case NativeFunctionKind::Values: {
        const CheckedExpression mapArgument = checkExpressionInfo(*expression.arguments[0]);
        if (mapArgument.type.kind != StaticType::Unknown && mapArgument.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                std::string(function->name) + " expects map as first argument, got "
                    + typeInfoName(mapArgument.type));
        }
        const TypeInfo* elementType = mapArgument.type.kind == StaticType::Map
            ? (function->kind == NativeFunctionKind::Keys
                    ? mapArgument.type.keyType.get()
                    : mapArgument.type.valueType.get())
            : nullptr;
        return CheckedExpression{elementType ? arrayType(*elementType) : simpleType(StaticType::Array)};
    }
    case NativeFunctionKind::Floor:
    case NativeFunctionKind::Ceil:
    case NativeFunctionKind::Sqrt: {
        const CheckedExpression argument = checkExpressionInfo(*expression.arguments[0]);
        if (argument.type.kind != StaticType::Unknown && argument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                std::string(function->name) + " expects number, got " + typeInfoName(argument.type));
        }
        return CheckedExpression{simpleType(StaticType::Number)};
    }
    case NativeFunctionKind::Str:
        checkExpressionInfo(*expression.arguments[0]);
        return CheckedExpression{simpleType(StaticType::String)};
    case NativeFunctionKind::Substr: {
        const CheckedExpression stringArgument = checkExpressionInfo(*expression.arguments[0]);
        if (stringArgument.type.kind != StaticType::Unknown && stringArgument.type.kind != StaticType::String) {
            throw TypeError(expression.paren,
                "substr expects string as first argument, got " + typeInfoName(stringArgument.type));
        }
        const CheckedExpression startArgument = checkExpressionInfo(*expression.arguments[1]);
        if (startArgument.type.kind != StaticType::Unknown && startArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "substr expects number as second argument, got " + typeInfoName(startArgument.type));
        }
        const CheckedExpression lengthArgument = checkExpressionInfo(*expression.arguments[2]);
        if (lengthArgument.type.kind != StaticType::Unknown && lengthArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "substr expects number as third argument, got " + typeInfoName(lengthArgument.type));
        }
        return CheckedExpression{simpleType(StaticType::String)};
    }
    case NativeFunctionKind::CharAt: {
        const CheckedExpression stringArgument = checkExpressionInfo(*expression.arguments[0]);
        if (stringArgument.type.kind != StaticType::Unknown && stringArgument.type.kind != StaticType::String) {
            throw TypeError(expression.paren,
                "charAt expects string as first argument, got " + typeInfoName(stringArgument.type));
        }
        const CheckedExpression indexArgument = checkExpressionInfo(*expression.arguments[1]);
        if (indexArgument.type.kind != StaticType::Unknown && indexArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "charAt expects number as second argument, got " + typeInfoName(indexArgument.type));
        }
        return CheckedExpression{simpleType(StaticType::String)};
    }
    case NativeFunctionKind::TypeOf:
        checkExpressionInfo(*expression.arguments[0]);
        return CheckedExpression{simpleType(StaticType::String)};
    case NativeFunctionKind::Contains: {
        const CheckedExpression collectionArgument = checkExpressionInfo(*expression.arguments[0]);
        if (collectionArgument.type.kind != StaticType::Unknown
            && collectionArgument.type.kind != StaticType::Array
            && collectionArgument.type.kind != StaticType::Map
            && collectionArgument.type.kind != StaticType::Range) {
            throw TypeError(expression.paren,
                "contains expects array, map, or range as first argument, got " + typeInfoName(collectionArgument.type));
        }
        const TypeInfo* expectedKey = nullptr;
        if (collectionArgument.type.kind == StaticType::Array) {
            expectedKey = collectionArgument.type.elementType.get();
        } else if (collectionArgument.type.kind == StaticType::Map) {
            expectedKey = collectionArgument.type.keyType.get();
        } else if (collectionArgument.type.kind == StaticType::Range) {
            expectedKey = nullptr;
        }
        const CheckedExpression keyArgument = checkExpressionInfo(*expression.arguments[1], expectedKey);
        if (collectionArgument.type.kind == StaticType::Map
            && SemanticTypes::isKnown(keyArgument.type)
            && !mapKeyTypeAllowed(keyArgument.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !SemanticTypes::compatible(*expectedKey, keyArgument.type)) {
            if (collectionArgument.type.kind == StaticType::Map) {
                throw TypeError(expression.paren, "map key is incompatible with map key type");
            }
            throw TypeError(expression.paren,
                "contains value expects " + typeInfoName(*expectedKey)
                    + ", got " + typeInfoName(keyArgument.type));
        }
        if (collectionArgument.type.kind == StaticType::Range
            && keyArgument.type.kind != StaticType::Unknown
            && keyArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "contains expects number as range value, got " + typeInfoName(keyArgument.type));
        }
        return CheckedExpression{simpleType(StaticType::Bool)};
    }
    case NativeFunctionKind::Slice: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        if (arrayArgument.type.kind != StaticType::Unknown && arrayArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "slice expects array as first argument, got " + typeInfoName(arrayArgument.type));
        }
        const CheckedExpression startArgument = checkExpressionInfo(*expression.arguments[1]);
        if (startArgument.type.kind != StaticType::Unknown && startArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "slice expects number as second argument, got " + typeInfoName(startArgument.type));
        }
        const CheckedExpression lengthArgument = checkExpressionInfo(*expression.arguments[2]);
        if (lengthArgument.type.kind != StaticType::Unknown && lengthArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "slice expects number as third argument, got " + typeInfoName(lengthArgument.type));
        }
        return CheckedExpression{copiedArrayType(arrayArgument.type)};
    }
    case NativeFunctionKind::Copy: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        if (arrayArgument.type.kind != StaticType::Unknown && arrayArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "copy expects array as first argument, got " + typeInfoName(arrayArgument.type));
        }
        return CheckedExpression{copiedArrayType(arrayArgument.type)};
    }
    case NativeFunctionKind::Concat: {
        const CheckedExpression leftArgument = checkExpressionInfo(*expression.arguments[0]);
        if (leftArgument.type.kind != StaticType::Unknown && leftArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "concat expects array as first argument, got " + typeInfoName(leftArgument.type));
        }
        const CheckedExpression rightArgument = checkExpressionInfo(*expression.arguments[1]);
        if (rightArgument.type.kind != StaticType::Unknown && rightArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "concat expects array as second argument, got " + typeInfoName(rightArgument.type));
        }
        return CheckedExpression{concatenatedArrayType(leftArgument.type, rightArgument.type)};
    }
    case NativeFunctionKind::Map: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayMap(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::Filter: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayFilter(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::FlatMap: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayFlatMap(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::Any:
    case NativeFunctionKind::All: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayAnyAll(
            expression.paren,
            arrayArgument.type,
            *expression.arguments[1],
            function->name);
    }
    case NativeFunctionKind::Count: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayCount(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::Find: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayFind(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::FindIndex: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayFindIndex(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::Reduce: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayReduce(
            expression.paren,
            arrayArgument.type,
            *expression.arguments[1],
            *expression.arguments[2]);
    }
    case NativeFunctionKind::Range: {
        for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
            const CheckedExpression argument = checkExpressionInfo(*expression.arguments[index]);
            if (argument.type.kind != StaticType::Unknown && argument.type.kind != StaticType::Number) {
                const char* ordinal = index == 0 ? "first" : (index == 1 ? "second" : "third");
                throw TypeError(expression.paren,
                    std::string("range expects number as ") + ordinal + " argument, got "
                        + typeInfoName(argument.type));
            }
        }
        return CheckedExpression{simpleType(StaticType::Range)};
    }
    }

    throw TypeError(variable->name, "unknown native stdlib function `" + variable->name.lexeme + "`");
}

TypeChecker::CheckedExpression TypeChecker::checkStructMethodCall(const MemberCallExpr& expression, const TypeInfo& receiverType)
{
    const std::string& name = expression.name.lexeme;
    const MethodInfo* method = findMethod(*receiverType.structName, name);
    if (!method) {
        throw TypeError(expression.paren, "struct `" + *receiverType.structName + "` has no method `" + name + "`");
    }

    TypeSubstitutions receiverSubstitutions;
    if (!method->receiverType.typeArguments.empty()) {
        if (method->receiverType.typeArguments.size() != receiverType.typeArguments.size()) {
            throw TypeError(expression.paren,
                "method receiver expects "
                    + std::to_string(method->receiverType.typeArguments.size())
                    + " type arguments but got "
                    + std::to_string(receiverType.typeArguments.size()));
        }
        for (std::size_t i = 0; i < method->receiverType.typeArguments.size(); ++i) {
            inferTypeArguments(
                method->receiverType.typeArguments[i],
                receiverType.typeArguments[i],
                receiverSubstitutions,
                expression.paren);
        }
        const StructTypeDecl* structType = findStructType(*receiverType.structName);
        if (structType) {
            for (const std::string& parameter : structType->genericParameters) {
                if (receiverSubstitutions.find(parameter) == receiverSubstitutions.end()) {
                    throw TypeError(expression.paren,
                        "cannot specialize method receiver type parameter " + parameter);
                }
            }
            validateGenericTypeArguments(
                structType->genericParameters,
                structType->genericParameterConstraints,
                receiverSubstitutions,
                expression.paren,
                "method receiver");
        }
    }

    std::vector<TypeInfo> parameterTypes;
    parameterTypes.reserve(method->parameterTypes.size());
    for (const TypeInfo& parameter : method->parameterTypes) {
        parameterTypes.push_back(
            SemanticTypes::substituteTypeParameters(parameter, receiverSubstitutions));
    }
    const TypeInfo returnType = SemanticTypes::substituteTypeParameters(
        method->returnType, receiverSubstitutions);
    const TypeInfo signature = functionType(
        std::move(parameterTypes),
        returnType,
        method->genericParameters,
        method->genericParameterConstraints);
    const CheckedExpression result = checkFunctionCall(
        expression.paren,
        signature,
        expression.typeArguments,
        expression.arguments);
    declarationIndex_.recordMemberCallMetadata(
        expression,
        MemberCallMetadataRecord{
            method->resolvedName,
            true,
            method->declaration != nullptr});
    if (method->declaration) {
        if (const DeclarationRecord* target = declarationIndex_.declaration(*method->declaration)) {
            declarationIndex_.recordMemberCallTarget(
                expression,
                CallTargetRecord{
                    CallTargetKind::StructMethod,
                    ResolvedSymbol{target->declarationId, target->symbolId}});
        }
    }
    return result;
}

TypeChecker::CheckedExpression TypeChecker::checkMemberCall(
    const MemberCallExpr& expression,
    const TypeInfo* expectedType)
{
    const std::string& name = expression.name.lexeme;
    const std::size_t arity = expression.arguments.size();

    if (!enumConstructorTypeName(expression).empty()) {
        return checkVariantConstructor(expression, expectedType);
    }

    if (const auto* variable = dynamic_cast<const VariableExpr*>(expression.receiver.get())) {
        if (const NamespaceImport* namespaceImport = findNamespace(variable->name.lexeme)) {
            const auto found = namespaceImport->values.find(name);
            if (found == namespaceImport->values.end()) {
                throw TypeError(expression.name,
                    "module namespace `" + variable->name.lexeme + "` has no exported member `" + name + "`");
            }
            declarationIndex_.recordMemberCallMetadata(
                expression,
                MemberCallMetadataRecord{found->second.resolvedName, false});
            return checkFunctionCall(
                expression.paren,
                found->second.type,
                expression.typeArguments,
                expression.arguments);
        }
    }

    std::optional<CheckedExpression> builtinReceiver;
    if (isBuiltinMemberName(name)) {
        CheckedExpression receiver = checkExpressionInfo(*expression.receiver);
        if (receiver.type.kind == StaticType::Struct
            && receiver.type.structName
            && findMethod(*receiver.type.structName, name)) {
            return checkStructMethodCall(expression, receiver.type);
        }
        builtinReceiver = std::move(receiver);
    }

    if (!expression.typeArguments.empty() && isBuiltinMemberName(name)) {
        throw TypeError(expression.paren, "function is not generic");
    }

    auto expectArity = [&](std::size_t expected) {
        if (arity != expected) {
            throw TypeError(expression.paren,
                "expected " + std::to_string(expected) + " arguments but got " + std::to_string(arity));
        }
    };

    auto checkReceiver = [&]() {
        if (builtinReceiver) {
            return *builtinReceiver;
        }
        return checkExpressionInfo(*expression.receiver);
    };

    if (name == "push") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "push expects array receiver, got " + typeInfoName(receiver.type));
        }

        Binding* target = findSimpleVariableBinding(*expression.receiver);
        const bool strictElementCheck = target == nullptr || target->explicitType;
        const TypeInfo* expectedElement = strictElementCheck ? receiver.type.elementType.get() : nullptr;
        const CheckedExpression value = checkExpressionInfo(*expression.arguments[0], expectedElement);
        if (strictElementCheck && expectedElement && !SemanticTypes::compatible(*expectedElement, value.type)) {
            throw TypeError(expression.paren,
                "push value expects " + typeInfoName(*expectedElement) + ", got " + typeInfoName(value.type));
        }
        if (target && target->type.kind == StaticType::Array) {
            refineArrayBindingFromMutation(*target, value.type);
        }
        return CheckedExpression{simpleType(StaticType::Nil)};
    }

    if (name == "pop") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "pop expects array receiver, got " + typeInfoName(receiver.type));
        }
        if (receiver.type.kind == StaticType::Array && receiver.type.elementType) {
            return CheckedExpression{*receiver.type.elementType};
        }
        return CheckedExpression{unknownType()};
    }

    if (name == "contains") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown
            && receiver.type.kind != StaticType::Array
            && receiver.type.kind != StaticType::Map
            && receiver.type.kind != StaticType::Range) {
            throw TypeError(expression.paren, "contains expects array, map, or range receiver, got " + typeInfoName(receiver.type));
        }
        const TypeInfo* expectedKey = nullptr;
        if (receiver.type.kind == StaticType::Array) {
            expectedKey = receiver.type.elementType.get();
        } else if (receiver.type.kind == StaticType::Map) {
            expectedKey = receiver.type.keyType.get();
        }
        const CheckedExpression value = checkExpressionInfo(*expression.arguments[0], expectedKey);
        if (receiver.type.kind == StaticType::Map
            && SemanticTypes::isKnown(value.type)
            && !mapKeyTypeAllowed(value.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !SemanticTypes::compatible(*expectedKey, value.type)) {
            if (receiver.type.kind == StaticType::Map) {
                throw TypeError(expression.paren, "map key is incompatible with map key type");
            }
            throw TypeError(expression.paren,
                "contains value expects " + typeInfoName(*expectedKey) + ", got " + typeInfoName(value.type));
        }
        if (receiver.type.kind == StaticType::Range
            && value.type.kind != StaticType::Unknown
            && value.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "contains expects number as range value, got " + typeInfoName(value.type));
        }
        return CheckedExpression{simpleType(StaticType::Bool)};
    }

    if (name == "remove") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                "remove expects map receiver, got " + typeInfoName(receiver.type));
        }
        const TypeInfo* expectedKey = receiver.type.kind == StaticType::Map
            ? receiver.type.keyType.get()
            : nullptr;
        const CheckedExpression key = checkExpressionInfo(*expression.arguments[0], expectedKey);
        if (receiver.type.kind == StaticType::Map
            && SemanticTypes::isKnown(key.type)
            && !mapKeyTypeAllowed(key.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !SemanticTypes::compatible(*expectedKey, key.type)) {
            throw TypeError(expression.paren, "map key is incompatible with map key type");
        }
        if (receiver.type.kind == StaticType::Map && receiver.type.valueType) {
            return CheckedExpression{*receiver.type.valueType};
        }
        return CheckedExpression{unknownType()};
    }

    if (name == "clear") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                "clear expects map receiver, got " + typeInfoName(receiver.type));
        }
        return CheckedExpression{simpleType(StaticType::Nil)};
    }

    if (name == "merge") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        const CheckedExpression right = checkExpressionInfo(*expression.arguments[0]);
        return checkMapMerge(expression.paren, receiver.type, right.type);
    }

    if (name == "keys" || name == "values") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                name + " expects map receiver, got " + typeInfoName(receiver.type));
        }
        const TypeInfo* elementType = receiver.type.kind == StaticType::Map
            ? (name == "keys" ? receiver.type.keyType.get() : receiver.type.valueType.get())
            : nullptr;
        return CheckedExpression{elementType ? arrayType(*elementType) : simpleType(StaticType::Array)};
    }

    if (name == "slice") {
        expectArity(2);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "slice expects array receiver, got " + typeInfoName(receiver.type));
        }
        const CheckedExpression start = checkExpressionInfo(*expression.arguments[0]);
        if (start.type.kind != StaticType::Unknown && start.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "slice expects number as first argument, got " + typeInfoName(start.type));
        }
        const CheckedExpression length = checkExpressionInfo(*expression.arguments[1]);
        if (length.type.kind != StaticType::Unknown && length.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "slice expects number as second argument, got " + typeInfoName(length.type));
        }
        return CheckedExpression{copiedArrayType(receiver.type)};
    }

    if (name == "copy") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "copy expects array receiver, got " + typeInfoName(receiver.type));
        }
        return CheckedExpression{copiedArrayType(receiver.type)};
    }

    if (name == "concat") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "concat expects array receiver, got " + typeInfoName(receiver.type));
        }
        const CheckedExpression right = checkExpressionInfo(*expression.arguments[0]);
        if (right.type.kind != StaticType::Unknown && right.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "concat expects array as first argument, got " + typeInfoName(right.type));
        }
        return CheckedExpression{concatenatedArrayType(receiver.type, right.type)};
    }

    if (name == "map") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayMap(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "filter") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayFilter(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "flatMap") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayFlatMap(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "any" || name == "all") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayAnyAll(expression.paren, receiver.type, *expression.arguments[0], name);
    }

    if (name == "count") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayCount(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "find") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayFind(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "findIndex") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayFindIndex(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "reduce") {
        expectArity(2);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayReduce(
            expression.paren,
            receiver.type,
            *expression.arguments[0],
            *expression.arguments[1]);
    }

    if (name == "len") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (SemanticTypes::isKnown(receiver.type)
            && receiver.type.kind != StaticType::Array
            && receiver.type.kind != StaticType::String
            && receiver.type.kind != StaticType::Map
            && receiver.type.kind != StaticType::Range) {
            throw TypeError(expression.paren, "len expects array, string, map, or range receiver, got " + typeInfoName(receiver.type));
        }
        return CheckedExpression{simpleType(StaticType::Number)};
    }

    if (name == "substr") {
        expectArity(2);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::String) {
            throw TypeError(expression.paren, "substr expects string receiver, got " + typeInfoName(receiver.type));
        }
        const CheckedExpression start = checkExpressionInfo(*expression.arguments[0]);
        if (start.type.kind != StaticType::Unknown && start.type.kind != StaticType::Number) {
            throw TypeError(expression.paren, "substr expects number as first argument, got " + typeInfoName(start.type));
        }
        const CheckedExpression length = checkExpressionInfo(*expression.arguments[1]);
        if (length.type.kind != StaticType::Unknown && length.type.kind != StaticType::Number) {
            throw TypeError(expression.paren, "substr expects number as second argument, got " + typeInfoName(length.type));
        }
        return CheckedExpression{simpleType(StaticType::String)};
    }

    if (name == "charAt") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::String) {
            throw TypeError(expression.paren, "charAt expects string receiver, got " + typeInfoName(receiver.type));
        }
        const CheckedExpression index = checkExpressionInfo(*expression.arguments[0]);
        if (index.type.kind != StaticType::Unknown && index.type.kind != StaticType::Number) {
            throw TypeError(expression.paren, "charAt expects number as first argument, got " + typeInfoName(index.type));
        }
        return CheckedExpression{simpleType(StaticType::String)};
    }

    const CheckedExpression receiver = checkExpressionInfo(*expression.receiver);
    if (receiver.type.kind == StaticType::Struct && receiver.type.structName) {
        return checkStructMethodCall(expression, receiver.type);
    }

    if (receiver.type.kind == StaticType::Unknown
        || (receiver.type.kind != StaticType::Array && receiver.type.kind != StaticType::String)) {
        throw TypeError(expression.paren, "can only call methods on known named structs");
    }

    throw TypeError(expression.paren, "unknown member call `" + name + "`");
}

TypeChecker::CheckedExpression TypeChecker::checkCall(const CallExpr& expression)
{
    if (isBuiltinLenCall(expression)) {
        return checkBuiltinLenCall(expression);
    }

    if (isNativeStdlibCall(expression)) {
        const CheckedExpression result = checkNativeStdlibCall(expression);
        const auto* variable = dynamic_cast<const VariableExpr*>(expression.callee.get());
        if (variable && isNativeCallbackName(variable->name.lexeme)) {
            flowFacts_.invalidateAll();
        }
        return result;
    }

    const CheckedExpression callee = checkExpressionInfo(*expression.callee);
    CheckedExpression result = checkFunctionCall(
        expression.paren,
        callee.type,
        expression.typeArguments,
        expression.arguments);
    invalidateCallEffects(expression);
    return result;
}

void TypeChecker::invalidateCallEffects(const CallExpr& expression)
{
    const CallTargetRecord* callTarget = declarationIndex_.callTarget(expression);
    if (!callTarget || callTarget->kind != CallTargetKind::Direct) {
        flowFacts_.invalidateAll();
        return;
    }

    const DeclarationRecord* declaration = declarationIndex_.declaration(callTarget->target.declarationId);
    if (!declaration || !declaration->statement
        || !dynamic_cast<const FunctionStmt*>(declaration->statement)) {
        flowFacts_.invalidateAll();
        return;
    }

    invalidateCapturedBindings(expression);
}

void TypeChecker::invalidateCapturedBindings(const CallExpr& expression)
{
    const CallTargetRecord* callTarget = declarationIndex_.callTarget(expression);
    if (!callTarget || callTarget->kind != CallTargetKind::Direct) {
        return;
    }

    const DeclarationRecord* declaration = declarationIndex_.declaration(callTarget->target.declarationId);
    if (!declaration || !declaration->statement) {
        return;
    }

    const auto* function = dynamic_cast<const FunctionStmt*>(declaration->statement);
    if (!function) {
        return;
    }

    const CaptureRecord* captures = declarationIndex_.captureMetadata(*function);
    if (!captures) {
        return;
    }

    invalidateCapturedSymbols(*captures);
}

void TypeChecker::invalidateStructMethodEffects(const MemberCallExpr& expression)
{
    const CallTargetRecord* callTarget = declarationIndex_.callTarget(expression);
    if (!callTarget || callTarget->kind != CallTargetKind::StructMethod) {
        return;
    }

    flowFacts_.invalidateAll();
}

void TypeChecker::invalidateCapturedSymbols(const CaptureRecord& captures)
{
    for (const ResolvedSymbol& symbol : captures.symbols) {
        if (const Binding* binding = findBinding(symbol.declarationId)) {
            flowFacts_.invalidate(binding->resolvedName);
        }
    }
}

TypeChecker::IndexTargetTypes TypeChecker::checkIndexTarget(
    const Expr& collectionExpression,
    const Expr& indexExpression,
    const Token& bracket,
    const std::string& nonArrayMessage)
{
    IndexTargetTypes result {
        checkExpression(collectionExpression),
        checkExpression(indexExpression),
    };

    if (result.collection.kind != StaticType::Unknown
        && result.collection.kind != StaticType::Array
        && result.collection.kind != StaticType::Map
        && result.collection.kind != StaticType::Range) {
        throw TypeError(bracket, nonArrayMessage);
    }

    if (result.collection.kind == StaticType::Map) {
        if (SemanticTypes::isKnown(result.index) && !mapKeyTypeAllowed(result.index)) {
            throw TypeError(bracket, "map key must be nil, number, bool, or string");
        }
        if (result.collection.keyType && !SemanticTypes::compatible(*result.collection.keyType, result.index)) {
            throw TypeError(bracket, "map key is incompatible with map key type");
        }
    } else if (result.collection.kind == StaticType::Array
        && result.index.kind != StaticType::Unknown
        && result.index.kind != StaticType::Number) {
        throw TypeError(bracket, "array index must be number");
    } else if (result.collection.kind == StaticType::Range
        && result.index.kind != StaticType::Unknown
        && result.index.kind != StaticType::Number) {
        throw TypeError(bracket, "range index must be number");
    }

    return result;
}

TypeInfo TypeChecker::checkIndex(const IndexExpr& expression)
{
    const IndexTargetTypes target = checkIndexTarget(
        *expression.collection, *expression.index, expression.bracket, "can only index arrays, maps, or ranges");

    TypeInfo result = unknownType();
    if (target.collection.kind == StaticType::Array && target.collection.elementType) {
        result = *target.collection.elementType;
    } else if (target.collection.kind == StaticType::Map && target.collection.valueType) {
        result = *target.collection.valueType;
    } else if (target.collection.kind == StaticType::Range) {
        result = simpleType(StaticType::Number);
    }
    if (const std::optional<std::string> factName = indexFlowFactName(
            *expression.collection,
            *expression.index)) {
        if (const std::optional<TypeInfo> narrowed = flowFacts_.narrowedTypeFor(*factName)) {
            result = *narrowed;
        }
    }
    declarationIndex_.recordIndexOperation(
        expression,
        IndexOperationRecord{
            IndexOperationKind::Read,
            target.collection,
            target.index,
            result});
    return result;
}

TypeChecker::CheckedExpression TypeChecker::checkIndexAssignment(const IndexAssignExpr& expression)
{
    const IndexTargetTypes target = checkIndexTarget(
        *expression.collection, *expression.index, expression.bracket, "can only assign array elements, map entries, or range elements");

    if (target.collection.kind == StaticType::Range) {
        throw TypeError(expression.bracket, "cannot assign range elements");
    }

    Binding* binding = findSimpleVariableBinding(*expression.collection);
    if (target.collection.kind == StaticType::Map) {
        const CheckedExpression value = checkExpressionInfo(
            *expression.value,
            target.collection.valueType ? target.collection.valueType.get() : nullptr);
        if (target.collection.valueType && !SemanticTypes::compatible(*target.collection.valueType, value.type)) {
            throw TypeError(expression.bracket, "map value is incompatible with map value type");
        }
        flowFacts_.invalidateAll();
        declarationIndex_.recordIndexOperation(
            expression,
            IndexOperationRecord{
                IndexOperationKind::Assign,
                target.collection,
                target.index,
                value.type});
        return value;
    }

    const bool strictElementCheck = binding == nullptr || binding->explicitType;
    const TypeInfo* expectedElement = strictElementCheck ? target.collection.elementType.get() : nullptr;
    const CheckedExpression value = checkExpressionInfo(*expression.value, expectedElement);
    if (strictElementCheck && expectedElement && !SemanticTypes::compatible(*expectedElement, value.type)) {
        throw TypeError(expression.bracket,
            "array index assignment expects " + typeInfoName(*expectedElement)
                + ", got " + typeInfoName(value.type));
    }

    if (binding && binding->type.kind == StaticType::Array) {
        refineArrayBindingFromMutation(*binding, value.type);
        flowFacts_.invalidateAll();
        declarationIndex_.recordIndexOperation(
            expression,
            IndexOperationRecord{
                IndexOperationKind::Assign,
                target.collection,
                target.index,
                value.type});
        return CheckedExpression{value.type};
    }

    flowFacts_.invalidateAll();
    declarationIndex_.recordIndexOperation(
        expression,
        IndexOperationRecord{
            IndexOperationKind::Assign,
            target.collection,
            target.index,
            value.type});
    return value;
}

TypeChecker::CheckedExpression TypeChecker::checkIndexCompoundAssignment(const IndexCompoundAssignExpr& expression)
{
    const IndexTargetTypes target = checkIndexTarget(
        *expression.collection, *expression.index, expression.bracket, "can only assign array elements, map entries, or range elements");

    if (target.collection.kind == StaticType::Map) {
        throw TypeError(expression.bracket, "compound assignment is not supported for map entries");
    }

    if (target.collection.kind == StaticType::Range) {
        throw TypeError(expression.bracket, "cannot assign range elements");
    }

    if (target.collection.kind == StaticType::Array && target.collection.elementType) {
        checkKnownNumber(expression.op, *target.collection.elementType, "compound assignment target must be number, got ");
    }

    const CheckedExpression value = checkExpressionInfo(*expression.value);
    checkKnownNumber(expression.op, value.type, "compound assignment value must be number, got ");

    const TypeInfo result = simpleType(StaticType::Number);
    flowFacts_.invalidateAll();
    declarationIndex_.recordIndexOperation(
        expression,
        IndexOperationRecord{
            IndexOperationKind::CompoundAssign,
            target.collection,
            target.index,
            result});
    return CheckedExpression{result};
}

std::optional<TypeInfo> TypeChecker::checkStructFieldTarget(
    const Expr& objectExpression,
    const Token& name,
    const std::string& nonStructMessage)
{
    const TypeInfo object = checkExpression(objectExpression);

    if (object.kind != StaticType::Unknown && object.kind != StaticType::Struct) {
        throw TypeError(name, nonStructMessage);
    }

    if (object.kind == StaticType::Struct && object.structName) {
        const StructTypeDecl* structType = findStructType(*object.structName);
        const StructFieldType* structField = structType ? findStructField(*structType, name.lexeme) : nullptr;
        if (!structField) {
            throw TypeError(name,
                "struct `" + unqualifiedStructName(*object.structName)
                    + (structType && structType->hasPrivateFields
                            ? "` has no accessible field `"
                            : "` has no field `")
                    + name.lexeme + "`");
        }
        if (structField->isPrivate && !canAccessPrivateFields(*structType)) {
            throw TypeError(name,
                "struct `" + unqualifiedStructName(*object.structName) + "` has no accessible field `"
                    + name.lexeme + "`");
        }
        return structFieldTypeForValue(object, *structType, *structField);
    }

    return std::nullopt;
}

TypeChecker::CheckedExpression TypeChecker::checkFieldAssignment(const FieldAssignExpr& expression)
{
    const std::optional<TypeInfo> structField = checkStructFieldTarget(
        *expression.object, expression.name, "can only assign fields on structs");
    const TypeInfo* expectedFieldType = structField ? &*structField : nullptr;
    const CheckedExpression value = checkExpressionInfo(*expression.value, expectedFieldType);

    if (structField) {
        if (!SemanticTypes::compatible(*structField, value.type)) {
            throw TypeError(expression.name,
                "field `" + expression.name.lexeme + "` expects " + typeInfoName(*structField)
                    + ", got " + typeInfoName(value.type));
        }
        flowFacts_.invalidateAll();
        declarationIndex_.recordFieldOperation(
            expression,
            FieldOperationRecord{
                FieldOperationKind::Assign,
                expression.name.lexeme,
                *structField,
                *structField,
                std::nullopt});
        return CheckedExpression{*structField};
    }

    flowFacts_.invalidateAll();
    declarationIndex_.recordFieldOperation(
        expression,
        FieldOperationRecord{
            FieldOperationKind::Assign,
            expression.name.lexeme,
            unknownType(),
            value.type,
            std::nullopt});
    return value;
}

TypeChecker::CheckedExpression TypeChecker::checkFieldCompoundAssignment(const FieldCompoundAssignExpr& expression)
{
    const std::optional<TypeInfo> structField = checkStructFieldTarget(
        *expression.object, expression.name, "can only assign fields on structs");
    if (structField) {
        checkKnownNumber(expression.op, *structField, "compound assignment target must be number, got ");
    }

    const CheckedExpression value = checkExpressionInfo(*expression.value);
    checkKnownNumber(expression.op, value.type, "compound assignment value must be number, got ");

    const TypeInfo result = simpleType(StaticType::Number);
    flowFacts_.invalidateAll();
    declarationIndex_.recordFieldOperation(
        expression,
        FieldOperationRecord{
            FieldOperationKind::CompoundAssign,
            expression.name.lexeme,
            structField ? *structField : unknownType(),
            result,
            std::nullopt});
    return CheckedExpression{result};
}

void TypeChecker::checkKnownNumber(const Token& token, const TypeInfo& type, const std::string& messagePrefix) const
{
    if (type.kind != StaticType::Unknown && type.kind != StaticType::Number) {
        throw TypeError(token, messagePrefix + typeInfoName(type));
    }
}

TypeInfo TypeChecker::resolveTypeParameterConstraint(const TypeAnnotation& typeName) const
{
    if (typeName.kind == TypeAnnotation::Kind::Simple
        && typeName.typeArguments.empty()
        && (typeName.token.lexeme == "Eq" || typeName.token.lexeme == "Ord")) {
        return capabilityType(typeName.token.lexeme);
    }
    return resolveAnnotation(typeName);
}

TypeInfo TypeChecker::resolveAnnotation(const TypeAnnotation& typeName) const
{
    if (typeName.kind == TypeAnnotation::Kind::Nullable) {
        return nullableType(resolveAnnotation(*typeName.innerType));
    }

    if (typeName.kind == TypeAnnotation::Kind::Array) {
        return arrayType(resolveAnnotation(*typeName.elementType));
    }

    if (typeName.kind == TypeAnnotation::Kind::Map) {
        TypeInfo keyType = resolveAnnotation(*typeName.keyType);
        if (!mapKeyTypeAllowed(keyType)) {
            throw TypeError(typeName.token, "map key must be nil, number, bool, or string");
        }
        return mapType(std::move(keyType), resolveAnnotation(*typeName.valueType));
    }

    if (typeName.kind == TypeAnnotation::Kind::Function) {
        std::vector<TypeInfo> parameterTypes;
        parameterTypes.reserve(typeName.parameterTypes.size());
        for (const TypeAnnotation& parameter : typeName.parameterTypes) {
            parameterTypes.push_back(resolveAnnotation(parameter));
        }
        return functionType(std::move(parameterTypes), resolveAnnotation(*typeName.returnType));
    }

    if (typeName.kind == TypeAnnotation::Kind::Qualified) {
        const NamespaceImport* namespaceImport = findNamespace(typeName.qualifier.lexeme);
        if (!namespaceImport) {
            throw TypeError(typeName.qualifier, "unknown module namespace `" + typeName.qualifier.lexeme + "`");
        }
        const auto structFound = namespaceImport->structs.find(typeName.token.lexeme);
        if (structFound != namespaceImport->structs.end()) {
            return resolveNamedStructAnnotation(
                typeName,
                qualifiedStructName(typeName.qualifier, typeName.token),
                structFound->second);
        }
        const auto enumFound = namespaceImport->enums.find(typeName.token.lexeme);
        if (enumFound != namespaceImport->enums.end()) {
            return resolveNamedEnumAnnotation(
                typeName,
                qualifiedStructName(typeName.qualifier, typeName.token),
                enumFound->second);
        }
        throw TypeError(typeName.token,
            "module namespace `" + typeName.qualifier.lexeme + "` has no exported type `"
                + typeName.token.lexeme + "`");
    }

   if (const TypeInfo* typeParameter = findTypeParameter(typeName.token.lexeme)) {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type parameter `" + typeName.token.lexeme + "` is not generic");
        }
        return *typeParameter;
    }

    if (typeName.token.lexeme == "number") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `number` is not generic");
        }
        return simpleType(StaticType::Number);
    }
    if (typeName.token.lexeme == "bool") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `bool` is not generic");
        }
        return simpleType(StaticType::Bool);
    }
    if (typeName.token.lexeme == "string") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `string` is not generic");
        }
        return simpleType(StaticType::String);
    }
    if (typeName.token.lexeme == "nil") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `nil` is not generic");
        }
        return simpleType(StaticType::Nil);
    }
    if (const StructTypeDecl* structType = findStructType(typeName.token.lexeme)) {
        return resolveNamedStructAnnotation(
            typeName, typeName.token.lexeme, *structType);
    }
    if (const EnumTypeDecl* enumType = findEnumType(typeName.token.lexeme)) {
        return resolveNamedEnumAnnotation(typeName, typeName.token.lexeme, *enumType);
    }
   throw TypeError(typeName.token, "unknown type `" + typeName.token.lexeme + "`");
}

void TypeChecker::checkAssignable(const Token& token, const std::string& context, const TypeInfo& expected, const TypeInfo& actual) const
{
    if (!SemanticTypes::compatible(expected, actual)) {
        throw TypeError(token, context);
    }
}

TypeInfo TypeChecker::checkUnary(const UnaryExpr& expression)
{
    const TypeInfo right = checkExpression(*expression.right);
    switch (expression.op.type) {
    case TokenType::Minus:
        if (SemanticTypes::isKnown(right) && right.kind != StaticType::Number) {
            throw TypeError(expression.op, "unary `-` expects number, got " + typeInfoName(right));
        }
        return simpleType(StaticType::Number);
    case TokenType::Bang:
        return simpleType(StaticType::Bool);
    default:
        throw TypeError(expression.op, "unsupported unary operator `" + expression.op.lexeme + "`");
    }
}

TypeInfo TypeChecker::checkBinary(const BinaryExpr& expression)
{
    const TypeInfo left = checkExpression(*expression.left);
    const TypeInfo right = checkExpression(*expression.right);

    const auto requireCapability = [&](const TypeInfo& operand, const std::string& capability) {
        if (operand.kind != StaticType::TypeParameter
            || SemanticTypes::satisfiesCapability(operand, capabilityType(capability))) {
            return;
        }
        const std::string parameter = operand.typeParameterName.value_or("<unknown>");
        throw TypeError(expression.op,
            "binary `" + expression.op.lexeme + "` requires type parameter `"
                + parameter + "` to satisfy " + capability);
    };

    switch (expression.op.type) {
    case TokenType::Plus:
        if (!SemanticTypes::isKnown(left) || !SemanticTypes::isKnown(right)) {
            return unknownType();
        }
        if (left.kind == StaticType::Number && right.kind == StaticType::Number) {
            return simpleType(StaticType::Number);
        }
        if (left.kind == StaticType::String && right.kind == StaticType::String) {
            return simpleType(StaticType::String);
        }
        throw TypeError(expression.op, "binary `+` expects two numbers or two strings, got "
            + typeInfoName(left) + " and " + typeInfoName(right));
    case TokenType::Minus:
    case TokenType::Star:
    case TokenType::Slash:
        if (!SemanticTypes::isKnown(left) || !SemanticTypes::isKnown(right)) {
            return simpleType(StaticType::Number);
        }
        if (left.kind != StaticType::Number || right.kind != StaticType::Number) {
            throw TypeError(expression.op, binaryTypesMessage(expression, left, right));
        }
        return simpleType(StaticType::Number);
    case TokenType::Greater:
    case TokenType::GreaterEqual:
    case TokenType::Less:
    case TokenType::LessEqual: {
        requireCapability(left, "Ord");
        requireCapability(right, "Ord");
        if (!SemanticTypes::isKnown(left) || !SemanticTypes::isKnown(right)) {
            return simpleType(StaticType::Bool);
        }
        const bool leftIsOrderedTypeParameter
            = left.kind == StaticType::TypeParameter
            && SemanticTypes::satisfiesCapability(left, capabilityType("Ord"));
        const bool rightIsOrderedTypeParameter
            = right.kind == StaticType::TypeParameter
            && SemanticTypes::satisfiesCapability(right, capabilityType("Ord"));
        if ((!leftIsOrderedTypeParameter && left.kind != StaticType::Number)
            || (!rightIsOrderedTypeParameter && right.kind != StaticType::Number)) {
            throw TypeError(expression.op, binaryTypesMessage(expression, left, right));
        }
        return simpleType(StaticType::Bool);
    }
    case TokenType::EqualEqual:
    case TokenType::BangEqual:
        requireCapability(left, "Eq");
        requireCapability(right, "Eq");
        return simpleType(StaticType::Bool);
    default:
        throw TypeError(expression.op, "unsupported binary operator `" + expression.op.lexeme + "`");
    }
}

bool TypeChecker::isGlobalBinding(const Binding& binding) const
{
    return binding.scopeDepth == 0;
}

bool TypeChecker::isCurrentFunctionBinding(const Binding& binding) const
{
    return binding.functionDepth == functionDepth_;
}
