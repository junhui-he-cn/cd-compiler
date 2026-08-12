#pragma once

#include "TypeUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>

// Shared helpers for the TypeChecker implementation split across translation
// units. This header is internal and not part of the public API surface.
inline bool mapKeyTypeAllowed(const TypeInfo& type)
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

inline TypeInfo mergeReturnTypes(const TypeInfo& current, const TypeInfo& next)
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

inline bool isNativeCallbackName(const std::string& name)
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

inline std::optional<std::string> normalizedIntegerLiteral(const Expr& expression)
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
