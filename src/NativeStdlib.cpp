#include "NativeStdlib.hpp"

#include <array>

namespace {

constexpr std::array<NativeFunctionSignature, 30> kNativeFunctions{{
    {"push", 2, NativeFunctionKind::Push},
    {"pop", 1, NativeFunctionKind::Pop},
    {"remove", 2, NativeFunctionKind::Remove},
    {"clear", 1, NativeFunctionKind::Clear},
    {"merge", 2, NativeFunctionKind::Merge},
    {"keys", 1, NativeFunctionKind::Keys},
    {"values", 1, NativeFunctionKind::Values},
    {"floor", 1, NativeFunctionKind::Floor},
    {"ceil", 1, NativeFunctionKind::Ceil},
    {"sqrt", 1, NativeFunctionKind::Sqrt},
    {"str", 1, NativeFunctionKind::Str},
    {"substr", 3, NativeFunctionKind::Substr},
    {"charAt", 2, NativeFunctionKind::CharAt},
    {"typeOf", 1, NativeFunctionKind::TypeOf},
    {"hash", 1, NativeFunctionKind::Hash},
    {"contains", 2, NativeFunctionKind::Contains},
    {"slice", 3, NativeFunctionKind::Slice},
    {"copy", 1, NativeFunctionKind::Copy},
    {"concat", 2, NativeFunctionKind::Concat},
    {"map", 2, NativeFunctionKind::Map},
    {"filter", 2, NativeFunctionKind::Filter},
    {"flatMap", 2, NativeFunctionKind::FlatMap},
    {"any", 2, NativeFunctionKind::Any},
    {"all", 2, NativeFunctionKind::All},
    {"count", 2, NativeFunctionKind::Count},
    {"find", 2, NativeFunctionKind::Find},
    {"findIndex", 2, NativeFunctionKind::FindIndex},
    {"reduce", 3, NativeFunctionKind::Reduce},
    {"range", 1, NativeFunctionKind::Range, 3},
    {"print", 1, NativeFunctionKind::Print},
}};

} // namespace

const NativeFunctionSignature* findNativeStdlibFunction(const std::string& name)
{
    for (const NativeFunctionSignature& function : kNativeFunctions) {
        if (name == function.name) {
            return &function;
        }
    }
    return nullptr;
}

bool isNativeStdlibName(const std::string& name)
{
    return findNativeStdlibFunction(name) != nullptr;
}

std::optional<std::size_t> nativeStdlibArity(const std::string& name)
{
    const NativeFunctionSignature* function = findNativeStdlibFunction(name);
    if (!function) {
        return std::nullopt;
    }
    return function->arity;
}
