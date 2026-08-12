#pragma once

#include "Ast.hpp"
#include "Diagnostic.hpp"
#include "LosslessSource.hpp"
#include "ModuleCache.hpp"
#include "ModuleGraph.hpp"
#include "ModuleInterfaceArtifact.hpp"
#include "Parser.hpp"
#include "Token.hpp"

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class LexErrorList;

struct FrontendVirtualFile {
    std::string path;
    std::string source;
};

class FrontendSession {
public:
    void setImportSearchPaths(std::vector<std::string> paths);
    // The path is the module-cache root.  Sidecars are read from its
    // `interfaces/` directory and only trusted when their cached product is
    // also present.
    void setModuleInterfaceCacheDirectory(std::filesystem::path path);
    // In strict mode, an imported sidecar that cannot be trusted is a stable
    // Import diagnostic instead of a source fallback. Entry modules always
    // use their source path.
    void setModuleInterfaceCacheStrict(bool strict);
    // Module-product emission requires a valid cdbc-cache manifest record in
    // addition to the sidecar and paired product before it can preload an
    // imported body. Interface-only consumers intentionally do not use this
    // stronger manifest boundary.
    void setModuleProductCacheMode(bool enabled);

    Program loadStdin(std::istream& input);
    Program loadFiles(const std::vector<std::string>& paths);
    Program loadVirtualFiles(const std::vector<FrontendVirtualFile>& files);
    // Virtual workspaces may opt into bounded disk imports.  Open virtual
    // sources still take precedence over disk files at the same path.
    void setVirtualImportRoots(std::vector<std::string> paths);

    std::vector<Token> displayTokens() const;
    LosslessSourceView losslessSourceView() const;
    std::size_t moduleCount() const;
    const ModuleGraph& moduleGraph() const;
    const std::vector<ModuleInterface>& preloadedModuleInterfaces() const;

private:
    struct ParsedUnit {
        std::size_t id = 0;
        std::size_t sourceId = 0;
        std::string path;
        std::string canonicalPath;
        std::string source;
        std::vector<Token> tokens;
        std::vector<StmtPtr> statements;
        bool isEntry = false;
        std::optional<ModuleInterfaceArtifact> interfaceArtifact;
    };

    struct ImportResolution {
        std::filesystem::path path;
        std::vector<std::string> triedDisplayPaths;
    };

    struct CachedInterfaceLoad {
        std::optional<ModuleInterfaceArtifact> artifact;
        std::string rejectionReason;
    };

    void reset();
    std::size_t loadFile(const std::string& path, bool isImport, bool isEntry);
    CachedInterfaceLoad loadCachedInterface(
        const std::string& canonicalPath,
        const std::string& source) const;
    std::string moduleProductCacheRejection(
        const std::string& canonicalPath,
        const ModuleInterfaceArtifact& artifact) const;
    ImportResolution resolveImportPath(const std::filesystem::path& importingPath, const Token& pathToken) const;
    Program assembleProgram();
    void rebuildModuleGraph();
    void rebuildPreloadedModuleInterfaces();

    std::vector<ParsedUnit> units_;
    std::unordered_map<std::string, std::size_t> canonicalToUnitId_;
    std::vector<std::string> loadingStack_;
    std::vector<SourceFile> sourceFiles_;
    std::vector<std::filesystem::path> importSearchPaths_;
    std::optional<std::filesystem::path> moduleInterfaceCacheDirectory_;
    bool moduleInterfaceCacheStrict_ = false;
    bool moduleProductCacheMode_ = false;
    mutable std::optional<ModuleCacheLoadResult> moduleProductCacheLoad_;
    std::vector<std::filesystem::path> virtualImportRoots_;
    std::unordered_set<std::string> directEntryCanonicalPaths_;
    ModuleGraph moduleGraph_;
    std::vector<ModuleInterface> preloadedModuleInterfaces_;
    std::unordered_map<std::string, std::string> virtualSources_;
    bool virtualSourceMode_ = false;
};
