#include "Ast.hpp"
#include "Diagnostic.hpp"
#include "FrontendSession.hpp"
#include "LosslessSource.hpp"
#include "ModuleCache.hpp"
#include "ModuleInterfaceArtifact.hpp"
#include "TypeChecker.hpp"
#include "TypeUtils.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string pathString(const fs::path& path)
{
    return path.lexically_normal().generic_string();
}

void writeFile(const fs::path& path, const std::string& source)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << source;
}

std::string readFile(const fs::path& path)
{
    std::ifstream input(path);
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

void replaceText(std::string& source, const std::string& from, const std::string& to)
{
    const std::size_t position = source.find(from);
    assert(position != std::string::npos);
    source.replace(position, from.size(), to);
}

const ModuleStmt* moduleByPath(const Program& program, const fs::path& path)
{
    const std::string expected = pathString(path);
    for (const StmtPtr& statement : program.statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (module && module->path == expected) {
            return module;
        }
    }
    assert(false && "expected module path not found");
    return nullptr;
}

void expectImportError(const std::function<void()>& action, const std::string& expectedMessage)
{
    try {
        action();
    } catch (const DiagnosticError& error) {
        assert(error.kind() == DiagnosticKind::Import);
        assert(error.message() == expectedMessage);
        return;
    }
    assert(false && "expected import error");
}

std::string writeCachedModule(
    const fs::path& cacheDirectory,
    const fs::path& path,
    const std::string& source,
    ModuleInterface interfaceInfo,
    const std::vector<ModuleInterfaceArtifactDependency>& dependencies)
{
    const std::string identity = pathString(fs::weakly_canonical(path));
    interfaceInfo.path = pathString(path);
    interfaceInfo.canonicalPath = identity;
    interfaceInfo.isEntry = false;

    ModuleInterfaceArtifact artifact;
    artifact.identity = identity;
    artifact.path = interfaceInfo.path;
    artifact.canonicalPath = identity;
    artifact.sourceHash = moduleCacheHash(source);
    artifact.isEntry = false;
    artifact.dependencies = dependencies;
    artifact.interfaceInfo = interfaceInfo;
    writeModuleInterfaceArtifact(moduleInterfaceArtifactPath(cacheDirectory, identity), artifact);

    ModuleCacheModule cacheModule;
    cacheModule.identity = identity;
    cacheModule.sourceHash = artifact.sourceHash;
    cacheModule.interfaceHash = moduleInterfaceArtifactHash(interfaceInfo);
    for (const ModuleInterfaceArtifactDependency& dependency : dependencies) {
        cacheModule.dependencies.push_back(ModuleCacheDependency{
            dependency.identity,
            dependency.kind,
            dependency.requestedPath,
            dependency.interfaceHash,
        });
    }
    writeFile(cacheDirectory / moduleCacheArtifactPath(cacheModule), "cached module\n");
    return cacheModule.interfaceHash;
}

void test_canonical_duplicate_import_spellings_are_deduplicated(const fs::path& root)
{
    fs::remove_all(root);
    fs::create_directories(root / "nested");

    writeFile(root / "shared.cd", "let value = 1;\nexport value;\n");
    writeFile(
        root / "input.cd",
        "import \"./shared.cd\";\n"
        "import \"./nested/../shared.cd\";\n"
        "print value;\n");

    FrontendSession session;
    Program program = session.loadFiles({(root / "input.cd").string()});

    assert(session.moduleCount() == 2);
    assert(program.statements.size() == 2);
    const auto* entry = moduleByPath(program, root / "input.cd");
    const auto* first = dynamic_cast<const ImportStmt*>(entry->statements[0].get());
    const auto* second = dynamic_cast<const ImportStmt*>(entry->statements[1].get());
    assert(first != nullptr && second != nullptr);
    assert(first->resolvedModuleId == second->resolvedModuleId);
}

void test_search_path_resolves_extensionless_import_and_reexport(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path app = root / "app";
    const fs::path search = root / "modules";

    writeFile(search / "lib.cd", "let value = 7;\nexport value;\n");
    writeFile(app / "api.cd", "export value from \"lib\";\n");
    writeFile(
        app / "input.cd",
        "import \"api\";\n"
        "import \"lib\";\n"
        "print value;\n");

    FrontendSession session;
    session.setImportSearchPaths({search.string()});
    Program program = session.loadFiles({(app / "input.cd").string()});

    assert(session.moduleCount() == 3);
    const auto* lib = moduleByPath(program, search / "lib.cd");
    const auto* api = moduleByPath(program, app / "api.cd");
    const auto* entry = moduleByPath(program, app / "input.cd");

    const auto* reExport = dynamic_cast<const ExportStmt*>(api->statements[0].get());
    assert(reExport != nullptr);
    assert(reExport->resolvedModuleId == lib->moduleId);

    const auto* apiImport = dynamic_cast<const ImportStmt*>(entry->statements[0].get());
    const auto* libImport = dynamic_cast<const ImportStmt*>(entry->statements[1].get());
    assert(apiImport != nullptr && libImport != nullptr);
    assert(apiImport->resolvedModuleId == api->moduleId);
    assert(libImport->resolvedModuleId == lib->moduleId);

    const ModuleGraph& graph = session.moduleGraph();
    assert(program.moduleGraph.has_value());
    assert(program.moduleGraph->nodes.size() == graph.nodes.size());
    assert(program.moduleGraph->edges.size() == graph.edges.size());
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        assert(program.moduleGraph->nodes[index].moduleId == graph.nodes[index].moduleId);
        assert(program.moduleGraph->nodes[index].canonicalPath == graph.nodes[index].canonicalPath);
        const auto source = std::find_if(
            program.sources.begin(),
            program.sources.end(),
            [&](const SourceFile& file) { return file.id == graph.nodes[index].sourceId; });
        assert(source != program.sources.end());
        assert(source->moduleIdentity == graph.nodes[index].canonicalPath);
    }
    for (std::size_t index = 0; index < graph.edges.size(); ++index) {
        assert(program.moduleGraph->edges[index].importingModuleId == graph.edges[index].importingModuleId);
        assert(program.moduleGraph->edges[index].importedModuleId == graph.edges[index].importedModuleId);
        assert(program.moduleGraph->edges[index].kind == graph.edges[index].kind);
        assert(program.moduleGraph->edges[index].requestedPath == graph.edges[index].requestedPath);
    }
    assert(graph.nodes.size() == 3);
    assert(graph.edges.size() == 3);
    assert(std::count_if(
        graph.nodes.begin(),
        graph.nodes.end(),
        [](const ModuleGraphNode& node) { return node.isEntry; }) == 1);
    assert(std::all_of(
        graph.nodes.begin(),
        graph.nodes.end(),
        [](const ModuleGraphNode& node) {
            return node.sourceId.valid() && !node.path.empty() && !node.canonicalPath.empty();
        }));
    assert(std::count_if(
        graph.edges.begin(),
        graph.edges.end(),
        [](const ModuleGraphEdge& edge) {
            return edge.kind == ModuleGraphEdgeKind::Import && edge.requestedPath == "api";
        }) == 1);
    assert(std::count_if(
        graph.edges.begin(),
        graph.edges.end(),
        [](const ModuleGraphEdge& edge) {
            return edge.kind == ModuleGraphEdgeKind::Import && edge.requestedPath == "lib";
        }) == 1);
    assert(std::count_if(
        graph.edges.begin(),
        graph.edges.end(),
        [](const ModuleGraphEdge& edge) {
            return edge.kind == ModuleGraphEdgeKind::ReExport && edge.requestedPath == "lib";
        }) == 1);
    assert(std::all_of(
        graph.edges.begin(),
        graph.edges.end(),
        [&graph](const ModuleGraphEdge& edge) {
            const auto hasNode = [&graph](std::size_t id) {
                return std::any_of(
                    graph.nodes.begin(),
                    graph.nodes.end(),
                    [id](const ModuleGraphNode& node) { return node.moduleId == id; });
            };
            return hasNode(edge.importingModuleId) && hasNode(edge.importedModuleId);
        }));

    FrontendSession equivalentSession;
    equivalentSession.setImportSearchPaths({search.string()});
    (void)equivalentSession.loadFiles({(app / "input.cd").string()});
    const ModuleGraph& equivalentGraph = equivalentSession.moduleGraph();
    assert(equivalentGraph.nodes.size() == graph.nodes.size());
    assert(equivalentGraph.edges.size() == graph.edges.size());
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        const ModuleGraphNode& expected = graph.nodes[index];
        const ModuleGraphNode& actual = equivalentGraph.nodes[index];
        assert(actual.moduleId == expected.moduleId);
        assert(actual.sourceId == expected.sourceId);
        assert(actual.path == expected.path);
        assert(actual.canonicalPath == expected.canonicalPath);
        assert(actual.isEntry == expected.isEntry);
    }
    for (std::size_t index = 0; index < graph.edges.size(); ++index) {
        const ModuleGraphEdge& expected = graph.edges[index];
        const ModuleGraphEdge& actual = equivalentGraph.edges[index];
        assert(actual.importingModuleId == expected.importingModuleId);
        assert(actual.importedModuleId == expected.importedModuleId);
        assert(actual.kind == expected.kind);
        assert(actual.requestedPath == expected.requestedPath);
    }

    std::istringstream stdinSource("print 0;\n");
    session.loadStdin(stdinSource);
    assert(session.moduleGraph().nodes.empty());
    assert(session.moduleGraph().edges.empty());
    assert(program.moduleGraph.has_value());
    assert(program.moduleGraph->nodes.size() == 3);
    assert(program.moduleGraph->edges.size() == 3);
}

