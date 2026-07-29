#pragma once

#include "Token.hpp"
#include "TypeUtils.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct TypeBinding {
    TypeInfo type;
    std::string resolvedName;
    std::size_t scopeDepth = 0;
    std::size_t functionDepth = 0;
    bool explicitType = false;
    bool imported = false;
    BindingId bindingId;
    DeclarationId declarationId;
    SymbolId symbolId;
    ScopeId scopeId;
    std::optional<SourceRange> range;
};

struct StructFieldType {
    Token name;
    TypeInfo type;
    bool isPrivate = false;
};

struct StructTypeDecl {
    Token name;
    std::vector<StructFieldType> fields;
    std::vector<std::string> genericParameters;
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints;
    bool hasPrivateFields = false;
    // Snapshot-local ownership used by the checker to distinguish a local
    // declaration from a type imported from another module.  It is never
    // serialized into a module interface artifact.
    std::optional<std::size_t> definingModuleId;
};

struct EnumVariantType {
    Token name;
    std::vector<TypeInfo> payloadTypes;
    std::vector<std::optional<Token>> payloadNames;
};

struct EnumTypeDecl {
    Token name;
    std::vector<std::string> genericParameters;
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints;
    std::vector<EnumVariantType> variants;
};

struct MethodSignature {
    TypeInfo receiverType;
    std::vector<TypeInfo> parameterTypes;
    TypeInfo returnType;
    std::string resolvedName;
    std::vector<std::string> genericParameters;
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints;
};

// Operators have a deliberately separate public shape from ordinary methods.
// The operator declaration has exactly one right-hand parameter and cannot
// declare method-level type parameters; the generic metadata belongs to the
// receiver struct.
struct OperatorSignature {
    TypeInfo receiverType;
    TypeInfo rightParameterType;
    TypeInfo returnType;
    std::vector<std::string> genericParameters;
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints;
    std::string resolvedName;
};
