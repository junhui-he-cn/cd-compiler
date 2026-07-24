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
bool compatible(const TypeInfo& expected, const TypeInfo& actual);
std::optional<TypeInfo> mergeArrayElementTypes(const TypeInfo& left, const TypeInfo& right);
TypeInfo substituteTypeParameters(const TypeInfo& type, const TypeSubstitutions& substitutions);

namespace SemanticTypes {

bool compatible(const TypeInfo& expected, const TypeInfo& actual);
std::optional<TypeInfo> mergeArrayElementTypes(const TypeInfo& left, const TypeInfo& right);
TypeInfo substituteTypeParameters(const TypeInfo& type, const TypeSubstitutions& substitutions);

} // namespace SemanticTypes

std::string staticTypeName(StaticType type);
std::string typeInfoName(const TypeInfo& type);
