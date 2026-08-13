#include "ModuleSymbols.hpp"

#include <utility>

void ModuleSymbols::clear()
{
    valueExports_.clear();
    structExports_.clear();
    enumExports_.clear();
    methodExports_.clear();
    localStructNames_.clear();
    localEnumNames_.clear();
    namespaces_.clear();
    directImports_.clear();
}

bool ModuleSymbols::markDirectImport(std::size_t importingModuleId, std::size_t importedModuleId)
{
    return directImports_[importingModuleId].insert(importedModuleId).second;
}

void ModuleSymbols::recordValueExport(std::size_t moduleId, std::string name, TypeBinding binding)
{
    valueExports_[moduleId].emplace(std::move(name), std::move(binding));
}

const ModuleValueExports* ModuleSymbols::valueExports(std::size_t moduleId) const
{
    const auto found = valueExports_.find(moduleId);
    return found == valueExports_.end() ? nullptr : &found->second;
}

void ModuleSymbols::markLocalStruct(std::size_t moduleId, const std::string& name)
{
    localStructNames_[moduleId].insert(name);
}

bool ModuleSymbols::isLocalStruct(std::size_t moduleId, const std::string& name) const
{
    const auto found = localStructNames_.find(moduleId);
    return found != localStructNames_.end() && found->second.find(name) != found->second.end();
}

void ModuleSymbols::markLocalEnum(std::size_t moduleId, const std::string& name)
{
    localEnumNames_[moduleId].insert(name);
}

bool ModuleSymbols::isLocalEnum(std::size_t moduleId, const std::string& name) const
{
    const auto found = localEnumNames_.find(moduleId);
    return found != localEnumNames_.end() && found->second.find(name) != found->second.end();
}

void ModuleSymbols::recordStructExport(std::size_t moduleId, std::string name, StructTypeDecl declaration)
{
    structExports_[moduleId].emplace(std::move(name), std::move(declaration));
}

const ModuleStructExports* ModuleSymbols::structExports(std::size_t moduleId) const
{
    const auto found = structExports_.find(moduleId);
    return found == structExports_.end() ? nullptr : &found->second;
}

void ModuleSymbols::recordEnumExport(std::size_t moduleId, std::string name, EnumTypeDecl declaration)
{
    enumExports_[moduleId].emplace(std::move(name), std::move(declaration));
}

const ModuleEnumExports* ModuleSymbols::enumExports(std::size_t moduleId) const
{
    const auto found = enumExports_.find(moduleId);
    return found == enumExports_.end() ? nullptr : &found->second;
}

void ModuleSymbols::recordMethodExport(
    std::size_t moduleId,
    std::string structName,
    std::string methodName,
    MethodSignature signature)
{
    methodExports_[moduleId][std::move(structName)].emplace(std::move(methodName), std::move(signature));
}

const ModuleMethodExports* ModuleSymbols::methodExports(std::size_t moduleId) const
{
    const auto found = methodExports_.find(moduleId);
    return found == methodExports_.end() ? nullptr : &found->second;
}

bool ModuleSymbols::hasValueExport(std::size_t moduleId, const std::string& name) const
{
    const ModuleValueExports* exports = valueExports(moduleId);
    return exports && exports->find(name) != exports->end();
}

bool ModuleSymbols::hasStructExport(std::size_t moduleId, const std::string& name) const
{
    const ModuleStructExports* exports = structExports(moduleId);
    return exports && exports->find(name) != exports->end();
}

bool ModuleSymbols::hasEnumExport(std::size_t moduleId, const std::string& name) const
{
    const ModuleEnumExports* exports = enumExports(moduleId);
    return exports && exports->find(name) != exports->end();
}

bool ModuleSymbols::hasAnyExport(std::size_t moduleId, const std::string& name) const
{
    return hasValueExport(moduleId, name) || hasStructExport(moduleId, name) || hasEnumExport(moduleId, name);
}

void ModuleSymbols::recordMethodExports(std::size_t moduleId, std::string structName, const StructMethodTable& methods)
{
    if (methods.empty()) {
        return;
    }
    auto& destination = methodExports_[moduleId][std::move(structName)];
    for (const auto& entry : methods) {
        destination.emplace(entry.first, entry.second);
    }
}

bool ModuleSymbols::hasNamespace(std::size_t moduleId, const std::string& alias) const
{
    return namespaceImport(moduleId, alias) != nullptr;
}

void ModuleSymbols::recordNamespace(std::size_t moduleId, std::string alias, NamespaceImport imported)
{
    namespaces_[moduleId].emplace(std::move(alias), std::move(imported));
}

const NamespaceImport* ModuleSymbols::namespaceImport(std::size_t moduleId, const std::string& alias) const
{
    const auto table = namespaces_.find(moduleId);
    if (table == namespaces_.end()) {
        return nullptr;
    }
    const auto found = table->second.find(alias);
    return found == table->second.end() ? nullptr : &found->second;
}