void test_importing_file_directory_precedes_search_path(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path app = root / "app";
    const fs::path search = root / "modules";

    writeFile(app / "math.cd", "let value = \"local\";\nexport value;\n");
    writeFile(search / "math.cd", "let value = \"search\";\nexport value;\n");
    writeFile(app / "input.cd", "import \"math\";\nprint value;\n");

    FrontendSession session;
    session.setImportSearchPaths({search.string()});
    Program program = session.loadFiles({(app / "input.cd").string()});

    assert(session.moduleCount() == 2);
    const auto* localMath = moduleByPath(program, app / "math.cd");
    const auto* entry = moduleByPath(program, app / "input.cd");
    const auto* import = dynamic_cast<const ImportStmt*>(entry->statements[0].get());
    assert(import != nullptr);
    assert(import->resolvedModuleId == localMath->moduleId);
}

void test_explicit_relative_import_does_not_use_search_path(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path app = root / "app";
    const fs::path search = root / "modules";

    writeFile(search / "missing.cd", "let value = \"search\";\nexport value;\n");
    writeFile(app / "input.cd", "import \"./missing\";\nprint value;\n");

    FrontendSession session;
    session.setImportSearchPaths({search.string()});
    expectImportError(
        [&]() { session.loadFiles({(app / "input.cd").string()}); },
        "failed to open import: " + pathString(app / "missing"));
}

