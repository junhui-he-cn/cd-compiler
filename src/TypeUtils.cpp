#include "TypeUtils.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

constexpr const char* kCapabilitySetName = "<all-capabilities>";

} // namespace

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

TypeInfo capabilityType(std::string name)
{
    TypeInfo result;
    result.kind = StaticType::Capability;
    // Reuse the existing named-type slot so cdi 0.1 sidecars keep their
    // established TypeInfo encoding while gaining a new kind value.
    result.structName = std::move(name);
    return result;
}

TypeInfo capabilitySetType(std::vector<TypeInfo> capabilities)
{
    for (const TypeInfo& capability : capabilities) {
        if (capability.kind != StaticType::Capability
            || !capability.structName
            || *capability.structName == kCapabilitySetName) {
            throw std::invalid_argument("capability sets may contain only named capabilities");
        }
    }
    if (capabilities.empty()) {
        throw std::invalid_argument("capability sets may not be empty");
    }

    std::sort(
        capabilities.begin(),
        capabilities.end(),
        [](const TypeInfo& left, const TypeInfo& right) {
            return *left.structName < *right.structName;
        });
    capabilities.erase(
        std::unique(
            capabilities.begin(),
            capabilities.end(),
            [](const TypeInfo& left, const TypeInfo& right) {
                return left.structName == right.structName;
            }),
        capabilities.end());

    TypeInfo result;
    result.kind = StaticType::Capability;
    result.structName = kCapabilitySetName;
    result.typeArguments = std::move(capabilities);
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

namespace SemanticTypes {

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

bool isCapability(const TypeInfo& type, const std::string& name)
{
    if (type.kind != StaticType::Capability || !type.structName) {
        return false;
    }
    if (*type.structName == kCapabilitySetName) {
        return std::any_of(
            type.typeArguments.begin(),
            type.typeArguments.end(),
            [&name](const TypeInfo& capability) {
                return SemanticTypes::isCapability(capability, name);
            });
    }
    return *type.structName == name;
}

bool isCapabilitySet(const TypeInfo& type)
{
    return type.kind == StaticType::Capability
        && type.structName
        && *type.structName == kCapabilitySetName;
}

bool satisfiesCapability(const TypeInfo& actual, const TypeInfo& capability)
{
    if (!SemanticTypes::isKnown(actual)) {
        return true;
    }
    if (capability.kind != StaticType::Capability || !capability.structName) {
        return false;
    }

    if (SemanticTypes::isCapabilitySet(capability)) {
        return std::all_of(
            capability.typeArguments.begin(),
            capability.typeArguments.end(),
            [&actual](const TypeInfo& requirement) {
                return SemanticTypes::satisfiesCapability(actual, requirement);
            });
    }

    const std::string& name = *capability.structName;
    if (actual.kind == StaticType::TypeParameter) {
        if (!actual.typeParameterConstraint) {
            return false;
        }
        return SemanticTypes::satisfiesCapability(*actual.typeParameterConstraint, capability);
    }
    if (actual.kind == StaticType::Capability) {
        if (SemanticTypes::isCapabilitySet(actual)) {
            return std::any_of(
                actual.typeArguments.begin(),
                actual.typeArguments.end(),
                [&capability](const TypeInfo& provided) {
                    return SemanticTypes::satisfiesCapability(provided, capability);
                });
        }
        return name == "Eq"
            ? SemanticTypes::isCapability(actual, "Eq")
                || SemanticTypes::isCapability(actual, "Ord")
            : SemanticTypes::isCapability(actual, name);
    }
    if (name == "Ord") {
        return actual.kind == StaticType::Number
            || actual.kind == StaticType::String;
    }
    if (name == "Eq") {
        if (actual.kind == StaticType::Nullable) {
            return actual.nullableOf
                && SemanticTypes::satisfiesCapability(*actual.nullableOf, capability);
        }
        return actual.kind != StaticType::Unknown;
    }
    if (name == "Hash") {
        if (actual.kind == StaticType::Nullable) {
            return actual.nullableOf
                && SemanticTypes::satisfiesCapability(*actual.nullableOf, capability);
        }
        switch (actual.kind) {
        case StaticType::Nil:
        case StaticType::Number:
        case StaticType::Bool:
        case StaticType::String:
        case StaticType::Function:
        case StaticType::Array:
        case StaticType::Map:
        case StaticType::Range:
        case StaticType::Struct:
        case StaticType::Enum:
            return true;
        default:
            return false;
        }
    }
    return false;
}

} // namespace SemanticTypes

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
    case StaticType::Capability:
        return "capability";
    }

    return "unknown";
}

