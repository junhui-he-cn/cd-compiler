#include "TypeChecker.hpp"

#include "TypeCheckerInternal.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

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
    if (typeName.kind == TypeAnnotation::Kind::Nullable
        || typeName.kind == TypeAnnotation::Kind::Optional) {
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

TypeInfo TypeChecker::resolveSimpleStructFieldAnnotation(const TypeAnnotation& typeName, const Token&)
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

TypeChecker::MethodInfo TypeChecker::operatorInfoFromSignature(const OperatorSignature& signature) const
{
    MethodInfo info;
    info.isOperator = true;
    info.receiverType = signature.receiverType;
    info.parameterTypes = {signature.rightParameterType};
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

OperatorSignature TypeChecker::qualifyNamespaceOperatorSignature(
    const OperatorSignature& signature,
    const std::string& alias,
    const ModuleStructExports& structs,
    const ModuleEnumExports& enums) const
{
    OperatorSignature result = signature;
    result.receiverType = qualifyNamespaceType(result.receiverType, alias, structs, enums);
    result.rightParameterType = qualifyNamespaceType(result.rightParameterType, alias, structs, enums);
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

void TypeChecker::importOperatorExports(
    const Token& diagnosticToken,
    const ModuleOperatorExports& operatorExports,
    const std::string* namespaceAlias,
    const ModuleStructExports* namespaceStructs,
    const ModuleEnumExports* namespaceEnums)
{
    for (const auto& structEntry : operatorExports) {
        std::string structName = structEntry.first;
        if (namespaceAlias) {
            structName = *namespaceAlias + "." + structName;
        }

        auto& table = methods_[structName];
        for (const auto& operatorEntry : structEntry.second) {
            OperatorSignature signature = operatorEntry.second;
            if (namespaceAlias && namespaceStructs && namespaceEnums) {
                signature = qualifyNamespaceOperatorSignature(
                    signature, *namespaceAlias, *namespaceStructs, *namespaceEnums);
            }
            if (table.find(operatorEntry.first) != table.end()) {
                throw TypeError(diagnosticToken,
                    "duplicate operator `" + operatorEntry.first
                        + "` for struct `" + structName + "`");
            }
            table.emplace(operatorEntry.first, operatorInfoFromSignature(signature));
        }
    }
}

void TypeChecker::recordStructMethodExports(std::size_t moduleId, const std::string& structName)
{
    const auto methods = methods_.find(structName);
    if (methods == methods_.end()) {
        return;
    }
    const StructTypeDecl* structType = findStructType(structName);
    for (const auto& method : methods->second) {
        if (method.second.declaration && method.second.declaration->isOperator) {
            if (!structType || method.second.parameterTypes.size() != 1) {
                continue;
            }
            moduleSymbols_.recordOperatorExport(
                moduleId,
                structName,
                method.first,
                OperatorSignature{
                    method.second.receiverType,
                    method.second.parameterTypes.front(),
                    method.second.returnType,
                    structType->genericParameters,
                    structType->genericParameterConstraints,
                    method.second.resolvedName});
            continue;
        }
        moduleSymbols_.recordMethodExport(moduleId, structName, method.first, methodSignatureFromInfo(method.second));
    }
}

void TypeChecker::checkMethodNameAvailable(const StructTypeDecl& structType, const ImplStmt& statement, const MethodDecl& method) const
{
    if (findMethod(statement.typeName.lexeme, method.name.lexeme)) {
        if (method.isOperator) {
            throw TypeError(method.name,
                "duplicate operator `" + method.name.lexeme
                    + "` for struct `" + statement.typeName.lexeme + "`");
        }
        throw TypeError(method.name, "duplicate method `" + method.name.lexeme + "` for struct `" + statement.typeName.lexeme + "`");
    }
    if (!method.isOperator && findStructField(structType, method.name.lexeme)) {
        throw TypeError(method.name,
            "method `" + method.name.lexeme + "` conflicts with field `" + method.name.lexeme + "` on struct `" + statement.typeName.lexeme + "`");
    }
}

void TypeChecker::registerMethodSignature(const StructTypeDecl& structType, const ImplStmt& statement, const MethodDecl& method)
{
    checkMethodNameAvailable(structType, statement, method);

    if (method.isOperator && !method.typeParameters.empty()) {
        throw TypeError(method.name,
            "operator `" + method.name.lexeme + "` for struct `"
                + statement.typeName.lexeme + "` cannot declare type parameters");
    }

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

    std::vector<TypeInfo> receiverTypeArguments;
    receiverTypeArguments.reserve(structType.genericParameters.size());
    for (const std::string& receiverParameter : structType.genericParameters) {
        const TypeInfo* type = findTypeParameter(receiverParameter);
        receiverTypeArguments.push_back(type ? *type : typeParameterType(receiverParameter));
    }
    TypeInfo receiverType = namedStructType(
        statement.typeName.lexeme, std::move(receiverTypeArguments));

    if (method.isOperator) {
        if (method.parameters.size() != 1
            || parameterTypes.size() != 1
            || !method.parameters.front().typeName
            || !SemanticTypes::compatible(receiverType, parameterTypes.front())
            || !SemanticTypes::compatible(parameterTypes.front(), receiverType)) {
            throw TypeError(method.name,
                "operator `" + method.name.lexeme + "` for struct `"
                    + statement.typeName.lexeme
                    + "` must accept exactly one right operand of type `"
                    + typeInfoName(receiverType) + "`");
        }
        if (!expectedReturnType || expectedReturnType->kind != StaticType::Bool) {
            throw TypeError(method.name,
                "operator `" + method.name.lexeme + "` for struct `"
                    + statement.typeName.lexeme + "` must return bool");
        }
    }

    MethodInfo info;
    info.declaration = &method;
    info.isOperator = method.isOperator;
    info.receiverType = std::move(receiverType);
    info.parameterTypes = std::move(parameterTypes);
    info.returnType = expectedReturnType ? *expectedReturnType : unknownType();
    const std::string methodLabel = method.isOperator
        ? "operator_" + tokenTypeName(method.name.type)
        : method.name.lexeme;
    info.resolvedName = makeResolvedName(
        "__method_" + statement.typeName.lexeme + "_" + methodLabel);
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
    std::vector<BindingId> parameterBindingIds;
    parameterBindingIds.reserve(declaration.parameters.size() + 1);
    Token thisToken{TokenType::Identifier, "this", declaration.name.line, declaration.name.column};
    Binding thisBinding = declareVariable(thisToken, method.receiverType, true);
    parameterNames.push_back(thisBinding.resolvedName);
    parameterBindingIds.push_back(thisBinding.bindingId);

    for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
        const Parameter& parameter = declaration.parameters[i];
        Binding parameterBinding = declareVariable(parameter.name, method.parameterTypes[i], parameter.typeName.has_value());
        parameterNames.push_back(parameterBinding.resolvedName);
        parameterBindingIds.push_back(parameterBinding.bindingId);
    }
    declarationIndex_.recordFunctionMetadata(
        declaration,
        FunctionMetadataRecord{
            method.resolvedName,
            declaration.name.lexeme,
            parameterNames,
            BindingId{},
            std::move(parameterBindingIds)});

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
            if (statement.typeParameters[i].constraints.empty()) {
                continue;
            }
            const TypeInfo headerConstraint = resolveTypeParameterConstraints(statement.typeParameters[i]);
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
        if (method.isOperator
            && !moduleStack_.empty()
            && !moduleSymbols_.isLocalStruct(
                moduleStack_.back(), statement.typeName.lexeme)) {
            throw TypeError(method.name,
                "cannot implement operator `" + method.name.lexeme
                    + "` for imported struct `" + statement.typeName.lexeme + "`");
        }
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