void test_module_interface_cache_hit_reuses_dependency_interfaces(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path base = root / "base.cd";
    const fs::path library = root / "lib.cd";
    const fs::path entry = root / "entry.cd";
    const fs::path cache = root / "cache";
    const std::string baseSource =
        "let baseValue = 3;\n"
        "export baseValue;\n";
    const std::string librarySource =
        "import \"./base.cd\";\n"
        "let value = 7;\n"
        "export value;\n";
    writeFile(base, baseSource);
    writeFile(library, librarySource);
    writeFile(entry, "import \"./lib.cd\";\nprint value;\n");

    ModuleInterface baseInterface;
    baseInterface.values.push_back(ModuleInterfaceValue{
        "baseValue",
        simpleType(StaticType::Number),
        "baseValue#7",
    });
    const std::string baseHash = writeCachedModule(
        cache,
        base,
        baseSource,
        baseInterface,
        {});

    ModuleInterface libraryInterface;
    libraryInterface.values.push_back(ModuleInterfaceValue{
        "value",
        simpleType(StaticType::Number),
        "value#8",
    });
    const std::string libraryIdentity = pathString(fs::weakly_canonical(library));
    const std::string baseIdentity = pathString(fs::weakly_canonical(base));
    writeCachedModule(
        cache,
        library,
        librarySource,
        libraryInterface,
        {ModuleInterfaceArtifactDependency{
            baseIdentity,
            ModuleGraphEdgeKind::Import,
            "./base.cd",
            baseHash,
        }});

    FrontendSession session;
    session.setModuleInterfaceCacheDirectory(cache);
    Program program = session.loadFiles({entry.string()});

    assert(session.moduleCount() == 3);
    const ModuleStmt* cachedBase = moduleByPath(program, base);
    const ModuleStmt* cachedLibrary = moduleByPath(program, library);
    const ModuleStmt* entryModule = moduleByPath(program, entry);
    assert(cachedBase->statements.empty());
    assert(cachedLibrary->statements.empty());
    assert(entryModule->statements.size() == 2);
    const auto* entryImport = dynamic_cast<const ImportStmt*>(entryModule->statements[0].get());
    assert(entryImport != nullptr);
    assert(entryImport->resolvedModuleId == cachedLibrary->moduleId);

    const ModuleGraph& graph = session.moduleGraph();
    assert(graph.nodes.size() == 3);
    assert(graph.edges.size() == 2);
    assert(graph.edges[0].importingModuleId == cachedLibrary->moduleId);
    assert(graph.edges[0].importedModuleId == cachedBase->moduleId);
    assert(graph.edges[0].kind == ModuleGraphEdgeKind::Import);
    assert(graph.edges[0].requestedPath == "./base.cd");
    assert(graph.edges[1].importingModuleId == entryModule->moduleId);
    assert(graph.edges[1].importedModuleId == cachedLibrary->moduleId);
    assert(graph.edges[1].kind == ModuleGraphEdgeKind::Import);
    assert(graph.edges[1].requestedPath == "./lib.cd");
    assert(program.moduleGraph.has_value());
    assert(program.moduleGraph->edges.size() == graph.edges.size());
    for (std::size_t index = 0; index < graph.edges.size(); ++index) {
        assert(program.moduleGraph->edges[index].importingModuleId == graph.edges[index].importingModuleId);
        assert(program.moduleGraph->edges[index].importedModuleId == graph.edges[index].importedModuleId);
        assert(program.moduleGraph->edges[index].kind == graph.edges[index].kind);
        assert(program.moduleGraph->edges[index].requestedPath == graph.edges[index].requestedPath);
    }

    const std::vector<ModuleInterface>& preloaded = session.preloadedModuleInterfaces();
    assert(preloaded.size() == 2);
    assert(preloaded[0].moduleId == cachedBase->moduleId);
    assert(preloaded[0].sourceId == cachedBase->sourceId);
    assert(preloaded[0].canonicalPath == baseIdentity);
    assert(preloaded[0].values.size() == 1);
    assert(preloaded[0].values[0].name == "baseValue");
    assert(preloaded[1].moduleId == cachedLibrary->moduleId);
    assert(preloaded[1].sourceId == cachedLibrary->sourceId);
    assert(preloaded[1].canonicalPath == libraryIdentity);
    assert(preloaded[1].dependencies.size() == 1);
    assert(preloaded[1].dependencies[0].importedModuleId == cachedBase->moduleId);
    assert(preloaded[1].values.size() == 1);
    assert(preloaded[1].values[0].resolvedName == "value#8");

    TypeChecker checker;
    checker.setPreloadedModuleInterfaces(session.preloadedModuleInterfaces());
    checker.check(program);
    assert(checker.moduleInterfaceMismatchCount() == 0);
    assert(checker.checkedModuleBodyIds() == std::vector<std::size_t>{entryModule->moduleId});
}

