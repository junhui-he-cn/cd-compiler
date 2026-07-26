#include "ModuleCache.hpp"
#include "ModuleInterfaceArtifact.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

ModuleCacheModule module(
    const std::string& identity,
    const std::string& sourceHash,
    const std::string& interfaceHash)
{
    ModuleCacheModule value;
    value.identity = identity;
    value.sourceHash = sourceHash;
    value.interfaceHash = interfaceHash;
    return value;
}

ModuleCacheDependency dependency(
    const std::string& identity,
    const std::string& interfaceHash)
{
    return ModuleCacheDependency{
        identity,
        ModuleGraphEdgeKind::Import,
        "./" + identity + ".cd",
        interfaceHash};
}

std::filesystem::path makeTemporaryDirectory()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("compiler-design-module-cache-" + std::to_string(stamp));
    std::filesystem::create_directories(path / "products");
    return path;
}

void touchProducts(
    const std::filesystem::path& cacheDirectory,
    const std::vector<ModuleCacheDecision>& decisions)
{
    for (const ModuleCacheDecision& decision : decisions) {
        std::ofstream output(cacheDirectory / decision.artifactPath);
        assert(output);
        output << "cached\n";
    }
}

ModuleCacheManifest manifestFrom(const std::vector<ModuleCacheDecision>& decisions)
{
    ModuleCacheManifest manifest;
    for (const ModuleCacheDecision& decision : decisions) {
        manifest.records.push_back(ModuleCacheRecord{
            decision.module,
            decision.artifactPath,
            moduleInterfaceArtifactPath({}, decision.module.identity).generic_string(),
        });
    }
    return manifest;
}

} // namespace

int main()
{
    const std::filesystem::path cacheDirectory = makeTemporaryDirectory();
    const std::filesystem::path manifestPath = cacheDirectory / "module-cache.cdbc";

    ModuleCacheModule lib = module("lib", "source-1", "public-1");
    ModuleCacheModule mid = module("mid", "source-1", "public-mid");
    mid.dependencies.push_back(dependency("lib", "public-1"));
    ModuleCacheModule entry = module("entry", "source-1", "public-entry");
    entry.isEntry = true;
    entry.entryOrder = 0;
    entry.dependencies.push_back(dependency("mid", "public-mid"));

    const std::vector<ModuleCacheModule> initial{lib, mid, entry};
    const std::vector<ModuleCacheDecision> first = planModuleCacheBuild(
        initial,
        ModuleCacheManifest{},
        cacheDirectory,
        "cache_miss");
    assert(first.size() == 3);
    for (const ModuleCacheDecision& decision : first) {
        assert(decision.status == ModuleCacheDecisionStatus::Rebuilt);
        assert(decision.reason == "cache_miss");
        assert(decision.publicImpact);
    }
    touchProducts(cacheDirectory, first);
    writeModuleCache(manifestPath, manifestFrom(first));

    const ModuleCacheLoadResult loaded = readModuleCache(manifestPath);
    assert(loaded.found);
    assert(loaded.error.empty());
    assert(loaded.manifest);
    assert(loaded.manifest->records.size() == 3);
    for (const ModuleCacheRecord& record : loaded.manifest->records) {
        assert(record.interfaceArtifactPath
            == moduleInterfaceArtifactPath({}, record.module.identity).generic_string());
    }

    lib.sourceHash = "source-2";
    const std::vector<ModuleCacheDecision> implementationOnly = planModuleCacheBuild(
        {lib, mid, entry},
        *loaded.manifest,
        cacheDirectory);
    assert(implementationOnly[0].status == ModuleCacheDecisionStatus::Rebuilt);
    assert(implementationOnly[0].reason == "source_changed");
    assert(!implementationOnly[0].publicImpact);
    assert(implementationOnly[1].status == ModuleCacheDecisionStatus::Reused);
    assert(implementationOnly[2].status == ModuleCacheDecisionStatus::Reused);

    lib.interfaceHash = "public-2";
    mid.dependencies[0].interfaceHash = "public-2";
    const std::vector<ModuleCacheDecision> publicChange = planModuleCacheBuild(
        {lib, mid, entry},
        *loaded.manifest,
        cacheDirectory);
    assert(publicChange[0].status == ModuleCacheDecisionStatus::Rebuilt);
    assert(publicChange[0].publicImpact);
    assert(publicChange[1].status == ModuleCacheDecisionStatus::Rebuilt);
    assert(publicChange[1].reason == "dependency_interface_changed");
    assert(publicChange[1].publicImpact);
    assert(publicChange[2].status == ModuleCacheDecisionStatus::Rebuilt);
    assert(publicChange[2].reason == "transitive_dependency_interface_changed");
    assert(publicChange[2].publicImpact);

    ModuleCacheModule equivalent = module("lib", "source-2", "public-2");
    assert(moduleCacheKey(equivalent) == moduleCacheKey(equivalent));
    assert(moduleCacheKey(equivalent) != moduleCacheKey(module("lib", "source-3", "public-2")));

    std::error_code error;
    std::filesystem::remove_all(cacheDirectory, error);
    assert(!error);
    return 0;
}
