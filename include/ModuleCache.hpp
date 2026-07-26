#pragma once

#include "ModuleGraph.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

// Persistent cache records deliberately contain only cross-process data.  In
// particular, module/source/syntax snapshot IDs never appear here.
struct ModuleCacheDependency {
    std::string identity;
    ModuleGraphEdgeKind kind = ModuleGraphEdgeKind::Import;
    std::string requestedPath;
    std::string interfaceHash;
};

struct ModuleCacheModule {
    std::string identity;
    std::string sourceHash;
    std::string interfaceHash;
    bool isEntry = false;
    std::optional<std::size_t> entryOrder;
    std::vector<ModuleCacheDependency> dependencies;
    std::string cacheKey;
};

struct ModuleCacheRecord {
    ModuleCacheModule module;
    std::string artifactPath;
    std::string interfaceArtifactPath;
};

struct ModuleCacheManifest {
    int schemaVersion = 2;
    std::vector<ModuleCacheRecord> records;
};

struct ModuleCacheLoadResult {
    bool found = false;
    std::optional<ModuleCacheManifest> manifest;
    std::string error;
};

enum class ModuleCacheDecisionStatus {
    Reused,
    Rebuilt,
};

struct ModuleCacheDecision {
    ModuleCacheModule module;
    ModuleCacheDecisionStatus status = ModuleCacheDecisionStatus::Rebuilt;
    std::string reason;
    std::string artifactPath;
    bool publicImpact = false;
};

std::string moduleCacheHash(const std::string& value);
std::string moduleCacheKey(const ModuleCacheModule& module);
std::string moduleCacheArtifactPath(const ModuleCacheModule& module);

ModuleCacheLoadResult readModuleCache(const std::filesystem::path& path);
void writeModuleCache(
    const std::filesystem::path& path,
    const ModuleCacheManifest& manifest);

std::vector<ModuleCacheDecision> planModuleCacheBuild(
    const std::vector<ModuleCacheModule>& modules,
    const ModuleCacheManifest& previous,
    const std::filesystem::path& cacheDirectory,
    const std::string& emptyCacheReason = "new_module");

void writeModuleRebuildReport(
    std::ostream& out,
    const std::vector<ModuleCacheDecision>& decisions,
    bool cacheFound,
    const std::string& cacheError = {});