void assertSourceFallback(
    const fs::path& caseRoot,
    const std::string& expectedDependencySource)
{
    FrontendSession session;
    session.setModuleInterfaceCacheDirectory(caseRoot / "cache");
    Program program = session.loadFiles({(caseRoot / "entry.cd").string()});
    const ModuleStmt* dependency = moduleByPath(program, caseRoot / "lib.cd");
    assert(!dependency->statements.empty());
    assert(dependency->source == expectedDependencySource);
    const std::string canonicalPath = pathString(fs::weakly_canonical(caseRoot / "lib.cd"));
    assert(std::none_of(
        session.preloadedModuleInterfaces().begin(),
        session.preloadedModuleInterfaces().end(),
        [&canonicalPath](const ModuleInterface& interfaceInfo) {
            return interfaceInfo.canonicalPath == canonicalPath;
        }));

    TypeChecker checker;
    checker.check(program);
    assert(checker.moduleInterfaceMismatchCount() == 0);
    assert(checker.checkedModuleBodyIds().size() == 2);
}

void writeSingleModuleCacheCase(
    const fs::path& caseRoot,
    const std::string& source)
{
    writeFile(caseRoot / "lib.cd", source);
    writeFile(
        caseRoot / "entry.cd",
        "import \"./lib.cd\";\n"
        "print value;\n");
}

std::string writeSingleModuleSidecar(
    const fs::path& caseRoot,
    const std::string& source)
{
    ModuleInterface interfaceInfo;
    interfaceInfo.values.push_back(ModuleInterfaceValue{
        "value",
        simpleType(StaticType::Number),
        "value#11",
    });
    return writeCachedModule(
        caseRoot / "cache",
        caseRoot / "lib.cd",
        source,
        interfaceInfo,
        {});
}

