#include "TypeUtils.hpp"

#include <cstddef>
#include <optional>
#include <utility>

TypeInfo unknownType()
{
    return TypeInfo{};
}

TypeInfo simpleType(StaticType kind)
{
    TypeInfo result;
    result.kind = kind;
    return result;
}

TypeInfo namedStructType(std::string name, std::vector<TypeInfo> typeArguments)
{
    TypeInfo result;
    result.kind = StaticType::Struct;
    result.structName = std::move(name);
    result.typeArguments = std::move(typeArguments);
    return result;
}

TypeInfo namedEnumType(std::string name, std::vector<TypeInfo> typeArguments)
{
    TypeInfo result;
    result.kind = StaticType::Enum;
    result.enumName = std::move(name);
    result.typeArguments = std::move(typeArguments);
    return result;
}

TypeInfo arrayType(TypeInfo elementType)
{
    TypeInfo result;
    result.kind = StaticType::Array;
    result.elementType = std::make_shared<TypeInfo>(std::move(elementType));
    return result;
}

TypeInfo mapType(TypeInfo keyType, TypeInfo valueType)
{
    TypeInfo result;
    result.kind = StaticType::Map;
    result.keyType = std::make_shared<TypeInfo>(std::move(keyType));
    result.valueType = std::make_shared<TypeInfo>(std::move(valueType));
    return result;
}

TypeInfo typeParameterType(std::string name, std::optional<TypeInfo> constraint)
{
    TypeInfo result;
    result.kind = StaticType::TypeParameter;
    result.typeParameterName = std::move(name);
    if (constraint) {
        result.typeParameterConstraint = std::make_shared<TypeInfo>(std::move(*constraint));
    }
    return result;
}

TypeInfo functionType(
    std::vector<TypeInfo> parameterTypes,
    TypeInfo returnType,
    std::vector<std::string> genericParameters,
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints)
{
    TypeInfo result;
    result.kind = StaticType::Function;
    result.parameterTypes = std::move(parameterTypes);
    result.returnType = std::make_shared<TypeInfo>(std::move(returnType));
    result.genericParameters = std::move(genericParameters);
    result.genericParameterConstraints = std::move(genericParameterConstraints);
    return result;
}

TypeInfo nullableType(TypeInfo innerType)
{
    TypeInfo result;
    result.kind = StaticType::Nullable;
    result.nullableOf = std::make_shared<TypeInfo>(std::move(innerType));
    return result;
}

TypeInfo functionWithoutSignature()
{
    TypeInfo result;
    result.kind = StaticType::Function;
    return result;
}

bool isKnown(const TypeInfo& type)
{
    return type.kind != StaticType::Unknown;
}

bool hasFunctionSignature(const TypeInfo& type)
{
    return type.kind == StaticType::Function && type.returnType != nullptr;
}

bool isNullable(const TypeInfo& type)
{
    return type.kind == StaticType::Nullable && type.nullableOf != nullptr;
}

std::string staticTypeName(StaticType type)
{
    switch (type) {
    case StaticType::Unknown:
        return "unknown";
    case StaticType::Nil:
        return "nil";
    case StaticType::Number:
        return "number";
    case StaticType::Bool:
        return "bool";
    case StaticType::String:
        return "string";
    case StaticType::Function:
        return "function";
    case StaticType::Array:
        return "array";
    case StaticType::Map:
        return "map";
    case StaticType::Range:
        return "range";
    case StaticType::Struct:
        return "struct";
    case StaticType::Enum:
        return "enum";
    case StaticType::Nullable:
        return "nullable";
    case StaticType::TypeParameter:
        return "type parameter";
    }

    return "unknown";
}

