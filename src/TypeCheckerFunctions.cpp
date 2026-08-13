#include "TypeChecker.hpp"

#include "NativeStdlib.hpp"
#include "TypeCheckerInternal.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

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
        statement.returnTypeName.has_value(),
        declarationIndex_.declaration(statement));

    beginScope();
    ++functionDepth_;
    const std::size_t enclosingLoopDepth = loopDepth_;
    loopDepth_ = 0;

    std::vector<std::string> parameterNames;
    std::vector<BindingId> parameterBindingIds;
    parameterBindingIds.reserve(statement.parameters.size());
    for (std::size_t i = 0; i < statement.parameters.size(); ++i) {
        const Parameter& parameter = statement.parameters[i];
        Binding parameterBinding = declareVariable(
            parameter.name,
            declaredParameterTypes[i],
            parameter.typeName.has_value(),
            declarationIndex_.declaration(parameter));
        parameterNames.push_back(parameterBinding.resolvedName);
        parameterBindingIds.push_back(parameterBinding.bindingId);
    }
    declarationIndex_.recordFunctionMetadata(
        statement,
        FunctionMetadataRecord{
            functionBinding.resolvedName,
            statement.name.lexeme,
            parameterNames,
            functionBinding.bindingId,
            std::move(parameterBindingIds)});

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

    Binding* storedFunction = nullptr;
    if (const DeclarationRecord* record = declarationIndex_.declaration(statement)) {
        storedFunction = bindingById(record->declarationId);
    }
    if (!storedFunction) {
        storedFunction = findVariable(statement.name.lexeme);
    }
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
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (index >= constraints.size() || !constraints[index]) {
            continue;
        }
        const auto found = substitutions.find(parameters[index]);
        if (found == substitutions.end()) {
            continue;
        }
        if (!satisfiesCapabilityWitness(found->second, *constraints[index])) {
            const std::string prefix = context.empty() ? "" : context + ": ";
            throw TypeError(callToken,
                prefix + "type parameter " + parameters[index] + " must satisfy "
                    + typeInfoName(*constraints[index]) + ", got "
                    + typeInfoName(found->second));
        }
    }
}

bool TypeChecker::satisfiesCapabilityWitness(
    const TypeInfo& actual,
    const TypeInfo& capability) const
{
    if (!SemanticTypes::isKnown(actual)) {
        return true;
    }
    if (capability.kind != StaticType::Capability || !capability.structName) {
        return SemanticTypes::compatible(capability, actual);
    }
    if (SemanticTypes::isCapabilitySet(capability)) {
        return std::all_of(
            capability.typeArguments.begin(),
            capability.typeArguments.end(),
            [this, &actual](const TypeInfo& requirement) {
                return satisfiesCapabilityWitness(actual, requirement);
            });
    }

    if (actual.kind == StaticType::TypeParameter) {
        return actual.typeParameterConstraint
            && satisfiesCapabilityWitness(*actual.typeParameterConstraint, capability);
    }
    if (actual.kind == StaticType::Capability) {
        return SemanticTypes::satisfiesCapability(actual, capability);
    }

    const std::string& name = *capability.structName;
    if (name == "Ord") {
        return actual.kind == StaticType::Number
            || actual.kind == StaticType::String;
    }
    return SemanticTypes::satisfiesCapability(actual, capability);
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
    std::vector<BindingId> parameterBindingIds;
    parameterBindingIds.reserve(expression.parameters.size());
    for (std::size_t i = 0; i < expression.parameters.size(); ++i) {
        const Parameter& parameter = expression.parameters[i];
        Binding parameterBinding = declareVariable(
            parameter.name,
            declaredParameterTypes[i],
            parameter.typeName.has_value() || contextualSignature != nullptr,
            declarationIndex_.declaration(parameter));
        parameterNames.push_back(parameterBinding.resolvedName);
        parameterBindingIds.push_back(parameterBinding.bindingId);
    }
    declarationIndex_.recordFunctionMetadata(
        expression,
        FunctionMetadataRecord{
            "<lambda>",
            "<lambda>",
            parameterNames,
            BindingId{},
            std::move(parameterBindingIds)});

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
    const MemberCallMetadataRecord* metadata = declarationIndex_.memberCallMetadata(expression);
    if (!metadata || !metadata->passesReceiver) {
        return;
    }

    flowFacts_.invalidateAll();
}

void TypeChecker::invalidateCapturedSymbols(const CaptureRecord& captures)
{
    for (const ResolvedSymbol& symbol : captures.symbols) {
        if (const Binding* binding = bindingById(symbol.declarationId)) {
            flowFacts_.invalidate(binding->resolvedName);
        }
    }
}
