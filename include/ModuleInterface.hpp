#pragma once

#include "ModuleGraph.hpp"
#include "SourceIdentity.hpp"
#include "TypeUtils.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct ModuleInterfaceValue {
    std::string name;
    TypeInfo type;
};

struct ModuleInterfaceField {
    std::string name;
    TypeInfo type;
};

struct ModuleInterfaceMethod {
    std::string name;
    std::vector<TypeInfo> parameterTypes;
    TypeInfo returnType;
    std::vector<std::string> genericParameters;
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints;
};

struct ModuleInterfaceStruct {
    std::string name;
    std::vector<std::string> genericParameters;
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints;
    std::vector<ModuleInterfaceField> fields;
    std::vector<ModuleInterfaceMethod> methods;
};

struct ModuleInterfaceVariant {
    std::string name;
    std::vector<TypeInfo> payloadTypes;
    std::vector<std::optional<std::string>> payloadNames;
};

struct ModuleInterfaceEnum {
    std::string name;
    std::vector<std::string> genericParameters;
    std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints;
    std::vector<ModuleInterfaceVariant> variants;
};

struct ModuleInterfaceDependency {
    std::size_t importedModuleId = 0;
    ModuleGraphEdgeKind kind = ModuleGraphEdgeKind::Import;
    std::string requestedPath;
};

struct ModuleInterface {
    std::size_t moduleId = 0;
    SourceFileId sourceId;
    std::string path;
    std::string canonicalPath;
    bool isEntry = false;
    std::vector<ModuleInterfaceDependency> dependencies;
    std::vector<ModuleInterfaceValue> values;
    std::vector<ModuleInterfaceStruct> structs;
    std::vector<ModuleInterfaceEnum> enums;
};