std::string typeInfoName(const TypeInfo& type)
{
    if (isNullable(type)) {
        return typeInfoName(*type.nullableOf) + "?";
    }

    if (type.kind == StaticType::Struct && type.structName) {
        std::string result = *type.structName;
        if (!type.typeArguments.empty()) {
            result += '<';
            for (std::size_t i = 0; i < type.typeArguments.size(); ++i) {
                if (i != 0) {
                    result += ", ";
                }
                result += typeInfoName(type.typeArguments[i]);
            }
            result += '>';
        }
        return result;
    }

    if (type.kind == StaticType::Enum && type.enumName) {
        std::string result = *type.enumName;
        if (!type.typeArguments.empty()) {
            result += '<';
            for (std::size_t i = 0; i < type.typeArguments.size(); ++i) {
                if (i != 0) {
                    result += ", ";
                }
                result += typeInfoName(type.typeArguments[i]);
            }
            result += '>';
        }
        return result;
    }

    if (type.kind == StaticType::Array && type.elementType) {
        return "[" + typeInfoName(*type.elementType) + "]";
    }

    if (type.kind == StaticType::Map && type.keyType && type.valueType) {
        return "map<" + typeInfoName(*type.keyType) + ", " + typeInfoName(*type.valueType) + ">";
    }

    if (type.kind == StaticType::TypeParameter && type.typeParameterName) {
        return *type.typeParameterName;
    }

    if (type.kind != StaticType::Function || !type.returnType) {
        return staticTypeName(type.kind);
    }

    std::string result = "fun";
    if (!type.genericParameters.empty()) {
        result += '<';
        for (std::size_t i = 0; i < type.genericParameters.size(); ++i) {
            if (i != 0) {
                result += ", ";
            }
            result += type.genericParameters[i];
            if (i < type.genericParameterConstraints.size()
                && type.genericParameterConstraints[i]) {
                result += ": ";
                result += typeInfoName(*type.genericParameterConstraints[i]);
            }
        }
        result += '>';
    }
    result += '(';
    for (std::size_t i = 0; i < type.parameterTypes.size(); ++i) {
        if (i != 0) {
            result += ", ";
        }
        result += typeInfoName(type.parameterTypes[i]);
    }
    result += "): ";
    result += typeInfoName(*type.returnType);
    return result;
}