void test_module_interface_cache_fallbacks(const fs::path& root)
{
    const std::string validSource =
        "let value = 7;\n"
        "export value;\n";

    const fs::path missingSidecar = root / "missing_sidecar";
    fs::remove_all(missingSidecar);
    writeSingleModuleCacheCase(missingSidecar, validSource);
    fs::create_directories(missingSidecar / "cache");
    assertSourceFallback(missingSidecar, validSource);

    const fs::path malformedSidecar = root / "malformed_sidecar";
    fs::remove_all(malformedSidecar);
    writeSingleModuleCacheCase(malformedSidecar, validSource);
    (void)writeSingleModuleSidecar(malformedSidecar, validSource);
    writeFile(
        moduleInterfaceArtifactPath(
            malformedSidecar / "cache",
            pathString(fs::weakly_canonical(malformedSidecar / "lib.cd"))),
        "cdi 9.9\n");
    assertSourceFallback(malformedSidecar, validSource);

    const fs::path missingProduct = root / "missing_product";
    fs::remove_all(missingProduct);
    writeSingleModuleCacheCase(missingProduct, validSource);
    const std::string interfaceHash = writeSingleModuleSidecar(missingProduct, validSource);
    ModuleCacheModule cacheModule;
    cacheModule.identity = pathString(fs::weakly_canonical(missingProduct / "lib.cd"));
    cacheModule.sourceHash = moduleCacheHash(validSource);
    cacheModule.interfaceHash = interfaceHash;
    assert(fs::remove(missingProduct / "cache" / moduleCacheArtifactPath(cacheModule)));
    assertSourceFallback(missingProduct, validSource);

    const fs::path changedSource = root / "changed_source";
    fs::remove_all(changedSource);
    writeSingleModuleCacheCase(changedSource, validSource);
    (void)writeSingleModuleSidecar(changedSource, validSource);
    const std::string changedSourceText =
        "let value = 8;\n"
        "export value;\n";
    writeFile(changedSource / "lib.cd", changedSourceText);
    assertSourceFallback(changedSource, changedSourceText);
}

void test_module_interface_cache_dependency_hash_fallback(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path base = root / "base.cd";
    const fs::path library = root / "lib.cd";
    const fs::path entry = root / "entry.cd";
    const fs::path cache = root / "cache";
    const std::string baseSource =
        "let baseValue = 3;\n"
        "export baseValue;\n";
    const std::string librarySource =
        "import \"./base.cd\";\n"
        "let value = 7;\n"
        "export value;\n";
    writeFile(base, baseSource);
    writeFile(library, librarySource);
    writeFile(entry, "import \"./lib.cd\";\nprint value;\n");

    ModuleInterface baseInterface;
    baseInterface.values.push_back(ModuleInterfaceValue{
        "baseValue",
        simpleType(StaticType::Number),
        "baseValue#12",
    });
    const std::string baseHash = writeCachedModule(
        cache,
        base,
        baseSource,
        baseInterface,
        {});

    ModuleInterface libraryInterface;
    libraryInterface.values.push_back(ModuleInterfaceValue{
        "value",
        simpleType(StaticType::Number),
        "value#13",
    });
    const std::string baseIdentity = pathString(fs::weakly_canonical(base));
    writeCachedModule(
        cache,
        library,
        librarySource,
        libraryInterface,
        {ModuleInterfaceArtifactDependency{
            baseIdentity,
            ModuleGraphEdgeKind::Import,
            "./base.cd",
            baseHash + "-stale",
        }});

    FrontendSession session;
    session.setModuleInterfaceCacheDirectory(cache);
    Program program = session.loadFiles({entry.string()});
    const ModuleStmt* cachedBase = moduleByPath(program, base);
    const ModuleStmt* parsedLibrary = moduleByPath(program, library);
    assert(cachedBase->statements.empty());
    assert(!parsedLibrary->statements.empty());
    assert(session.preloadedModuleInterfaces().size() == 1);
    assert(session.preloadedModuleInterfaces().front().canonicalPath == baseIdentity);
    assert(session.moduleGraph().edges.size() == 2);
    assert(session.moduleGraph().edges[0].importingModuleId == parsedLibrary->moduleId);
    assert(session.moduleGraph().edges[0].importedModuleId == cachedBase->moduleId);

    TypeChecker checker;
    checker.setPreloadedModuleInterfaces(session.preloadedModuleInterfaces());
    checker.check(program);
    assert(checker.moduleInterfaceMismatchCount() == 0);
    assert(checker.checkedModuleBodyIds().size() == 2);
    assert(std::find(
               checker.checkedModuleBodyIds().begin(),
               checker.checkedModuleBodyIds().end(),
               cachedBase->moduleId)
        == checker.checkedModuleBodyIds().end());
}

