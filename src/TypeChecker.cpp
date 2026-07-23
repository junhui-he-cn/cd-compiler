#include "TypeChecker.hpp"

#include "NativeStdlib.hpp"

#include <stdexcept>
#include <utility>

namespace {

TypeInfo logicalResultType(const TypeInfo& left, const TypeInfo& right)
{
    if (!isKnown(left) || !isKnown(right)) {
        return unknownType();
    }
    if (left.kind == right.kind) {
        return left;
    }
    return unknownType();
}

TypeInfo mergeReturnTypes(const TypeInfo& current, const TypeInfo& next)
{
    if (compatible(current, next) && compatible(next, current) && current.kind == next.kind) {
        if (current.kind != StaticType::Function || typeInfoName(current) == typeInfoName(next)) {
            return current;
        }
    }
    if (!isKnown(current) || !isKnown(next)) {
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
    std::optional<TypeInfo> merged = mergeArrayElementTypes(*left.elementType, *right.elementType);
    if (!merged) {
        return simpleType(StaticType::Array);
    }
    return arrayType(std::move(*merged));
}

TypeInfo mergedMapType(const TypeInfo& left, const TypeInfo& right)
{
    if (left.kind != StaticType::Map || right.kind != StaticType::Map
        || !left.keyType || !right.keyType || !left.valueType || !right.valueType) {
        return simpleType(StaticType::Map);
    }
    std::optional<TypeInfo> key = mergeArrayElementTypes(*left.keyType, *right.keyType);
    std::optional<TypeInfo> value = mergeArrayElementTypes(*left.valueType, *right.valueType);
    if (!key || !value) {
        return simpleType(StaticType::Map);
    }
    return mapType(std::move(*key), std::move(*value));
}

bool mapKeyTypeAllowed(const TypeInfo& type)
{
    if (!isKnown(type)) {
        return true;
    }
    if (isNullable(type)) {
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

bool isPrimitiveMatchKind(StaticType kind)
{
    return kind == StaticType::Nil
        || kind == StaticType::Number
        || kind == StaticType::Bool
        || kind == StaticType::String;
}

const TypeInfo* primitiveMatchBaseType(const TypeInfo& type)
{
    const TypeInfo* base = isNullable(type) ? type.nullableOf.get() : &type;
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

std::string binaryTypesMessage(const BinaryExpr& expression, const TypeInfo& left, const TypeInfo& right)
{
    return "binary `" + expression.op.lexeme + "` expects numbers, got "
        + typeInfoName(left) + " and " + typeInfoName(right);
}

void rethrowWithModuleContext(const DiagnosticError& error, const ModuleStmt& module)
{
    if (error.location()) {
        throw FileDiagnosticError(error, DiagnosticSourceContext{module.path, module.source, false});
    }
    throw error;
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


const std::string& ResolvedNames::letName(const LetStmt& statement) const
{
    const auto found = letNames_.find(&statement);
    if (found == letNames_.end()) {
        throw std::logic_error("missing resolved let name");
    }
    return found->second;
}

const std::string& ResolvedNames::functionName(const FunctionStmt& statement) const
{
    const auto found = functionNames_.find(&statement);
    if (found == functionNames_.end()) {
        throw std::logic_error("missing resolved function name");
    }
    return found->second;
}

const std::vector<std::string>& ResolvedNames::parameterNames(const FunctionStmt& statement) const
{
    const auto found = parameterNames_.find(&statement);
    if (found == parameterNames_.end()) {
        throw std::logic_error("missing resolved parameter names");
    }
    return found->second;
}

const std::string& ResolvedNames::functionName(const FunctionExpr& expression) const
{
    const auto found = functionExpressionNames_.find(&expression);
    if (found == functionExpressionNames_.end()) {
        throw std::logic_error("missing resolved function expression name");
    }
    return found->second;
}

const std::vector<std::string>& ResolvedNames::parameterNames(const FunctionExpr& expression) const
{
    const auto found = functionExpressionParameterNames_.find(&expression);
    if (found == functionExpressionParameterNames_.end()) {
        throw std::logic_error("missing resolved function expression parameter names");
    }
    return found->second;
}

const std::string& ResolvedNames::methodName(const MethodDecl& method) const
{
    const auto found = methodNames_.find(&method);
    if (found == methodNames_.end()) {
        throw std::logic_error("missing resolved method name");
    }
    return found->second;
}

const std::vector<std::string>& ResolvedNames::methodParameterNames(const MethodDecl& method) const
{
    const auto found = methodParameterNames_.find(&method);
    if (found == methodParameterNames_.end()) {
        throw std::logic_error("missing resolved method parameter names");
    }
    return found->second;
}

bool ResolvedNames::hasVariable(const VariableExpr& expression) const
{
    return variableNames_.find(&expression) != variableNames_.end();
}

const std::string& ResolvedNames::variableName(const VariableExpr& expression) const
{
    const auto found = variableNames_.find(&expression);
    if (found == variableNames_.end()) {
        throw std::logic_error("missing resolved variable name");
    }
    return found->second;
}

BindingId ResolvedNames::variableBindingId(const VariableExpr& expression) const
{
    const auto found = variableBindingIds_.find(&expression);
    if (found == variableBindingIds_.end()) {
        throw std::logic_error("missing resolved variable binding ID");
    }
    return found->second;
}

const std::string& ResolvedNames::assignmentName(const AssignExpr& expression) const
{
    const auto found = assignmentNames_.find(&expression);
    if (found == assignmentNames_.end()) {
        throw std::logic_error("missing resolved assignment name");
    }
    return found->second;
}

BindingId ResolvedNames::assignmentBindingId(const AssignExpr& expression) const
{
    const auto found = assignmentBindingIds_.find(&expression);
    if (found == assignmentBindingIds_.end()) {
        throw std::logic_error("missing resolved assignment binding ID");
    }
    return found->second;
}

const std::string& ResolvedNames::compoundAssignmentName(const CompoundAssignExpr& expression) const
{
    const auto found = compoundAssignmentNames_.find(&expression);
    if (found == compoundAssignmentNames_.end()) {
        throw std::logic_error("missing resolved compound assignment name");
    }
    return found->second;
}

BindingId ResolvedNames::compoundAssignmentBindingId(const CompoundAssignExpr& expression) const
{
    const auto found = compoundAssignmentBindingIds_.find(&expression);
    if (found == compoundAssignmentBindingIds_.end()) {
        throw std::logic_error("missing resolved compound assignment binding ID");
    }
    return found->second;
}

BindingId ResolvedNames::letBindingId(const LetStmt& statement) const
{
    const auto found = letBindingIds_.find(&statement);
    if (found == letBindingIds_.end()) {
        throw std::logic_error("missing resolved let binding ID");
    }
    return found->second;
}

const std::string& ResolvedNames::forInVariableName(const ForInStmt& statement) const
{
    const auto found = forInVariableNames_.find(&statement);
    if (found == forInVariableNames_.end()) {
        throw std::logic_error("missing resolved for-in variable name");
    }
    return found->second;
}

BindingId ResolvedNames::forInBindingId(const ForInStmt& statement) const
{
    const auto found = forInBindingIds_.find(&statement);
    if (found == forInBindingIds_.end()) {
        throw std::logic_error("missing resolved for-in binding ID");
    }
    return found->second;
}

bool ResolvedNames::hasScope(const Stmt& statement) const
{
    return scopeIds_.find(&statement) != scopeIds_.end();
}

ScopeId ResolvedNames::scopeId(const Stmt& statement) const
{
    const auto found = scopeIds_.find(&statement);
    if (found == scopeIds_.end()) {
        throw std::logic_error("missing resolved scope ID");
    }
    return found->second;
}

const TypeBinding& ResolvedNames::binding(BindingId id) const
{
    const auto found = bindings_.find(id);
    if (found == bindings_.end()) {
        throw std::logic_error("missing binding ID");
    }
    return found->second;
}

std::size_t ResolvedNames::bindingCount() const
{
    return bindings_.size();
}

std::size_t ResolvedNames::bindingShadowMismatchCount() const
{
    return bindingShadowMismatches_;
}

bool ResolvedNames::hasFieldAccess(const FieldAccessExpr& expression) const
{
    return fieldAccessNames_.find(&expression) != fieldAccessNames_.end();
}

const std::string& ResolvedNames::fieldAccessName(const FieldAccessExpr& expression) const
{
    const auto found = fieldAccessNames_.find(&expression);
    if (found == fieldAccessNames_.end()) {
        throw std::logic_error("missing resolved field access name");
    }
    return found->second;
}

bool ResolvedNames::hasMemberCallCallee(const MemberCallExpr& expression) const
{
    return memberCallCalleeNames_.find(&expression) != memberCallCalleeNames_.end();
}

const std::string& ResolvedNames::memberCallCalleeName(const MemberCallExpr& expression) const
{
    const auto found = memberCallCalleeNames_.find(&expression);
    if (found == memberCallCalleeNames_.end()) {
        throw std::logic_error("missing resolved member call callee name");
    }
    return found->second;
}

bool ResolvedNames::memberCallPassesReceiver(const MemberCallExpr& expression) const
{
    const auto found = memberCallPassesReceiver_.find(&expression);
    if (found == memberCallPassesReceiver_.end()) {
        throw std::logic_error("missing member call receiver mode");
    }
    return found->second;
}

const MethodDecl* ResolvedNames::memberCallMethodTarget(const MemberCallExpr& expression) const
{
    const auto found = memberCallMethodTargets_.find(&expression);
    return found == memberCallMethodTargets_.end() ? nullptr : found->second;
}

bool ResolvedNames::hasVariantConstructor(const MemberCallExpr& expression) const
{
    return variantConstructors_.find(&expression) != variantConstructors_.end();
}

const std::string& ResolvedNames::variantEnumName(const MemberCallExpr& expression) const
{
    const auto found = variantConstructors_.find(&expression);
    if (found == variantConstructors_.end()) {
        throw std::logic_error("missing resolved variant enum name");
    }
    return found->second.first;
}

const std::string& ResolvedNames::variantName(const MemberCallExpr& expression) const
{
    const auto found = variantConstructors_.find(&expression);
    if (found == variantConstructors_.end()) {
        throw std::logic_error("missing resolved variant name");
    }
    return found->second.second;
}

bool ResolvedNames::hasPatternVariable(const VariablePattern& pattern) const
{
    return patternVariableNames_.find(&pattern) != patternVariableNames_.end();
}

const std::string& ResolvedNames::patternVariableName(const VariablePattern& pattern) const
{
    const auto found = patternVariableNames_.find(&pattern);
    if (found == patternVariableNames_.end()) {
        throw std::logic_error("missing resolved pattern variable name");
    }
    return found->second;
}

BindingId ResolvedNames::patternVariableBindingId(const VariablePattern& pattern) const
{
    const auto found = patternVariableBindingIds_.find(&pattern);
    if (found == patternVariableBindingIds_.end()) {
        throw std::logic_error("missing resolved pattern variable binding ID");
    }
    return found->second;
}

DeclarationId ResolvedNames::declarationId(const Stmt& statement) const
{
    const auto found = declarationIds_.find(&statement);
    if (found == declarationIds_.end()) {
        throw std::logic_error("missing declaration ID");
    }
    return found->second;
}

SymbolId ResolvedNames::symbolId(const Stmt& statement) const
{
    const auto found = symbolIds_.find(&statement);
    if (found == symbolIds_.end()) {
        throw std::logic_error("missing symbol ID");
    }
    return found->second;
}

DeclarationId ResolvedNames::methodDeclarationId(const MethodDecl& method) const
{
    const auto found = methodDeclarationIds_.find(&method);
    if (found == methodDeclarationIds_.end()) {
        throw std::logic_error("missing method declaration ID");
    }
    return found->second;
}

SymbolId ResolvedNames::methodSymbolId(const MethodDecl& method) const
{
    const auto found = methodSymbolIds_.find(&method);
    if (found == methodSymbolIds_.end()) {
        throw std::logic_error("missing method symbol ID");
    }
    return found->second;
}

const std::string& ResolvedNames::patternEnumName(const VariantPattern& pattern) const
{
    const auto found = patternEnumNames_.find(&pattern);
    if (found == patternEnumNames_.end()) {
        throw std::logic_error("missing resolved pattern enum name");
    }
    return found->second;
}

const std::vector<std::size_t>& ResolvedNames::patternPayloadIndices(const VariantPattern& pattern) const
{
    const auto found = patternPayloadIndices_.find(&pattern);
    if (found == patternPayloadIndices_.end()) {
        throw std::logic_error("missing resolved pattern payload indices");
    }
    return found->second;
}

void ResolvedNames::clear()
{
    letNames_.clear();
    functionNames_.clear();
    parameterNames_.clear();
    functionExpressionNames_.clear();
    functionExpressionParameterNames_.clear();
    methodNames_.clear();
    methodParameterNames_.clear();
    variableNames_.clear();
    variableBindingIds_.clear();
    assignmentNames_.clear();
    assignmentBindingIds_.clear();
    compoundAssignmentNames_.clear();
    compoundAssignmentBindingIds_.clear();
    letBindingIds_.clear();
    declarationIds_.clear();
    symbolIds_.clear();
    methodDeclarationIds_.clear();
    methodSymbolIds_.clear();
    forInVariableNames_.clear();
    forInBindingIds_.clear();
    scopeIds_.clear();
    patternVariableBindingIds_.clear();
    bindings_.clear();
    bindingShadowMismatches_ = 0;
    fieldAccessNames_.clear();
    memberCallCalleeNames_.clear();
    memberCallPassesReceiver_.clear();
    memberCallMethodTargets_.clear();
    variantConstructors_.clear();
    patternVariableNames_.clear();
    patternEnumNames_.clear();
    patternPayloadIndices_.clear();
}

void ResolvedNames::recordBinding(const TypeBinding& binding)
{
    if (binding.bindingId.valid()) {
        bindings_.emplace(binding.bindingId, binding);
    }
}

void ResolvedNames::compareBindingName(const TypeBinding& binding)
{
    if (binding.bindingId.valid()) {
        const auto found = bindings_.find(binding.bindingId);
        if (found == bindings_.end() || found->second.resolvedName != binding.resolvedName) {
            ++bindingShadowMismatches_;
        }
    }
}

void ResolvedNames::recordLet(const LetStmt& statement, const TypeBinding& binding)
{
    letNames_.emplace(&statement, binding.resolvedName);
    letBindingIds_.emplace(&statement, binding.bindingId);
    recordDeclaration(statement, binding.declarationId, binding.symbolId);
    recordBinding(binding);
}

void ResolvedNames::recordDeclaration(const Stmt& statement, DeclarationId declaration, SymbolId symbol)
{
    if (declaration.valid()) {
        declarationIds_.emplace(&statement, declaration);
    }
    if (symbol.valid()) {
        symbolIds_.emplace(&statement, symbol);
    }
}

void ResolvedNames::recordMethodDeclaration(const MethodDecl& method, DeclarationId declaration, SymbolId symbol)
{
    if (declaration.valid()) {
        methodDeclarationIds_.emplace(&method, declaration);
    }
    if (symbol.valid()) {
        methodSymbolIds_.emplace(&method, symbol);
    }
}

void ResolvedNames::recordFunction(const FunctionStmt& statement, std::string name)
{
    functionNames_.emplace(&statement, std::move(name));
}

void ResolvedNames::recordParameters(const FunctionStmt& statement, std::vector<std::string> names)
{
    parameterNames_.emplace(&statement, std::move(names));
}

void ResolvedNames::recordFunction(const FunctionExpr& expression, std::string name)
{
    functionExpressionNames_.emplace(&expression, std::move(name));
}

void ResolvedNames::recordParameters(const FunctionExpr& expression, std::vector<std::string> names)
{
    functionExpressionParameterNames_.emplace(&expression, std::move(names));
}

void ResolvedNames::recordMethod(const MethodDecl& method, std::string name)
{
    methodNames_.emplace(&method, std::move(name));
}

void ResolvedNames::recordMethodParameters(const MethodDecl& method, std::vector<std::string> names)
{
    methodParameterNames_.emplace(&method, std::move(names));
}

void ResolvedNames::recordVariable(const VariableExpr& expression, const TypeBinding& binding)
{
    variableNames_.emplace(&expression, binding.resolvedName);
    variableBindingIds_.emplace(&expression, binding.bindingId);
    compareBindingName(binding);
}

void ResolvedNames::recordAssignment(const AssignExpr& expression, const TypeBinding& binding)
{
    assignmentNames_.emplace(&expression, binding.resolvedName);
    assignmentBindingIds_.emplace(&expression, binding.bindingId);
    compareBindingName(binding);
}

void ResolvedNames::recordCompoundAssignment(const CompoundAssignExpr& expression, const TypeBinding& binding)
{
    compoundAssignmentNames_.emplace(&expression, binding.resolvedName);
    compoundAssignmentBindingIds_.emplace(&expression, binding.bindingId);
    compareBindingName(binding);
}

void ResolvedNames::recordForInVariable(const ForInStmt& statement, const TypeBinding& binding)
{
    forInVariableNames_.emplace(&statement, binding.resolvedName);
    forInBindingIds_.emplace(&statement, binding.bindingId);
    recordDeclaration(statement, binding.declarationId, binding.symbolId);
    compareBindingName(binding);
}

void ResolvedNames::recordScope(const Stmt& statement, ScopeId id)
{
    scopeIds_.emplace(&statement, id);
}

void ResolvedNames::recordFieldAccess(const FieldAccessExpr& expression, std::string name)
{
    fieldAccessNames_.emplace(&expression, std::move(name));
}

void ResolvedNames::recordMemberCallCallee(
    const MemberCallExpr& expression,
    std::string name,
    bool passesReceiver,
    const MethodDecl* method)
{
    memberCallCalleeNames_.emplace(&expression, std::move(name));
    memberCallPassesReceiver_.emplace(&expression, passesReceiver);
    if (method) {
        memberCallMethodTargets_.emplace(&expression, method);
    }
}

void ResolvedNames::recordVariantConstructor(
    const MemberCallExpr& expression,
    std::string enumName,
    std::string variantName)
{
    variantConstructors_.emplace(
        &expression,
        std::make_pair(std::move(enumName), std::move(variantName)));
}

void ResolvedNames::recordPatternVariable(const VariablePattern& pattern, const TypeBinding& binding)
{
    patternVariableNames_.emplace(&pattern, binding.resolvedName);
    patternVariableBindingIds_.emplace(&pattern, binding.bindingId);
    compareBindingName(binding);
}

void ResolvedNames::recordPatternVariant(
    const VariantPattern& pattern,
    std::string enumName,
    std::vector<std::size_t> payloadIndices)
{
    patternEnumNames_.emplace(&pattern, std::move(enumName));
    patternPayloadIndices_.emplace(&pattern, std::move(payloadIndices));
}

const ResolvedNames& TypeChecker::check(const Program& program)
{
    declarationIndex_ = DeclarationIndex::collect(program);
    declarationIndexMismatchCount_ = 0;
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
    moduleInterfaces_.clear();
    checkedModules_.clear();
    moduleStack_.clear();
    resolvedNames_.clear();
    currentProgram_ = &program;
    nextResolvedName_ = 0;
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
        for (const auto& statement : program.statements) {
            if (const auto* module = dynamic_cast<const ModuleStmt*>(statement.get())) {
                checkModule(*module);
            } else {
                checkStatement(*statement);
            }
        }
    } else {
        beginScope();
        checkStatementList(program.statements);
        endScope();
    }

    buildModuleInterfaces(program);
    declarationIndexMismatchCount_ = declarationIndex_.compareResolvedNames(resolvedNames_);
    currentProgram_ = nullptr;
    return resolvedNames_;
}

const std::vector<ModuleInterface>& TypeChecker::moduleInterfaces() const
{
    return moduleInterfaces_;
}

const DeclarationIndex& TypeChecker::declarationIndex() const
{
    return declarationIndex_;
}

std::size_t TypeChecker::declarationIndexMismatchCount() const
{
    return declarationIndexMismatchCount_;
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
        TypeInfo constraint = resolveAnnotation(*parameter.constraint);
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
    resolvedNames_.recordBinding(binding);
    return binding;
}

TypeChecker::Binding TypeChecker::declareVariable(
    const LetStmt& statement,
    TypeInfo type,
    bool explicitType)
{
    Binding binding = declareVariable(statement.name, std::move(type), explicitType);
    resolvedNames_.recordLet(statement, binding);
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
    resolvedNames_.recordBinding(binding);
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
        {}};
    structTypes_.emplace(statement.name.lexeme, std::move(declaration));
    structDeclarations_.emplace(statement.name.lexeme, &statement);
    structCheckStates_.emplace(statement.name.lexeme, StructCheckState::Declared);
    resolvedNames_.recordDeclaration(
        statement,
        DeclarationId{nextDeclarationId_++},
        SymbolId{nextSymbolId_++});

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
            resolvedNames_.recordDeclaration(
                *enumDecl,
                DeclarationId{nextDeclarationId_++},
                SymbolId{nextSymbolId_++});
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
        resolvedNames_.recordScope(*block, currentScopeId());
        checkStatementList(block->statements);
        endScope();
        return;
    }

    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
        checkExpression(*ifStmt->condition);
        const BranchFlowFacts branchFacts = flowFacts_.factsForIfCondition(
            *ifStmt->condition,
            [this](const VariableExpr& variable) {
                return nonNilNarrowingForVariable(variable);
            });
        flowFacts_.withNarrowings(branchFacts.thenNarrowings, [&]() {
            checkStatement(*ifStmt->thenBranch);
        });
        if (ifStmt->elseBranch) {
            flowFacts_.withNarrowings(branchFacts.elseNarrowings, [&]() {
                checkStatement(*ifStmt->elseBranch);
            });
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

    if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
        beginScope();
        resolvedNames_.recordScope(*forStmt, currentScopeId());
        if (forStmt->initializer) {
            checkStatement(*forStmt->initializer);
        }
        if (forStmt->condition) {
            checkExpression(*forStmt->condition);
        }
        if (forStmt->increment) {
            checkExpression(*forStmt->increment);
        }
        ++loopDepth_;
        checkStatement(*forStmt->body);
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
        resolvedNames_.recordScope(*forInStmt, currentScopeId());
        const Binding itemBinding = declareVariable(forInStmt->variable, elementType, false);
        resolvedNames_.recordForInVariable(*forInStmt, itemBinding);
        ++loopDepth_;
        if (const auto* body = dynamic_cast<const BlockStmt*>(forInStmt->body.get())) {
            resolvedNames_.recordScope(*body, currentScopeId());
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


void TypeChecker::buildModuleInterfaces(const Program& program)
{
    moduleInterfaces_.clear();

    for (const auto& statement : program.statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (!module) {
            continue;
        }

        ModuleInterface interfaceInfo;
        interfaceInfo.moduleId = module->moduleId;
        interfaceInfo.path = module->path;
        interfaceInfo.isEntry = module->isEntry;

        if (const ModuleValueExports* exports = moduleSymbols_.valueExports(module->moduleId)) {
            for (const auto& entry : *exports) {
                interfaceInfo.values.push_back(ModuleInterfaceValue{entry.first, entry.second.type});
            }
        }

        if (const ModuleStructExports* structExports = moduleSymbols_.structExports(module->moduleId)) {
            for (const auto& entry : *structExports) {
                ModuleInterfaceStruct structInfo;
                structInfo.name = entry.first;
                structInfo.genericParameters = entry.second.genericParameters;
                structInfo.genericParameterConstraints = entry.second.genericParameterConstraints;
                for (const StructFieldType& field : entry.second.fields) {
                    structInfo.fields.push_back(ModuleInterfaceField{field.name.lexeme, field.type});
                }

                if (const ModuleMethodExports* methodExports = moduleSymbols_.methodExports(module->moduleId)) {
                    const auto methodsForStruct = methodExports->find(entry.first);
                    if (methodsForStruct != methodExports->end()) {
                        for (const auto& methodEntry : methodsForStruct->second) {
                            structInfo.methods.push_back(ModuleInterfaceMethod{
                                methodEntry.first,
                                methodEntry.second.parameterTypes,
                                methodEntry.second.returnType,
                                methodEntry.second.genericParameters,
                                methodEntry.second.genericParameterConstraints});
                        }
                    }
                }

                interfaceInfo.structs.push_back(std::move(structInfo));
            }
        }

        if (const ModuleEnumExports* enumExports = moduleSymbols_.enumExports(module->moduleId)) {
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

        moduleInterfaces_.push_back(std::move(interfaceInfo));
    }
}

void TypeChecker::checkModule(const ModuleStmt& module)
{
    if (checkedModules_.find(module.moduleId) != checkedModules_.end()) {
        return;
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

    moduleStack_.push_back(module.moduleId);
    beginScope();
    try {
        checkStatementList(module.statements);
    } catch (const FileDiagnosticError&) {
        throw;
    } catch (const DiagnosticError& error) {
        rethrowWithModuleContext(error, module);
    }
    endScope();
    moduleStack_.pop_back();

    checkedModules_.insert(module.moduleId);

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
    if (!currentProgram_) {
        throw TypeError(statement.keyword, "internal error: import without program");
    }

    const std::size_t currentModuleId = moduleStack_.back();
    if (!statement.alias && !moduleSymbols_.markDirectImport(currentModuleId, statement.resolvedModuleId)) {
        return;
    }

    const ModuleStmt* imported = findModule(*currentProgram_, statement.resolvedModuleId);
    if (!imported) {
        throw TypeError(statement.keyword, "internal error: unresolved import module");
    }
    checkModule(*imported);

    if (statement.alias) {
        NamespaceImport namespaceImport;
        if (const ModuleValueExports* exports = moduleSymbols_.valueExports(imported->moduleId)) {
            namespaceImport.values = *exports;
        }
        if (const ModuleStructExports* structExports = moduleSymbols_.structExports(imported->moduleId)) {
            namespaceImport.structs = *structExports;
        }
        if (const ModuleEnumExports* enumExports = moduleSymbols_.enumExports(imported->moduleId)) {
            namespaceImport.enums = *enumExports;
        }
        if (const ModuleMethodExports* methodExports = moduleSymbols_.methodExports(imported->moduleId)) {
            namespaceImport.methods = *methodExports;
        }
        declareNamespaceAlias(statement, std::move(namespaceImport));
        return;
    }

    if (const ModuleValueExports* exports = moduleSymbols_.valueExports(imported->moduleId)) {
        for (const auto& entry : *exports) {
            Token name{TokenType::Identifier, entry.first, statement.keyword.line, statement.keyword.column};
            declareImportedVariable(name, entry.second);
        }
    }

    if (const ModuleStructExports* structExports = moduleSymbols_.structExports(imported->moduleId)) {
        for (const auto& entry : *structExports) {
            if (structTypes_.find(entry.first) != structTypes_.end()) {
                Token name{TokenType::Identifier, entry.first, statement.keyword.line, statement.keyword.column};
                throw TypeError(name, "duplicate struct `" + entry.first + "`");
            }
            structTypes_.emplace(entry.first, entry.second);
        }
    }

    if (const ModuleEnumExports* enumExports = moduleSymbols_.enumExports(imported->moduleId)) {
        for (const auto& entry : *enumExports) {
            if (enumTypes_.find(entry.first) != enumTypes_.end()
                || structTypes_.find(entry.first) != structTypes_.end()) {
                Token name{TokenType::Identifier, entry.first, statement.keyword.line, statement.keyword.column};
                throw TypeError(name, "duplicate type " + entry.first);
            }
            enumTypes_.emplace(entry.first, entry.second);
        }
    }

    if (const ModuleMethodExports* methodExports = moduleSymbols_.methodExports(imported->moduleId)) {
        importMethodExports(statement.keyword, *methodExports);
    }
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
    std::size_t targetModuleId,
    std::size_t currentModuleId,
    const std::string& structName)
{
    const ModuleMethodExports* methodExports = moduleSymbols_.methodExports(targetModuleId);
    if (!methodExports) {
        return;
    }
    const auto found = methodExports->find(structName);
    if (found == methodExports->end()) {
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
    const ModuleStmt* target = findModule(*currentProgram_, statement.resolvedModuleId);
    if (!target) {
        throw TypeError(statement.keyword, "internal error: unresolved re-export module");
    }
    checkModule(*target);

    const ModuleValueExports* valueExports = moduleSymbols_.valueExports(target->moduleId);
    const ModuleStructExports* structExports = moduleSymbols_.structExports(target->moduleId);
    const ModuleEnumExports* enumExports = moduleSymbols_.enumExports(target->moduleId);

    for (const Token& name : statement.names) {
        ensureExportNameAvailable(currentModuleId, name);

        bool exported = false;
        if (valueExports) {
            const auto found = valueExports->find(name.lexeme);
            if (found != valueExports->end()) {
                moduleSymbols_.recordValueExport(currentModuleId, name.lexeme, found->second);
                exported = true;
            }
        }

        if (structExports) {
            const auto found = structExports->find(name.lexeme);
            if (found != structExports->end()) {
                moduleSymbols_.recordStructExport(currentModuleId, name.lexeme, found->second);
                forwardStructMethodExports(target->moduleId, currentModuleId, name.lexeme);
                exported = true;
            }
        }

        if (enumExports) {
            const auto found = enumExports->find(name.lexeme);
            if (found != enumExports->end()) {
                moduleSymbols_.recordEnumExport(currentModuleId, name.lexeme, found->second);
                exported = true;
            }
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
    if (context.expectedReturnType && !compatible(*context.expectedReturnType, type)) {
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
    const Stmt& last = *body.back();
    if (dynamic_cast<const ReturnStmt*>(&last)) {
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

void TypeChecker::checkImplicitNilReturn(
    const Token& functionToken,
    const std::string& functionLabel,
    const TypeInfo& expectedReturnType) const
{
    if (!compatible(expectedReturnType, simpleType(StaticType::Nil))) {
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
        return *expectedReturnType;
    }

    return context.sawReturn ? context.returnType : simpleType(StaticType::Nil);
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
        typeParameterConstraints(statement.typeParameters)};
    std::unordered_map<std::string, Token> fieldNames;
    for (const StructFieldDecl& field : statement.fields) {
        if (fieldNames.find(field.name.lexeme) != fieldNames.end()) {
            throw TypeError(field.name,
                "duplicate field `" + field.name.lexeme + "` in struct `" + statement.name.lexeme + "`");
        }
        fieldNames.emplace(field.name.lexeme, field.name);
        declaration.fields.push_back(StructFieldType{field.name, resolveStructFieldAnnotation(field)});
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
    if (isNullable(result) && result.nullableOf) {
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
    if (isBuiltinMemberName(method.name.lexeme)) {
        throw TypeError(method.name, "method `" + method.name.lexeme + "` conflicts with builtin member call `" + method.name.lexeme + "`");
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
    resolvedNames_.recordMethodDeclaration(
        method,
        DeclarationId{nextDeclarationId_++},
        SymbolId{nextSymbolId_++});
    resolvedNames_.recordMethod(method, info.resolvedName);
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
    resolvedNames_.recordMethodParameters(declaration, std::move(parameterNames));

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
            const TypeInfo headerConstraint = resolveAnnotation(
                *statement.typeParameters[i].constraint);
            const std::shared_ptr<TypeInfo>& declaredConstraint
                = i < structType->genericParameterConstraints.size()
                ? structType->genericParameterConstraints[i]
                : nullptr;
            if (!declaredConstraint
                || !compatible(*declaredConstraint, headerConstraint)
                || !compatible(headerConstraint, *declaredConstraint)) {
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
    resolvedNames_.recordDeclaration(statement, functionBinding.declarationId, functionBinding.symbolId);
    resolvedNames_.recordFunction(statement, functionBinding.resolvedName);

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
    resolvedNames_.recordParameters(statement, std::move(parameterNames));

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
    endTypeParameterScope();
}

void TypeChecker::inferTypeArguments(
    const TypeInfo& expected,
    const TypeInfo& actual,
    TypeSubstitutions& substitutions,
    const Token& callToken) const
{
    if (expected.kind == StaticType::TypeParameter && expected.typeParameterName) {
        if (!isKnown(actual)) {
            return;
        }
        const auto [it, inserted] = substitutions.emplace(*expected.typeParameterName, actual);
        if (!inserted
            && (!compatible(it->second, actual) || !compatible(actual, it->second))) {
            throw TypeError(callToken,
                "type parameter " + *expected.typeParameterName + " inferred as "
                    + typeInfoName(it->second) + " and " + typeInfoName(actual));
        }
        return;
    }

    if (expected.kind == StaticType::Array && actual.kind == StaticType::Array
        && expected.elementType && actual.elementType) {
        inferTypeArguments(*expected.elementType, *actual.elementType, substitutions, callToken);
        return;
    }

    if (expected.kind == StaticType::Map && actual.kind == StaticType::Map
        && expected.keyType && actual.keyType && expected.valueType && actual.valueType) {
        inferTypeArguments(*expected.keyType, *actual.keyType, substitutions, callToken);
        inferTypeArguments(*expected.valueType, *actual.valueType, substitutions, callToken);
        return;
    }

    if (expected.kind == StaticType::Struct && actual.kind == StaticType::Struct
        && expected.structName && actual.structName
        && expected.structName == actual.structName
        && expected.typeArguments.size() == actual.typeArguments.size()) {
        for (std::size_t i = 0; i < expected.typeArguments.size(); ++i) {
            inferTypeArguments(expected.typeArguments[i], actual.typeArguments[i], substitutions, callToken);
        }
        return;
    }

    if (expected.kind == StaticType::Nullable && expected.nullableOf) {
        if (actual.kind == StaticType::Nil) {
            return;
        }
        if (actual.kind == StaticType::Nullable && actual.nullableOf) {
            inferTypeArguments(*expected.nullableOf, *actual.nullableOf, substitutions, callToken);
        } else {
            inferTypeArguments(*expected.nullableOf, actual, substitutions, callToken);
        }
        return;
    }

    if (expected.kind == StaticType::Enum && actual.kind == StaticType::Enum
        && expected.enumName && actual.enumName
        && expected.enumName == actual.enumName
        && expected.typeArguments.size() == actual.typeArguments.size()) {
        for (std::size_t i = 0; i < expected.typeArguments.size(); ++i) {
            inferTypeArguments(expected.typeArguments[i], actual.typeArguments[i], substitutions, callToken);
        }
        return;
    }

    if (expected.kind == StaticType::Function && actual.kind == StaticType::Function
        && hasFunctionSignature(expected) && hasFunctionSignature(actual)
        && expected.parameterTypes.size() == actual.parameterTypes.size()) {
        for (std::size_t i = 0; i < expected.parameterTypes.size(); ++i) {
            inferTypeArguments(expected.parameterTypes[i], actual.parameterTypes[i], substitutions, callToken);
        }
        inferTypeArguments(*expected.returnType, *actual.returnType, substitutions, callToken);
    }
}

TypeInfo TypeChecker::substituteTypeParameters(
    const TypeInfo& type,
    const TypeSubstitutions& substitutions) const
{
    if (type.kind == StaticType::TypeParameter && type.typeParameterName) {
        const auto found = substitutions.find(*type.typeParameterName);
        if (found != substitutions.end()) {
            return found->second;
        }
    }

    TypeInfo result = type;
    if (type.elementType) {
        result.elementType = std::make_shared<TypeInfo>(
            substituteTypeParameters(*type.elementType, substitutions));
    }
    if (type.keyType) {
        result.keyType = std::make_shared<TypeInfo>(
            substituteTypeParameters(*type.keyType, substitutions));
    }
    if (type.valueType) {
        result.valueType = std::make_shared<TypeInfo>(
            substituteTypeParameters(*type.valueType, substitutions));
    }
    if (type.nullableOf) {
        result.nullableOf = std::make_shared<TypeInfo>(
            substituteTypeParameters(*type.nullableOf, substitutions));
    }
    if (type.returnType) {
        result.returnType = std::make_shared<TypeInfo>(
            substituteTypeParameters(*type.returnType, substitutions));
    }
    for (TypeInfo& parameter : result.parameterTypes) {
        parameter = substituteTypeParameters(parameter, substitutions);
    }
    for (TypeInfo& argument : result.typeArguments) {
        argument = substituteTypeParameters(argument, substitutions);
    }
    if (type.typeParameterConstraint) {
        result.typeParameterConstraint = std::make_shared<TypeInfo>(
            substituteTypeParameters(*type.typeParameterConstraint, substitutions));
    }
    for (std::shared_ptr<TypeInfo>& constraint : result.genericParameterConstraints) {
        if (constraint) {
            constraint = std::make_shared<TypeInfo>(
                substituteTypeParameters(*constraint, substitutions));
        }
    }
    return result;
}

void TypeChecker::validateGenericTypeArguments(
    const std::vector<std::string>& parameters,
    const std::vector<std::shared_ptr<TypeInfo>>& constraints,
    const TypeSubstitutions& substitutions,
    const Token& callToken,
    const std::string& context) const
{
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i >= constraints.size() || !constraints[i]) {
            continue;
        }
        const auto found = substitutions.find(parameters[i]);
        if (found == substitutions.end()) {
            continue;
        }
        if (!compatible(*constraints[i], found->second)) {
            const std::string prefix = context.empty() ? "" : context + ": ";
            throw TypeError(callToken,
                prefix + "type parameter " + parameters[i] + " must satisfy "
                    + typeInfoName(*constraints[i]) + ", got " + typeInfoName(found->second));
        }
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

    TypeInfo specialized = substituteTypeParameters(callbackType, substitutions);
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
    if (calleeType.kind != StaticType::Function || !hasFunctionSignature(calleeType)) {
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
            ? substituteTypeParameters(calleeType.parameterTypes[i], substitutions)
            : calleeType.parameterTypes[i];
        const TypeInfo& actual = checkedArguments[i].type;
        if (!compatible(expected, actual)) {
            throw TypeError(callToken,
                "argument " + std::to_string(i + 1) + " expects "
                    + typeInfoName(expected) + ", got " + typeInfoName(actual));
        }
    }

    const TypeInfo returnType = generic
        ? substituteTypeParameters(*calleeType.returnType, substitutions)
        : *calleeType.returnType;
    return CheckedExpression{returnType};
}

const TypeInfo* TypeChecker::contextualFunctionType(const TypeInfo* expectedType) const
{
    if (!expectedType || expectedType->kind != StaticType::Function || !hasFunctionSignature(*expectedType)) {
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

    resolvedNames_.recordFunction(expression, "<lambda>");

    std::vector<TypeInfo> declaredParameterTypes;
    declaredParameterTypes.reserve(expression.parameters.size());
    for (std::size_t i = 0; i < expression.parameters.size(); ++i) {
        const Parameter& parameter = expression.parameters[i];
        TypeInfo parameterType = parameter.typeName
            ? resolveAnnotation(*parameter.typeName)
            : (contextualSignature ? contextualSignature->parameterTypes[i] : unknownType());

        if (contextualSignature && parameter.typeName
            && !compatible(parameterType, contextualSignature->parameterTypes[i])) {
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
        if (expectedReturnType && !compatible(*contextualSignature->returnType, *expectedReturnType)) {
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
    resolvedNames_.recordParameters(expression, std::move(parameterNames));

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
    return substituteTypeParameters(field.type, substitutions);
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
        if (!compatible(expectedFieldType, actual.type)) {
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
    if (expectedStructType && isNullable(*expectedStructType)) {
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
        const TypeInfo expectedFieldType = substituteTypeParameters(field.type, substitutions);
        const CheckedExpression actual = checkExpressionInfo(
            *found->second->value, &expectedFieldType);
        if (!generic || !hasEscapingTypeParameter(expectedFieldType, {})) {
            if (!compatible(expectedFieldType, actual.type)) {
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
    if (expectedMatches && !compatible(*expectedStructType, declared)) {
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
    if (expectedEnumType && isNullable(*expectedEnumType)) {
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
        const TypeInfo payloadType = substituteTypeParameters(
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

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const TypeInfo payloadType = substituteTypeParameters(
            variant->payloadTypes[i], substitutions);
        if (!compatible(payloadType, arguments[i].type)) {
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
    resolvedNames_.recordVariantConstructor(expression, runtimeEnumName, expression.name.lexeme);
    return CheckedExpression{namedEnumType(enumName, std::move(typeArguments))};
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
        resolvedNames_.recordPatternVariable(*variable, binding);
        coversStruct = true;
        return true;
    }

    if (const auto* recordPattern = dynamic_cast<const RecordPattern*>(&pattern)) {
        const TypeInfo* structExpectedType = isNullable(expectedType)
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
        for (const RecordPatternField& field : recordPattern->fields) {
            if (!usedFields.insert(field.name.lexeme).second) {
                throw TypeError(field.name,
                    "duplicate field `" + field.name.lexeme
                        + "` in record pattern for struct `" + patternTypeName + "`");
            }
            const StructFieldType* structField = findStructField(*structType, field.name.lexeme);
            if (!structField) {
                throw TypeError(field.name,
                    "struct `" + patternTypeName + "` has no field `"
                        + field.name.lexeme + "` in record pattern");
            }

            std::unordered_set<std::string> nestedCoverage;
            std::unordered_set<std::string> nestedLiterals;
            bool nestedCoversNil = false;
            bool nestedCoversStruct = false;
            const TypeInfo fieldType = structFieldTypeForValue(
                *structExpectedType, *structType, *structField);
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

        coversStruct = universal;
        return isNullable(expectedType) ? false : universal;
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
                if (!compatible(merged.type, alternativeBinding->second.type)
                    || !compatible(alternativeBinding->second.type, merged.type)) {
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
                    resolvedNames_.recordPatternVariable(*occurrence, binding);
                }
            }
        }

        coversNil = coversNil || mergedCoversNil;
        coversStruct = coversStruct || mergedCoversStruct;
        if (isNullable(expectedType)
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
                return true;
            }
            if (isNullable(expectedType)) {
                coversNil = true;
                return false;
            }
            if (expectedType.kind == StaticType::Enum) {
                throw TypeError(literal->value, "literal patterns are not valid for enum values");
            }
        }

        const TypeInfo* valueType = isNullable(expectedType)
            ? expectedType.nullableOf.get()
            : &expectedType;
        if (!valueType || !compatible(*valueType, literalType)) {
            throw TypeError(literal->value,
                "literal pattern expects " + typeInfoName(expectedType)
                    + ", got " + typeInfoName(literalType));
        }
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
    if (isNullable(expectedType)) {
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
    resolvedNames_.recordPatternVariant(*variantPattern, runtimeEnumName, payloadIndices);

    for (std::size_t i = 0; i < variantPattern->arguments.size(); ++i) {
        std::unordered_set<std::string> nestedCoverage;
        std::unordered_set<std::string> nestedLiterals;
        bool nestedCoversNil = false;
        bool nestedCoversStruct = false;
        const TypeInfo payloadType = substituteTypeParameters(
            variant->payloadTypes[payloadIndices[i]], substitutions);
        checkPattern(
            *variantPattern->arguments[i],
            payloadType,
            nestedCoverage,
            nestedLiterals,
            nestedCoversNil,
            nestedCoversStruct,
            deferredBindings);
    }
    return false;
}

void TypeChecker::checkMatch(const MatchStmt& statement)
{
    const TypeInfo scrutineeType = checkExpression(*statement.value);
    const bool nullableEnum = isNullable(scrutineeType)
        && scrutineeType.nullableOf->kind == StaticType::Enum
        && scrutineeType.nullableOf->enumName;
    const bool nullableStruct = isNullable(scrutineeType)
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
            checkExpression(*arm.guard);
        }
        checkStatement(*arm.body);
        endScope();
    }

    if (!coversAll) {
        if (isNullable(scrutineeType) && !coveredNil) {
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
    if (!binding || !isNullable(binding->type)) {
        return std::nullopt;
    }
    return FlowNarrowing{binding->resolvedName, *binding->type.nullableOf};
}

TypeInfo TypeChecker::inferArrayElementType(const ArrayExpr& expression)
{
    std::optional<TypeInfo> current;
    for (const auto& element : expression.elements) {
        TypeInfo elementType = checkExpression(*element);
        if (!isKnown(elementType)) {
            return unknownType();
        }
        if (!current) {
            current = std::move(elementType);
            continue;
        }
        std::optional<TypeInfo> merged = mergeArrayElementTypes(*current, elementType);
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

    if (!isKnown(valueType)) {
        target.type = simpleType(StaticType::Array);
        return;
    }

    if (!isKnown(target.type) || (target.type.kind == StaticType::Array && !target.type.elementType)) {
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

    std::optional<TypeInfo> merged = mergeArrayElementTypes(*target.type.elementType, valueType);
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
            if (!compatible(*expectedType->elementType, actual.type)) {
                throw TypeError(expression.bracket,
                    "array element expects " + typeInfoName(*expectedType->elementType)
                        + ", got " + typeInfoName(actual.type));
            }
        }
        return CheckedExpression{*expectedType};
    }

    const TypeInfo element = inferArrayElementType(expression);
    if (isKnown(element)) {
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
        if (isKnown(currentKey) && !mapKeyTypeAllowed(currentKey)) {
            throw TypeError(entry.colon, "map key must be nil, number, bool, or string");
        }
        const TypeInfo currentValue = checkExpression(*entry.value);

        if (!isKnown(currentKey) || !isKnown(currentValue)) {
            hasUnknownComponent = true;
            continue;
        }

        if (!keyType) {
            keyType = currentKey;
        } else {
            std::optional<TypeInfo> merged = mergeArrayElementTypes(*keyType, currentKey);
            if (!merged) {
                hasUnknownComponent = true;
            } else {
                keyType = std::move(*merged);
            }
        }

        if (!valueType) {
            valueType = currentValue;
        } else {
            std::optional<TypeInfo> merged = mergeArrayElementTypes(*valueType, currentValue);
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
            if (isKnown(key.type) && !mapKeyTypeAllowed(key.type)) {
                throw TypeError(entry.colon, "map key must be nil, number, bool, or string");
            }
            if (!compatible(*expectedType->keyType, key.type)) {
                throw TypeError(entry.colon, "map key is incompatible with map key type");
            }

            const CheckedExpression value = checkExpressionInfo(*entry.value, expectedType->valueType.get());
            if (!compatible(*expectedType->valueType, value.type)) {
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
        return checkFunctionExpression(*function, expectedType);
    }

    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            if (findNamespace(variable->name.lexeme)) {
                throw TypeError(variable->name, "namespace alias `" + variable->name.lexeme + "` is not a value");
            }
            throw TypeError(variable->name, "undefined variable `" + variable->name.lexeme + "`");
        }
        resolvedNames_.recordVariable(*variable, *binding);
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
            if (hasFunctionSignature(target->type) && hasFunctionSignature(value.type)
                && target->type.parameterTypes.size() != value.type.parameterTypes.size()) {
                throw TypeError(assign->name,
                    "cannot assign function with " + std::to_string(value.type.parameterTypes.size())
                        + " parameters to `" + assign->name.lexeme
                        + "` of type function with " + std::to_string(target->type.parameterTypes.size()) + " parameters");
            }

            if (target->explicitType && !compatible(target->type, value.type)) {
                throw TypeError(assign->name, "cannot assign " + typeInfoName(value.type) + " to `" + assign->name.lexeme
                    + "` of type " + typeInfoName(target->type));
            }

            if (!target->explicitType) {
                target->type = value.type;
            }
        } else if (!compatible(target->type, value.type)) {
            const std::string targetTypeName = target->type.kind == StaticType::Function && !target->explicitType
                ? staticTypeName(StaticType::Function)
                : typeInfoName(target->type);
            throw TypeError(assign->name, "cannot assign " + typeInfoName(value.type) + " to `" + assign->name.lexeme
                + "` of type " + targetTypeName);
        } else if (!isKnown(target->type)) {
            target->type = value.type;
        }

        resolvedNames_.recordBinding(*target);
        resolvedNames_.recordAssignment(*assign, *target);
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

        if (!isKnown(target->type)) {
            target->type = simpleType(StaticType::Number);
        }
        resolvedNames_.recordBinding(*target);
        resolvedNames_.recordCompoundAssignment(*compound, *target);
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
        if (isNativeStdlibName(memberCall->name.lexeme)
            && !resolvedNames_.hasMemberCallCallee(*memberCall)
            && !resolvedNames_.hasVariantConstructor(*memberCall)) {
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
                resolvedNames_.recordFieldAccess(*field, found->second.resolvedName);
                CheckedExpression result{found->second.type};
                declarationIndex_.recordTypedExpression(*field, result.type);
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
                    "struct `" + *object.structName + "` has no field `" + field->name.lexeme + "`");
            }
            CheckedExpression result{
                structFieldTypeForValue(object, *structType, *structField)};
            declarationIndex_.recordTypedExpression(*field, result.type);
            return result;
        }
        CheckedExpression result{unknownType()};
        declarationIndex_.recordTypedExpression(*field, result.type);
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
    const bool nullableEnum = isNullable(scrutineeType)
        && scrutineeType.nullableOf->kind == StaticType::Enum
        && scrutineeType.nullableOf->enumName;
    const bool nullableStruct = isNullable(scrutineeType)
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
            checkExpression(*arm.guard);
        }

        const CheckedExpression result = checkExpressionInfo(*arm.value, expectedType);
        if (expectedType && !compatible(*expectedType, result.type)) {
            throw TypeError(arm.arrow,
                "match arm result expects " + typeInfoName(*expectedType)
                    + ", got " + typeInfoName(result.type));
        }
        if (!resultType) {
            resultType = result.type;
        } else if (isKnown(*resultType) && isKnown(result.type)
            && (!compatible(*resultType, result.type) || !compatible(result.type, *resultType))) {
            throw TypeError(arm.arrow,
                "match arm result expects " + typeInfoName(*resultType)
                    + ", got " + typeInfoName(result.type));
        } else {
            resultType = mergeReturnTypes(*resultType, result.type);
        }
        endScope();
    }

    if (!coversAll) {
        if (isNullable(scrutineeType) && !coveredNil) {
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
    if (isKnown(argument.type)
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
    if (callback.type.kind != StaticType::Function || !hasFunctionSignature(callback.type)) {
        return CheckedExpression{simpleType(StaticType::Array)};
    }
    if (callback.type.parameterTypes.size() != 1) {
        throw TypeError(callToken, "map expects callback with 1 argument");
    }
    const TypeInfo callbackType = specializeGenericCallback(
        callToken, callback.type, {elementType}, "map");
    if (elementType.kind != StaticType::Unknown
        && !compatible(callbackType.parameterTypes.front(), elementType)) {
        throw TypeError(callToken,
            "map callback expects " + typeInfoName(elementType)
                + ", got " + typeInfoName(callbackType.parameterTypes.front()));
    }
    if (callbackType.returnType && isKnown(*callbackType.returnType)) {
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
    if (predicate.type.kind == StaticType::Function && hasFunctionSignature(predicate.type)) {
        if (predicate.type.parameterTypes.size() != 1) {
            throw TypeError(callToken, "filter expects callback with 1 argument");
        }
        const TypeInfo predicateType = specializeGenericCallback(
            callToken, predicate.type, {elementType}, "filter");
        if (elementType.kind != StaticType::Unknown
            && !compatible(predicateType.parameterTypes.front(), elementType)) {
            throw TypeError(callToken,
                "filter callback expects " + typeInfoName(elementType)
                    + ", got " + typeInfoName(predicateType.parameterTypes.front()));
        }
        if (predicateType.returnType
            && isKnown(*predicateType.returnType)
            && !compatible(simpleType(StaticType::Bool), *predicateType.returnType)) {
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
    if (callback.type.kind != StaticType::Function || !hasFunctionSignature(callback.type)) {
        return CheckedExpression{simpleType(StaticType::Array)};
    }
    if (callback.type.parameterTypes.size() != 1) {
        throw TypeError(callToken, "flatMap expects callback with 1 argument");
    }
    const TypeInfo callbackType = specializeGenericCallback(
        callToken, callback.type, {elementType}, "flatMap");
    if (elementType.kind != StaticType::Unknown
        && !compatible(callbackType.parameterTypes.front(), elementType)) {
        throw TypeError(callToken,
            "flatMap callback expects " + typeInfoName(elementType)
                + ", got " + typeInfoName(callbackType.parameterTypes.front()));
    }
    if (callbackType.returnType && isKnown(*callbackType.returnType)) {
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
    if (predicate.type.kind == StaticType::Function && hasFunctionSignature(predicate.type)) {
        if (predicate.type.parameterTypes.size() != 1) {
            throw TypeError(callToken, functionName + " expects callback with 1 argument");
        }
        const TypeInfo predicateType = specializeGenericCallback(
            callToken, predicate.type, {elementType}, functionName);
        if (elementType.kind != StaticType::Unknown
            && !compatible(predicateType.parameterTypes.front(), elementType)) {
            throw TypeError(callToken,
                functionName + " callback expects " + typeInfoName(elementType)
                    + ", got " + typeInfoName(predicateType.parameterTypes.front()));
        }
        if (predicateType.returnType
            && isKnown(*predicateType.returnType)
            && !compatible(simpleType(StaticType::Bool), *predicateType.returnType)) {
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
        if (isNullable(*arrayTypeInfo.elementType)) {
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
    if (callback.type.kind == StaticType::Function && hasFunctionSignature(callback.type)) {
        if (callback.type.parameterTypes.size() != 2) {
            throw TypeError(callToken, "reduce expects callback with 2 arguments");
        }
        const TypeInfo callbackType = specializeGenericCallback(
            callToken, callback.type, {initial.type, elementType}, "reduce");
        if (initial.type.kind != StaticType::Unknown
            && !compatible(callbackType.parameterTypes.front(), initial.type)) {
            throw TypeError(callToken,
                "reduce callback accumulator expects " + typeInfoName(initial.type)
                    + ", got " + typeInfoName(callbackType.parameterTypes.front()));
        }
        if (elementType.kind != StaticType::Unknown
            && !compatible(callbackType.parameterTypes[1], elementType)) {
            throw TypeError(callToken,
                "reduce callback element expects " + typeInfoName(elementType)
                    + ", got " + typeInfoName(callbackType.parameterTypes[1]));
        }
        if (callbackType.returnType
            && isKnown(*callbackType.returnType)
            && !compatible(initial.type, *callbackType.returnType)) {
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
        if (strictElementCheck && expectedElement && !compatible(*expectedElement, valueArgument.type)) {
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
            && isKnown(keyArgument.type)
            && !mapKeyTypeAllowed(keyArgument.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !compatible(*expectedKey, keyArgument.type)) {
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
            && isKnown(keyArgument.type)
            && !mapKeyTypeAllowed(keyArgument.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !compatible(*expectedKey, keyArgument.type)) {
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
            substituteTypeParameters(parameter, receiverSubstitutions));
    }
    const TypeInfo returnType = substituteTypeParameters(
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
    resolvedNames_.recordMemberCallCallee(
        expression, method->resolvedName, true, method->declaration);
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
            resolvedNames_.recordMemberCallCallee(expression, found->second.resolvedName, false);
            return checkFunctionCall(
                expression.paren,
                found->second.type,
                expression.typeArguments,
                expression.arguments);
        }
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
        if (strictElementCheck && expectedElement && !compatible(*expectedElement, value.type)) {
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
            && isKnown(value.type)
            && !mapKeyTypeAllowed(value.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !compatible(*expectedKey, value.type)) {
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
            && isKnown(key.type)
            && !mapKeyTypeAllowed(key.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !compatible(*expectedKey, key.type)) {
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
        if (isKnown(receiver.type)
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
        return checkNativeStdlibCall(expression);
    }

    const CheckedExpression callee = checkExpressionInfo(*expression.callee);
    return checkFunctionCall(
        expression.paren,
        callee.type,
        expression.typeArguments,
        expression.arguments);
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
        if (isKnown(result.index) && !mapKeyTypeAllowed(result.index)) {
            throw TypeError(bracket, "map key must be nil, number, bool, or string");
        }
        if (result.collection.keyType && !compatible(*result.collection.keyType, result.index)) {
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

    if (target.collection.kind == StaticType::Array && target.collection.elementType) {
        return *target.collection.elementType;
    }
    if (target.collection.kind == StaticType::Map && target.collection.valueType) {
        return *target.collection.valueType;
    }
    if (target.collection.kind == StaticType::Range) {
        return simpleType(StaticType::Number);
    }
    return unknownType();
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
        if (target.collection.valueType && !compatible(*target.collection.valueType, value.type)) {
            throw TypeError(expression.bracket, "map value is incompatible with map value type");
        }
        return value;
    }

    const bool strictElementCheck = binding == nullptr || binding->explicitType;
    const TypeInfo* expectedElement = strictElementCheck ? target.collection.elementType.get() : nullptr;
    const CheckedExpression value = checkExpressionInfo(*expression.value, expectedElement);
    if (strictElementCheck && expectedElement && !compatible(*expectedElement, value.type)) {
        throw TypeError(expression.bracket,
            "array index assignment expects " + typeInfoName(*expectedElement)
                + ", got " + typeInfoName(value.type));
    }

    if (binding && binding->type.kind == StaticType::Array) {
        refineArrayBindingFromMutation(*binding, value.type);
        return CheckedExpression{value.type};
    }

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

    return CheckedExpression{simpleType(StaticType::Number)};
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
                "struct `" + *object.structName + "` has no field `" + name.lexeme + "`");
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
        if (!compatible(*structField, value.type)) {
            throw TypeError(expression.name,
                "field `" + expression.name.lexeme + "` expects " + typeInfoName(*structField)
                    + ", got " + typeInfoName(value.type));
        }
        return CheckedExpression{*structField};
    }

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

    return CheckedExpression{simpleType(StaticType::Number)};
}

void TypeChecker::checkKnownNumber(const Token& token, const TypeInfo& type, const std::string& messagePrefix) const
{
    if (type.kind != StaticType::Unknown && type.kind != StaticType::Number) {
        throw TypeError(token, messagePrefix + typeInfoName(type));
    }
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
    if (!compatible(expected, actual)) {
        throw TypeError(token, context);
    }
}

TypeInfo TypeChecker::checkUnary(const UnaryExpr& expression)
{
    const TypeInfo right = checkExpression(*expression.right);
    switch (expression.op.type) {
    case TokenType::Minus:
        if (isKnown(right) && right.kind != StaticType::Number) {
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

    switch (expression.op.type) {
    case TokenType::Plus:
        if (!isKnown(left) || !isKnown(right)) {
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
        if (!isKnown(left) || !isKnown(right)) {
            return simpleType(StaticType::Number);
        }
        if (left.kind != StaticType::Number || right.kind != StaticType::Number) {
            throw TypeError(expression.op, binaryTypesMessage(expression, left, right));
        }
        return simpleType(StaticType::Number);
    case TokenType::Greater:
    case TokenType::GreaterEqual:
    case TokenType::Less:
    case TokenType::LessEqual:
        if (!isKnown(left) || !isKnown(right)) {
            return simpleType(StaticType::Bool);
        }
        if (left.kind != StaticType::Number || right.kind != StaticType::Number) {
            throw TypeError(expression.op, binaryTypesMessage(expression, left, right));
        }
        return simpleType(StaticType::Bool);
    case TokenType::EqualEqual:
    case TokenType::BangEqual:
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
