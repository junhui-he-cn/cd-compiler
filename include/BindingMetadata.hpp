#pragma once

#include "SourceIdentity.hpp"

#include <string>

// Storage is an explicit compiler fact. Unknown or missing metadata is
// intentionally conservative and cannot be promoted into SSA state.
enum class BindingStorageClass {
    Unknown,
    Local,
    Captured,
    Module,
    Exported,
    Synthetic,
};

struct IRBinding {
    BindingId bindingId;
    std::string resolvedName;
    BindingStorageClass storage = BindingStorageClass::Unknown;

    bool canPromote() const
    {
        return storage == BindingStorageClass::Local;
    }
};
