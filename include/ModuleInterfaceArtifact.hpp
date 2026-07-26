#pragma once

#include "ModuleInterface.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

// A portable dependency reference.  ModuleInterface::dependencies keeps
// snapshot-local module IDs for the current in-memory checker; sidecars use
// canonical identities instead.
struct ModuleInterfaceArtifactDependency {
    std::string identity;
    ModuleGraphEdgeKind kind = ModuleGraphEdgeKind::Import;
    std::string requestedPath;
    std::string interfaceHash;
};

struct ModuleInterfaceArtifact {
    std::string identity;
    std::string path;
    std::string canonicalPath;
    std::string sourceHash;
    std::string interfaceHash;
    bool isEntry = false;
    std::optional<std::size_t> entryOrder;
    std::size_t resolvedNameNext = 0;
    std::vector<ModuleInterfaceArtifactDependency> dependencies;
    ModuleInterface interfaceInfo;
};

struct ModuleInterfaceArtifactLoadResult {
    bool found = false;
    std::optional<ModuleInterfaceArtifact> artifact;
    std::string error;
};

std::string moduleInterfaceArtifactHash(const ModuleInterface& interfaceInfo);
std::string moduleInterfaceArtifactFileName(const std::string& identity);
std::filesystem::path moduleInterfaceArtifactPath(
    const std::filesystem::path& cacheDirectory,
    const std::string& identity);

void writeModuleInterfaceArtifactText(
    std::ostream& out,
    const ModuleInterfaceArtifact& artifact);
ModuleInterfaceArtifactLoadResult readModuleInterfaceArtifactText(const std::string& source);

void writeModuleInterfaceArtifact(
    const std::filesystem::path& path,
    const ModuleInterfaceArtifact& artifact);
ModuleInterfaceArtifactLoadResult readModuleInterfaceArtifact(
    const std::filesystem::path& path);