std::string typeInfoName(const TypeInfo& type)
{
    if (SemanticTypes::isNullable(type)) {
        return "optional<" + typeInfoName(*type.nullableOf) + ">";
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

    if (type.kind == StaticType::Capability && type.structName) {
        if (*type.structName == kCapabilitySetName) {
            std::string result;
            for (std::size_t i = 0; i < type.typeArguments.size(); ++i) {
                if (i != 0) {
                    result += " + ";
                }
                result += typeInfoName(type.typeArguments[i]);
            }
            return result;
        }
        return *type.structName;
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
    if (!SemanticTypes::isKnown(expected) || !SemanticTypes::isKnown(actual)) {
        return true;
    }
    if (SemanticTypes::isNullable(expected)) {
        if (actual.kind == StaticType::Nil) {
            return true;
        }
        if (SemanticTypes::isNullable(actual)) {
            return SemanticTypes::compatible(*expected.nullableOf, *actual.nullableOf);
        }
        return SemanticTypes::compatible(*expected.nullableOf, actual);
    }
    if (SemanticTypes::isNullable(actual)) {
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
    if (expected.kind == StaticType::Capability || actual.kind == StaticType::Capability) {
        if (expected.kind != StaticType::Capability
            || actual.kind != StaticType::Capability
            || expected.structName != actual.structName
            || expected.typeArguments.size() != actual.typeArguments.size()) {
            return false;
        }
        for (std::size_t i = 0; i < expected.typeArguments.size(); ++i) {
            if (!SemanticTypes::compatible(expected.typeArguments[i], actual.typeArguments[i])) {
                return false;
            }
        }
        return true;
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
    if (!SemanticTypes::hasFunctionSignature(expected) || !SemanticTypes::hasFunctionSignature(actual)) {
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

std::optional<TypeInferenceConflict> inferTypeArguments(
    const TypeInfo& expected,
    const TypeInfo& actual,
    TypeSubstitutions& substitutions)
{
    if (expected.kind == StaticType::TypeParameter && expected.typeParameterName) {
        if (!SemanticTypes::isKnown(actual)) {
            return std::nullopt;
        }
        const auto [it, inserted] = substitutions.emplace(*expected.typeParameterName, actual);
        if (!inserted
            && (!SemanticTypes::compatible(it->second, actual)
                || !SemanticTypes::compatible(actual, it->second))) {
            return TypeInferenceConflict{*expected.typeParameterName, it->second, actual};
        }
        return std::nullopt;
    }

    if (expected.kind == StaticType::Array && actual.kind == StaticType::Array
        && expected.elementType && actual.elementType) {
        return SemanticTypes::inferTypeArguments(
            *expected.elementType, *actual.elementType, substitutions);
    }

    if (expected.kind == StaticType::Map && actual.kind == StaticType::Map
        && expected.keyType && actual.keyType && expected.valueType && actual.valueType) {
        if (std::optional<TypeInferenceConflict> conflict = SemanticTypes::inferTypeArguments(
                *expected.keyType, *actual.keyType, substitutions)) {
            return conflict;
        }
        return SemanticTypes::inferTypeArguments(
            *expected.valueType, *actual.valueType, substitutions);
    }

    if (expected.kind == StaticType::Struct && actual.kind == StaticType::Struct
        && expected.structName && actual.structName
        && expected.structName == actual.structName
        && expected.typeArguments.size() == actual.typeArguments.size()) {
        for (std::size_t i = 0; i < expected.typeArguments.size(); ++i) {
            if (std::optional<TypeInferenceConflict> conflict = SemanticTypes::inferTypeArguments(
                    expected.typeArguments[i], actual.typeArguments[i], substitutions)) {
                return conflict;
            }
        }
        return std::nullopt;
    }

    if (expected.kind == StaticType::Nullable && expected.nullableOf) {
        if (actual.kind == StaticType::Nil) {
            return std::nullopt;
        }
        if (actual.kind == StaticType::Nullable && actual.nullableOf) {
            return SemanticTypes::inferTypeArguments(
                *expected.nullableOf, *actual.nullableOf, substitutions);
        }
        return SemanticTypes::inferTypeArguments(*expected.nullableOf, actual, substitutions);
    }

    if (expected.kind == StaticType::Enum && actual.kind == StaticType::Enum
        && expected.enumName && actual.enumName
        && expected.enumName == actual.enumName
        && expected.typeArguments.size() == actual.typeArguments.size()) {
        for (std::size_t i = 0; i < expected.typeArguments.size(); ++i) {
            if (std::optional<TypeInferenceConflict> conflict = SemanticTypes::inferTypeArguments(
                    expected.typeArguments[i], actual.typeArguments[i], substitutions)) {
                return conflict;
            }
        }
        return std::nullopt;
    }

    if (expected.kind == StaticType::Function && actual.kind == StaticType::Function
        && SemanticTypes::hasFunctionSignature(expected)
        && SemanticTypes::hasFunctionSignature(actual)
        && expected.parameterTypes.size() == actual.parameterTypes.size()) {
        for (std::size_t i = 0; i < expected.parameterTypes.size(); ++i) {
            if (std::optional<TypeInferenceConflict> conflict = SemanticTypes::inferTypeArguments(
                    expected.parameterTypes[i], actual.parameterTypes[i], substitutions)) {
                return conflict;
            }
        }
        return SemanticTypes::inferTypeArguments(
            *expected.returnType, *actual.returnType, substitutions);
    }

    return std::nullopt;
}

std::optional<TypeConstraintViolation> validateTypeParameterConstraints(
    const std::vector<std::string>& parameters,
    const std::vector<std::shared_ptr<TypeInfo>>& constraints,
    const TypeSubstitutions& substitutions)
{
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i >= constraints.size() || !constraints[i]) {
            continue;
        }
        const auto found = substitutions.find(parameters[i]);
        if (found == substitutions.end()) {
            continue;
        }
        const bool satisfies = constraints[i]->kind == StaticType::Capability
            ? SemanticTypes::satisfiesCapability(found->second, *constraints[i])
            : SemanticTypes::compatible(*constraints[i], found->second);
        if (!satisfies) {
            return TypeConstraintViolation{parameters[i], *constraints[i], found->second};
        }
    }
    return std::nullopt;
}

std::optional<TypeInfo> mergeArrayElementTypes(const TypeInfo& left, const TypeInfo& right)
{
    if (!SemanticTypes::isKnown(left) || !SemanticTypes::isKnown(right)) {
        return std::nullopt;
    }

    if (left.kind == StaticType::Nil && right.kind == StaticType::Nil) {
        return simpleType(StaticType::Nil);
    }

    if (SemanticTypes::isNullable(left)) {
        if (right.kind == StaticType::Nil) {
            return left;
        }
        if (SemanticTypes::isNullable(right)) {
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

    if (SemanticTypes::isNullable(right)) {
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
