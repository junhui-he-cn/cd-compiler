#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

enum class StaticType {
    Unknown,
    Nil,
    Number,
    Bool,
    String,
    Function,
    Array,
    Map,
    Range,
    Struct,
    Enum,
    Nullable,
    TypeParameter,
    Capability,
};

struct TypeInfo {
    StaticType kind = StaticType::Unknown;
    std::vector<TypeInfo> parameterTypes;
    std::shared_ptr<TypeInfo> returnType;
    std::optional<std::string> structName;
    std::optional<std::string> enumName;
    std::vector<TypeInfo> typeArguments;
    std::shared_ptr<TypeInfo> elementType;
    std::shared_ptr<TypeInfo> keyType;
    std::shared_ptr<TypeInfo> valueType;
    std::shared_ptr<TypeInfo> nullableOf;
    std::optional<std::string> typeParameterName;
    std::shared_ptr<TypeInfo> typeParameterConstraint;
    std::vector<std::string> genericParameters;
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints;
};

using TypeSubstitutions = std::unordered_map<std::string, TypeInfo>;

TypeInfo unknownType();
TypeInfo simpleType(StaticType kind);
TypeInfo namedStructType(std::string name, std::vector<TypeInfo> typeArguments = {});
TypeInfo namedEnumType(std::string name, std::vector<TypeInfo> typeArguments = {});
TypeInfo arrayType(TypeInfo elementType);
TypeInfo mapType(TypeInfo keyType, TypeInfo valueType);
TypeInfo typeParameterType(std::string name, std::optional<TypeInfo> constraint = std::nullopt);
TypeInfo capabilityType(std::string name);
TypeInfo capabilitySetType(std::vector<TypeInfo> capabilities);
TypeInfo functionType(
    std::vector<TypeInfo> parameterTypes,
    TypeInfo returnType,
    std::vector<std::string> genericParameters = {},
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints = {});
TypeInfo nullableType(TypeInfo innerType);
TypeInfo functionWithoutSignature();

bool isKnown(const TypeInfo& type);
bool hasFunctionSignature(const TypeInfo& type);
bool isNullable(const TypeInfo& type);
bool isCapability(const TypeInfo& type, const std::string& name);
bool isCapabilitySet(const TypeInfo& type);
bool satisfiesCapability(const TypeInfo& actual, const TypeInfo& capability);
bool compatible(const TypeInfo& expected, const TypeInfo& actual);
std::optional<TypeInfo> mergeArrayElementTypes(const TypeInfo& left, const TypeInfo& right);
TypeInfo substituteTypeParameters(const TypeInfo& type, const TypeSubstitutions& substitutions);

namespace SemanticTypes {

struct TypeInferenceConflict {
    std::string parameterName;
    TypeInfo first;
    TypeInfo second;
};

struct TypeConstraintViolation {
    std::string parameterName;
    TypeInfo constraint;
    TypeInfo actual;
};

bool isKnown(const TypeInfo& type);
bool hasFunctionSignature(const TypeInfo& type);
bool isNullable(const TypeInfo& type);
bool isCapability(const TypeInfo& type, const std::string& name);
bool isCapabilitySet(const TypeInfo& type);
bool satisfiesCapability(const TypeInfo& actual, const TypeInfo& capability);
bool compatible(const TypeInfo& expected, const TypeInfo& actual);
std::optional<TypeInfo> mergeArrayElementTypes(const TypeInfo& left, const TypeInfo& right);
TypeInfo substituteTypeParameters(const TypeInfo& type, const TypeSubstitutions& substitutions);
std::optional<TypeInferenceConflict> inferTypeArguments(
    const TypeInfo& expected,
    const TypeInfo& actual,
    TypeSubstitutions& substitutions);
std::optional<TypeConstraintViolation> validateTypeParameterConstraints(
    const std::vector<std::string>& parameters,
    const std::vector<std::shared_ptr<TypeInfo>>& constraints,
    const TypeSubstitutions& substitutions);

} // namespace SemanticTypes

std::string staticTypeName(StaticType type);
std::string typeInfoName(const TypeInfo& type);