void test_module_interface_cache_strict(const fs::path& root)
{
    const std::string validSource =
        "let value = 7;\n"
        "export value;\n";

    const auto expectedError = [](const fs::path& modulePath, const char* reason) {
        return "module interface cache rejected for "
            + pathString(fs::weakly_canonical(modulePath)) + ": " + reason;
    };
    const auto loadStrict = [](const fs::path& caseRoot) {
        FrontendSession session;
        session.setModuleInterfaceCacheDirectory(caseRoot / "cache");
        session.setModuleInterfaceCacheStrict(true);
        return session.loadFiles({(caseRoot / "entry.cd").string()});
    };

    const fs::path valid = root / "valid";
    fs::remove_all(valid);
    writeSingleModuleCacheCase(valid, validSource);
    (void)writeSingleModuleSidecar(valid, validSource);
    Program validProgram = loadStrict(valid);
    const ModuleStmt* validDependency = moduleByPath(validProgram, valid / "lib.cd");
    assert(validDependency->statements.empty());

    const fs::path missingSidecar = root / "missing_sidecar";
    fs::remove_all(missingSidecar);
    writeSingleModuleCacheCase(missingSidecar, validSource);
    fs::create_directories(missingSidecar / "cache");
    expectImportError(
        [&]() { (void)loadStrict(missingSidecar); },
        expectedError(missingSidecar / "lib.cd", "missing sidecar"));

    const fs::path malformedSidecar = root / "malformed_sidecar";
    fs::remove_all(malformedSidecar);
    writeSingleModuleCacheCase(malformedSidecar, validSource);
    (void)writeSingleModuleSidecar(malformedSidecar, validSource);
    writeFile(
        moduleInterfaceArtifactPath(
            malformedSidecar / "cache",
            pathString(fs::weakly_canonical(malformedSidecar / "lib.cd"))),
        "cdi 9.9\n");
    expectImportError(
        [&]() { (void)loadStrict(malformedSidecar); },
        expectedError(malformedSidecar / "lib.cd", "malformed sidecar"));

    const fs::path changedSource = root / "changed_source";
    fs::remove_all(changedSource);
    writeSingleModuleCacheCase(changedSource, validSource);
    (void)writeSingleModuleSidecar(changedSource, validSource);
    writeFile(changedSource / "lib.cd", "let value = 8;\nexport value;\n");
    expectImportError(
        [&]() { (void)loadStrict(changedSource); },
        expectedError(changedSource / "lib.cd", "source hash mismatch"));

    const fs::path missingProduct = root / "missing_product";
    fs::remove_all(missingProduct);
    writeSingleModuleCacheCase(missingProduct, validSource);
    const std::string missingProductHash = writeSingleModuleSidecar(missingProduct, validSource);
    ModuleCacheModule missingProductModule;
    missingProductModule.identity = pathString(fs::weakly_canonical(missingProduct / "lib.cd"));
    missingProductModule.sourceHash = moduleCacheHash(validSource);
    missingProductModule.interfaceHash = missingProductHash;
    assert(fs::remove(missingProduct / "cache" / moduleCacheArtifactPath(missingProductModule)));
    expectImportError(
        [&]() { (void)loadStrict(missingProduct); },
        expectedError(missingProduct / "lib.cd", "missing paired product"));

    const fs::path identityMismatch = root / "identity_mismatch";
    fs::remove_all(identityMismatch);
    writeSingleModuleCacheCase(identityMismatch, validSource);
    (void)writeSingleModuleSidecar(identityMismatch, validSource);
    const fs::path identitySidecar = moduleInterfaceArtifactPath(
        identityMismatch / "cache",
        pathString(fs::weakly_canonical(identityMismatch / "lib.cd")));
    std::string identityText = readFile(identitySidecar);
    const std::string identity = pathString(fs::weakly_canonical(identityMismatch / "lib.cd"));
    const std::string wrongIdentity = pathString(fs::weakly_canonical(identityMismatch / "other.cd"));
    replaceText(identityText, "identity = \"" + identity + "\"", "identity = \"" + wrongIdentity + "\"");
    replaceText(
        identityText,
        "canonical_path = \"" + identity + "\"",
        "canonical_path = \"" + wrongIdentity + "\"");
    writeFile(identitySidecar, identityText);
    expectImportError(
        [&]() { (void)loadStrict(identityMismatch); },
        expectedError(identityMismatch / "lib.cd", "identity/canonical path mismatch"));

    const fs::path dependencyMismatch = root / "dependency_mismatch";
    fs::remove_all(dependencyMismatch);
    const fs::path base = dependencyMismatch / "base.cd";
    const fs::path library = dependencyMismatch / "lib.cd";
    writeFile(base, "let baseValue = 3;\nexport baseValue;\n");
    const std::string librarySource =
        "import \"./base.cd\";\n"
        "let value = 7;\n"
        "export value;\n";
    writeFile(library, librarySource);
    writeFile(dependencyMismatch / "entry.cd", "import \"./lib.cd\";\nprint value;\n");

    ModuleInterface baseInterface;
    baseInterface.values.push_back(ModuleInterfaceValue{
        "baseValue",
        simpleType(StaticType::Number),
        "baseValue#14",
    });
    const std::string baseHash = writeCachedModule(
        dependencyMismatch / "cache",
        base,
        readFile(base),
        baseInterface,
        {});

    ModuleInterface libraryInterface;
    libraryInterface.values.push_back(ModuleInterfaceValue{
        "value",
        simpleType(StaticType::Number),
        "value#15",
    });
    writeCachedModule(
        dependencyMismatch / "cache",
        library,
        librarySource,
        libraryInterface,
        {ModuleInterfaceArtifactDependency{
            pathString(fs::weakly_canonical(base)),
            ModuleGraphEdgeKind::Import,
            "./base.cd",
            baseHash + "-stale",
        }});
    expectImportError(
        [&]() { (void)loadStrict(dependencyMismatch); },
        expectedError(library, "dependency interface hash mismatch"));
}

