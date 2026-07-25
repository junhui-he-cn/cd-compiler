#pragma once

#include "Ast.hpp"
#include "Diagnostic.hpp"
#include "LosslessSource.hpp"
#include "ModuleGraph.hpp"
#include "Parser.hpp"
#include "Token.hpp"

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class FrontendSession {
public:
    void setImportSearchPaths(std::vector<std::string> paths);

    Program loadStdin(std::istream& input);
    Program loadFiles(const std::vector<std::string>& paths);

    std::vector<Token> displayTokens() const;
    LosslessSourceView losslessSourceView() const;
    const std::string& sourceForDiagnostics() const;
    std::optional<FileDiagnosticError> remapDirectDiagnostic(const DiagnosticError& error) const;
    std::optional<FileDiagnosticErrorList> remapDirectDiagnostics(const ParseErrorList& errors) const;
    std::size_t moduleCount() const;
    const ModuleGraph& moduleGraph() const;

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
    };

    struct DirectInput {
        std::size_t sourceId = 0;
        std::string path;
        std::string canonicalPath;
        std::string source;
        std::size_t combinedStartOffset = 0;
    };

    struct ImportResolution {
        std::filesystem::path path;
        std::vector<std::string> triedDisplayPaths;
    };

    void reset();
    std::size_t loadFile(const std::string& path, bool isImport, bool isEntry, bool fileDiagnostics);
    ImportResolution resolveImportPath(const std::filesystem::path& importingPath, const Token& pathToken) const;
    Program assembleProgram();
    void rebuildModuleGraph();
    void rebuildCombinedSource();
    void annotateSourceTokens(std::vector<Token>& tokens, std::size_t sourceId) const;
    void annotateDirectTokens(std::vector<Token>& tokens) const;

    std::vector<ParsedUnit> units_;
    std::unordered_map<std::string, std::size_t> canonicalToUnitId_;
    std::vector<std::string> loadingStack_;
    std::vector<std::size_t> entryUnitIds_;
    std::vector<DirectInput> directInputs_;
    std::vector<SourceFile> sourceFiles_;
    std::vector<int> directSourceLineStarts_;
    std::vector<Token> directDisplayTokens_;
    std::vector<std::filesystem::path> importSearchPaths_;
    std::string combinedSource_;
    ModuleGraph moduleGraph_;
    bool hasImports_ = false;
};