namespace SemanticTypes {

bool compatible(const TypeInfo& expected, const TypeInfo& actual)
{
    if (!isKnown(expected) || !isKnown(actual)) {
        return true;
    }
    if (isNullable(expected)) {
        if (actual.kind == StaticType::Nil) {
            return true;
        }
        if (isNullable(actual)) {
            return SemanticTypes::compatible(*expected.nullableOf, *actual.nullableOf);
        }
        return SemanticTypes::compatible(*expected.nullableOf, actual);
    }
    if (isNullable(actual)) {
        return false;
    }
    if (expected.kind == StaticType::TypeParameter || actual.kind == StaticType::TypeParameter) {
        if (expected.kind == StaticType::TypeParameter && actual.kind == StaticType::TypeParameter) {
            return expected.typeParameterName == actual.typeParameterName;
        }
        if (actual.kind == StaticType::TypeParameter && actual.typeParameterConstraint) {
            return SemanticTypes::compatible(expected, *actual.typeParameterConstraint);
        }
        return false;
    }
    if (expected.kind != actual.kind) {
        return false;
    }
    if (expected.kind == StaticType::Struct) {
        if (expected.structName || actual.structName) {
            if (expected.structName != actual.structName
                || expected.typeArguments.size() != actual.typeArguments.size()) {
                return false;
            }
            for (std::size_t i = 0; i < expected.typeArguments.size(); ++i) {
                if (!SemanticTypes::compatible(expected.typeArguments[i], actual.typeArguments[i])
                    || !SemanticTypes::compatible(actual.typeArguments[i], expected.typeArguments[i])) {
                    return false;
                }
            }
            return true;
        }
        return true;
    }
    if (expected.kind == StaticType::Enum) {
        if (expected.enumName || actual.enumName) {
            if (expected.enumName != actual.enumName
                || expected.typeArguments.size() != actual.typeArguments.size()) {
                return false;
            }
            for (std::size_t i = 0; i < expected.typeArguments.size(); ++i) {
                if (!SemanticTypes::compatible(expected.typeArguments[i], actual.typeArguments[i])
                    || !SemanticTypes::compatible(actual.typeArguments[i], expected.typeArguments[i])) {
                    return false;
                }
            }
            return true;
        }
        return true;
    }
    if (expected.kind == StaticType::Array) {
        if (!expected.elementType || !actual.elementType) {
            return true;
        }
        return SemanticTypes::compatible(*expected.elementType, *actual.elementType);
    }
    if (expected.kind == StaticType::Map) {
        if (!expected.keyType || !actual.keyType || !expected.valueType || !actual.valueType) {
            return true;
        }
        return SemanticTypes::compatible(*expected.keyType, *actual.keyType)
            && SemanticTypes::compatible(*expected.valueType, *actual.valueType);
    }
    if (expected.kind != StaticType::Function) {
        return true;
    }
    if (!hasFunctionSignature(expected) || !hasFunctionSignature(actual)) {
        return true;
    }
    if (expected.parameterTypes.size() != actual.parameterTypes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.parameterTypes.size(); ++i) {
        if (!SemanticTypes::compatible(expected.parameterTypes[i], actual.parameterTypes[i])) {
            return false;
        }
    }
    return SemanticTypes::compatible(*expected.returnType, *actual.returnType);
}

std::optional<TypeInfo> mergeArrayElementTypes(const TypeInfo& left, const TypeInfo& right)
{
    if (!isKnown(left) || !isKnown(right)) {
        return std::nullopt;
    }

    if (left.kind == StaticType::Nil && right.kind == StaticType::Nil) {
        return simpleType(StaticType::Nil);
    }

    if (isNullable(left)) {
        if (right.kind == StaticType::Nil) {
            return left;
        }
        if (isNullable(right)) {
            std::optional<TypeInfo> inner = SemanticTypes::mergeArrayElementTypes(*left.nullableOf, *right.nullableOf);
            if (!inner) {
                return std::nullopt;
            }
            return nullableType(std::move(*inner));
        }
        std::optional<TypeInfo> inner = SemanticTypes::mergeArrayElementTypes(*left.nullableOf, right);
        if (!inner) {
            return std::nullopt;
        }
        return nullableType(std::move(*inner));
    }

    if (isNullable(right)) {
        if (left.kind == StaticType::Nil) {
            return right;
        }
        std::optional<TypeInfo> inner = SemanticTypes::mergeArrayElementTypes(left, *right.nullableOf);
        if (!inner) {
            return std::nullopt;
        }
        return nullableType(std::move(*inner));
    }

    if (left.kind == StaticType::Nil) {
        return nullableType(right);
    }
    if (right.kind == StaticType::Nil) {
        return nullableType(left);
    }

    if (left.kind == StaticType::Array && right.kind == StaticType::Array) {
        if (!left.elementType || !right.elementType) {
            return simpleType(StaticType::Array);
        }
        std::optional<TypeInfo> element = SemanticTypes::mergeArrayElementTypes(*left.elementType, *right.elementType);
        if (!element) {
            return std::nullopt;
        }
        return arrayType(std::move(*element));
    }

    if (SemanticTypes::compatible(left, right) && SemanticTypes::compatible(right, left)) {
        return left;
    }

    return std::nullopt;
}

TypeInfo substituteTypeParameters(
    const TypeInfo& type,
    const TypeSubstitutions& substitutions)
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
            SemanticTypes::substituteTypeParameters(*type.elementType, substitutions));
    }
    if (type.keyType) {
        result.keyType = std::make_shared<TypeInfo>(
            SemanticTypes::substituteTypeParameters(*type.keyType, substitutions));
    }
    if (type.valueType) {
        result.valueType = std::make_shared<TypeInfo>(
            SemanticTypes::substituteTypeParameters(*type.valueType, substitutions));
    }
    if (type.nullableOf) {
        result.nullableOf = std::make_shared<TypeInfo>(
            SemanticTypes::substituteTypeParameters(*type.nullableOf, substitutions));
    }
    if (type.returnType) {
        result.returnType = std::make_shared<TypeInfo>(
            SemanticTypes::substituteTypeParameters(*type.returnType, substitutions));
    }
    for (TypeInfo& parameter : result.parameterTypes) {
        parameter = SemanticTypes::substituteTypeParameters(parameter, substitutions);
    }
    for (TypeInfo& argument : result.typeArguments) {
        argument = SemanticTypes::substituteTypeParameters(argument, substitutions);
    }
    if (type.typeParameterConstraint) {
        result.typeParameterConstraint = std::make_shared<TypeInfo>(
            SemanticTypes::substituteTypeParameters(*type.typeParameterConstraint, substitutions));
    }
    for (std::shared_ptr<TypeInfo>& constraint : result.genericParameterConstraints) {
        if (constraint) {
            constraint = std::make_shared<TypeInfo>(
                SemanticTypes::substituteTypeParameters(*constraint, substitutions));
        }
    }
    return result;
}

} // namespace SemanticTypes

bool compatible(const TypeInfo& expected, const TypeInfo& actual)
{
    return SemanticTypes::compatible(expected, actual);
}

std::optional<TypeInfo> mergeArrayElementTypes(const TypeInfo& left, const TypeInfo& right)
{
    return SemanticTypes::mergeArrayElementTypes(left, right);
}

TypeInfo substituteTypeParameters(const TypeInfo& type, const TypeSubstitutions& substitutions)
{
    return SemanticTypes::substituteTypeParameters(type, substitutions);
}