void test_module_interface_cache_fallback_preserves_parse_diagnostics(const fs::path& root)
{
    fs::remove_all(root);
    const std::string validSource =
        "let value = 7;\n"
        "export value;\n";
    writeSingleModuleCacheCase(root, validSource);
    (void)writeSingleModuleSidecar(root, validSource);
    const std::string invalidSource =
        "let value = ;\n"
        "export value;\n";
    writeFile(root / "lib.cd", invalidSource);

    try {
        FrontendSession session;
        session.setModuleInterfaceCacheDirectory(root / "cache");
        (void)session.loadFiles({(root / "entry.cd").string()});
    } catch (const FileDiagnosticErrorList& errors) {
        assert(errors.errors().size() == 1);
        const FileDiagnosticError& error = errors.errors().front();
        assert(error.kind() == DiagnosticKind::Parse);
        assert(error.sourceContext().path.find("lib.cd") != std::string::npos);
        assert(error.range().has_value());
        return;
    }
    assert(false && "expected stale-sidecar source parse diagnostic");
}

void test_direct_inputs_preserve_source_spans(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path first = root / "first.cd";
    const fs::path second = root / "second.cd";
    writeFile(first, "print 1;\n");
    writeFile(second, "print 2;\n");

    FrontendSession session;
    Program program = session.loadFiles({first.string(), second.string()});

    assert(!program.moduleGraph.has_value());
    assert(program.sources.size() == 2);
    assert(program.sources[0].path.find("first.cd") != std::string::npos);
    assert(program.sources[1].path.find("second.cd") != std::string::npos);
    assert(!program.sources[0].moduleIdentity);
    assert(!program.sources[1].moduleIdentity);
    const auto* firstPrint = dynamic_cast<const PrintStmt*>(program.statements.front().get());
    assert(firstPrint != nullptr);
    assert(firstPrint->span.has_value());
    assert(firstPrint->span->source == 0);
    assert(firstPrint->span->line == 1);
}

void test_direct_diagnostics_keep_source_ranges(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path first = root / "first.cd";
    const fs::path second = root / "second.cd";
    writeFile(first, "print 1;\n");
    writeFile(second, "print ;\n");

    FrontendSession session;
    try {
        (void)session.loadFiles({first.string(), second.string()});
    } catch (const FileDiagnosticErrorList& errors) {
        assert(errors.errors().size() == 1);
        const FileDiagnosticError& error = errors.errors().front();
        assert(error.sourceContext().path.find("second.cd") != std::string::npos);
        assert(error.range().has_value());
        assert(error.range()->source == SourceFileId{1});
        assert(error.range()->start <= error.range()->end);
        assert(error.range()->end <= error.sourceContext().source.size());
        return;
    }
    assert(false && "expected direct multi-file parse diagnostic");
}

