#pragma once

#include "TypeCheckerTypes.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

using ModuleValueExports = std::unordered_map<std::string, TypeBinding>;
using ModuleStructExports = std::unordered_map<std::string, StructTypeDecl>;
using ModuleEnumExports = std::unordered_map<std::string, EnumTypeDecl>;
using StructMethodTable = std::unordered_map<std::string, MethodSignature>;
using ModuleMethodExports = std::unordered_map<std::string, StructMethodTable>;

struct NamespaceImport {
    ModuleValueExports values;
    ModuleStructExports structs;
    ModuleEnumExports enums;
    ModuleMethodExports methods;
};

class ModuleSymbols {
public:
    void clear();

    bool markDirectImport(std::size_t importingModuleId, std::size_t importedModuleId);

    void recordValueExport(std::size_t moduleId, std::string name, TypeBinding binding);
    const ModuleValueExports* valueExports(std::size_t moduleId) const;

    void markLocalStruct(std::size_t moduleId, const std::string& name);
    bool isLocalStruct(std::size_t moduleId, const std::string& name) const;
    void markLocalEnum(std::size_t moduleId, const std::string& name);
    bool isLocalEnum(std::size_t moduleId, const std::string& name) const;
    void recordStructExport(std::size_t moduleId, std::string name, StructTypeDecl declaration);
    const ModuleStructExports* structExports(std::size_t moduleId) const;
    void recordEnumExport(std::size_t moduleId, std::string name, EnumTypeDecl declaration);
    const ModuleEnumExports* enumExports(std::size_t moduleId) const;
    void recordMethodExport(std::size_t moduleId, std::string structName, std::string methodName, MethodSignature signature);
    const ModuleMethodExports* methodExports(std::size_t moduleId) const;
    bool hasValueExport(std::size_t moduleId, const std::string& name) const;
    bool hasStructExport(std::size_t moduleId, const std::string& name) const;
    bool hasEnumExport(std::size_t moduleId, const std::string& name) const;
    bool hasAnyExport(std::size_t moduleId, const std::string& name) const;
    void recordMethodExports(std::size_t moduleId, std::string structName, const StructMethodTable& methods);

    bool hasNamespace(std::size_t moduleId, const std::string& alias) const;
    void recordNamespace(std::size_t moduleId, std::string alias, NamespaceImport imported);
    const NamespaceImport* namespaceImport(std::size_t moduleId, const std::string& alias) const;

private:
    std::unordered_map<std::size_t, ModuleValueExports> valueExports_;
    std::unordered_map<std::size_t, ModuleStructExports> structExports_;
    std::unordered_map<std::size_t, ModuleEnumExports> enumExports_;
    std::unordered_map<std::size_t, ModuleMethodExports> methodExports_;
    std::unordered_map<std::size_t, std::unordered_set<std::string>> localStructNames_;
    std::unordered_map<std::size_t, std::unordered_set<std::string>> localEnumNames_;
    std::unordered_map<std::size_t, std::unordered_map<std::string, NamespaceImport>> namespaces_;
    std::unordered_map<std::size_t, std::unordered_set<std::size_t>> directImports_;
};
