#pragma once

#include <cstddef>
#include <optional>
#include <string>

enum class NativeFunctionKind {
    Push,
    Pop,
    Remove,
    Clear,
    Merge,
    Keys,
    Values,
    Floor,
    Ceil,
    Sqrt,
    Str,
    Substr,
    CharAt,
    TypeOf,
    Hash,
    Contains,
    Slice,
    Copy,
    Concat,
    Map,
    Filter,
    FlatMap,
    Any,
    All,
    Count,
    Find,
    FindIndex,
    Reduce,
    Range,
};

struct NativeFunctionSignature {
    const char* name;
    std::size_t arity;
    NativeFunctionKind kind;
    // Zero means the function has exactly `arity` arguments. Otherwise this
    // is the inclusive maximum arity.
    std::size_t maxArity = 0;
};

const NativeFunctionSignature* findNativeStdlibFunction(const std::string& name);
bool isNativeStdlibName(const std::string& name);
std::optional<std::size_t> nativeStdlibArity(const std::string& name);
