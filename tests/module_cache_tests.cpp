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

ModuleCacheManifest manifestFrom(
    const std::filesystem::path& cacheDirectory,
    const std::vector<ModuleCacheDecision>& decisions)
{
    ModuleCacheManifest manifest;
    for (const ModuleCacheDecision& decision : decisions) {
        ModuleCacheRecord record{
            decision.module,
            decision.artifactPath,
            moduleInterfaceArtifactPath({}, decision.module.identity).generic_string(),
        };
        record.module.contentDigest = moduleCacheArtifactDigest(
            cacheDirectory / decision.artifactPath);
        manifest.records.push_back(std::move(record));
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
    writeModuleCache(manifestPath, manifestFrom(cacheDirectory, first));

    const ModuleCacheLoadResult loaded = readModuleCache(manifestPath);
    assert(loaded.found);
    assert(loaded.error.empty());
    assert(loaded.manifest);
    assert(loaded.manifest->schemaVersion == 4);
    assert(loaded.manifest->records.size() == 3);
    for (const ModuleCacheRecord& record : loaded.manifest->records) {
        assert(record.module.optimizationLevel == "O0");
        assert(record.module.optimizerPipeline == "m7-ssa-o0-v1");
        assert(!record.module.contentDigest.empty());
        assert(record.interfaceArtifactPath
            == moduleInterfaceArtifactPath({}, record.module.identity).generic_string());
    }

    const std::filesystem::path legacyManifestPath = cacheDirectory / "legacy-cache.cdbc";
    {
        std::ofstream legacy(legacyManifestPath);
        assert(legacy);
        legacy << "cdbc-cache 0.2\n\nschema = 2\nmodules:\n";
    }
    const ModuleCacheLoadResult legacy = readModuleCache(legacyManifestPath);
    assert(legacy.found);
    assert(!legacy.manifest);
    assert(!legacy.error.empty());

    const std::filesystem::path schema3ManifestPath = cacheDirectory / "schema-3-cache.cdbc";
    {
        std::ofstream schema3(schema3ManifestPath);
        assert(schema3);
        schema3 << "cdbc-cache 0.2\n\nschema = 3\nmodules:\n";
    }
    const ModuleCacheLoadResult schema3 = readModuleCache(schema3ManifestPath);
    assert(schema3.found);
    assert(!schema3.manifest);
    assert(!schema3.error.empty());

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

    ModuleCacheModule optimized = module("lib", "source-1", "public-1");
    optimized.optimizationLevel = "O1";
    optimized.optimizerPipeline = "m7-ssa-o1-copy-phi-const-branch-dce-reach-thread-merge-v7";
    assert(moduleCacheKey(optimized) != moduleCacheKey(equivalent));
    ModuleCacheModule baselineMid = module("mid", "source-1", "public-mid");
    baselineMid.dependencies.push_back(dependency("lib", "public-1"));
    ModuleCacheModule baselineEntry = module("entry", "source-1", "public-entry");
    baselineEntry.isEntry = true;
    baselineEntry.entryOrder = 0;
    baselineEntry.dependencies.push_back(dependency("mid", "public-mid"));
    const std::vector<ModuleCacheDecision> optimizationChange = planModuleCacheBuild(
        {optimized, baselineMid, baselineEntry},
        *loaded.manifest,
        cacheDirectory);
    assert(optimizationChange[0].status == ModuleCacheDecisionStatus::Rebuilt);
    assert(optimizationChange[0].reason == "optimization_configuration_changed");
    assert(!optimizationChange[0].publicImpact);
    assert(optimizationChange[1].status == ModuleCacheDecisionStatus::Reused);
    assert(optimizationChange[2].status == ModuleCacheDecisionStatus::Reused);

    ModuleCacheModule originalLib = module("lib", "source-1", "public-1");
    const std::filesystem::path originalLibProduct = cacheDirectory
        / moduleCacheArtifactPath(originalLib);
    {
        std::ofstream corrupted(originalLibProduct);
        assert(corrupted);
        corrupted << "corrupted\n";
    }
    const std::vector<ModuleCacheDecision> corrupted = planModuleCacheBuild(
        {originalLib, baselineMid, baselineEntry},
        *loaded.manifest,
        cacheDirectory);
    assert(corrupted[0].status == ModuleCacheDecisionStatus::Rebuilt);
    assert(corrupted[0].reason == "cache_artifact_invalid");
    assert(corrupted[1].status == ModuleCacheDecisionStatus::Reused);
    assert(corrupted[2].status == ModuleCacheDecisionStatus::Reused);

    std::error_code error;
    std::filesystem::remove_all(cacheDirectory, error);
    assert(!error);
    return 0;
}