void test_module_type_error_recovery(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path firstFailure = root / "a.cd";
    const fs::path secondFailure = root / "b.cd";
    const fs::path blocked = root / "blocked.cd";
    const fs::path independent = root / "ok.cd";
    const fs::path entry = root / "entry.cd";
    writeFile(
        firstFailure,
        "print missing_a_first;\n"
        "print missing_a_second;\n");
    writeFile(secondFailure, "print missing_b;\n");
    writeFile(
        blocked,
        "import \"./a.cd\";\n"
        "print missing_blocked;\n");
    writeFile(
        independent,
        "let good = 1;\n"
        "export good;\n");
    writeFile(
        entry,
        "import \"./a.cd\";\n"
        "import \"./b.cd\";\n"
        "import \"./blocked.cd\";\n"
        "import \"./ok.cd\";\n"
        "print missing_entry;\n");

    FrontendSession frontend;
    Program program = frontend.loadFiles({entry.string()});
    const ModuleStmt* independentModule = moduleByPath(program, independent);

    TypeChecker checker;
    try {
        checker.check(program);
    } catch (const TypeErrorList& errors) {
        assert(errors.errors().size() == 2);
        assert(errors.errors()[0].sourceContext().path == pathString(firstFailure));
        assert(errors.errors()[0].message() == "undefined variable `missing_a_first`");
        assert(errors.errors()[1].sourceContext().path == pathString(secondFailure));
        assert(errors.errors()[1].message() == "undefined variable `missing_b`");
        assert(checker.checkedModuleBodyIds() == std::vector<std::size_t>{independentModule->moduleId});
        assert(checker.moduleInterfaces().size() == 1);
        assert(checker.moduleInterfaces().front().moduleId == independentModule->moduleId);
        fs::remove_all(root);
        return;
    }

    assert(false && "expected independent module type diagnostics");
}

void assertLosslessFile(
    const LosslessSourceFileView& view,
    const SourceFile& source,
    std::size_t expectedComments)
{
    assert(view.sourceId() == source.id);
    assert(view.roundTrips(source.text));

    std::size_t cursor = 0;
    std::size_t commentCount = 0;
    for (const LosslessPiece& piece : view.pieces()) {
        assert(piece.range.source == source.id);
        assert(piece.range.start == cursor);
        assert(piece.range.start <= piece.range.end);
        assert(piece.range.end <= source.text.size());
        assert(piece.text == source.text.substr(
            piece.range.start,
            piece.range.end - piece.range.start));
        if (piece.isToken()) {
            assert(piece.token.has_value());
            assert(!piece.triviaKind.has_value());
        } else {
            assert(!piece.token.has_value());
            assert(piece.triviaKind.has_value());
            if (*piece.triviaKind == TriviaKind::LineComment) {
                ++commentCount;
            }
        }
        cursor = piece.range.end;
    }
    assert(cursor == source.text.size());
    assert(commentCount == expectedComments);
}

void test_lossless_source_view_round_trips_comments(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path first = root / "first.cd";
    const fs::path second = root / "second.cd";
    const std::string firstSource =
        "// header\n"
        "let value = 1; // trailing\n"
        "\n"
        "print value;\n";
    const std::string secondSource =
        "print \"// not a comment\"; // second file\n";
    writeFile(first, firstSource);
    writeFile(second, secondSource);

    FrontendSession session;
    Program program = session.loadFiles({first.string(), second.string()});
    const LosslessSourceView view = session.losslessSourceView();
    assert(view.files().size() == 2);
    assertLosslessFile(view.file(SourceFileId{0}), program.sources[0], 2);
    assertLosslessFile(view.file(SourceFileId{1}), program.sources[1], 1);
}

} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "compiler_frontend_session_test";

    test_canonical_duplicate_import_spellings_are_deduplicated(root / "duplicates");
    test_search_path_resolves_extensionless_import_and_reexport(root / "search_reexport");
    test_importing_file_directory_precedes_search_path(root / "precedence");
    test_explicit_relative_import_does_not_use_search_path(root / "explicit_no_fallback");
    test_module_interface_cache_hit_reuses_dependency_interfaces(root / "module_interface_cache");
    test_module_interface_cache_fallbacks(root / "module_interface_cache_fallbacks");
    test_module_interface_cache_dependency_hash_fallback(root / "module_interface_cache_dependency_hash");
    test_module_interface_cache_strict(root / "module_interface_cache_strict");
    test_module_interface_cache_fallback_preserves_parse_diagnostics(
        root / "module_interface_cache_diagnostics");
    test_direct_inputs_preserve_source_spans(root / "direct_sources");
    test_direct_diagnostics_keep_source_ranges(root / "direct_diagnostics");
    test_module_type_error_recovery(root / "module_type_error_recovery");
    test_lossless_source_view_round_trips_comments(root / "lossless_sources");

    fs::remove_all(root);
}
