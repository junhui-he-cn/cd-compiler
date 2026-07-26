#include "FrontendSession.hpp"

#include "Diagnostic.hpp"
#include "Lexer.hpp"
#include "LosslessSource.hpp"
#include "ModuleCache.hpp"
#include "Parser.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace {

std::string readAll(std::istream& input)
{
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::filesystem::path normalizedExistingPath(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    if (!error) {
        return canonical.lexically_normal();
    }
    return std::filesystem::absolute(path).lexically_normal();
}

std::string pathString(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

std::string sourceMetadataPath(const std::string& displayPath)
{
    const std::filesystem::path path(displayPath);
    if (!path.is_absolute()) {
        return displayPath;
    }

    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(
        path,
        std::filesystem::current_path(error),
        error);
    if (!error && !relative.empty()) {
        return pathString(relative);
    }
    return displayPath;
}

std::string displayCycle(const std::vector<std::string>& stack, const std::string& repeated)
{
    const auto found = std::find(stack.begin(), stack.end(), repeated);
    std::ostringstream output;
    if (found == stack.end()) {
        output << repeated << " -> " << repeated;
        return output.str();
    }

    for (auto current = found; current != stack.end(); ++current) {
        if (current != found) {
            output << " -> ";
        }
        output << *current;
    }
    output << " -> " << repeated;
    return output.str();
}

void appendWithNewlineSeparation(std::string& output, const std::string& source)
{
    if (!output.empty() && output.back() != '\n') {
        output.push_back('\n');
    }
    output += source;
    if (!output.empty() && output.back() != '\n') {
        output.push_back('\n');
    }
}

bool hasImportToken(const std::vector<Token>& tokens)
{
    return std::any_of(tokens.begin(), tokens.end(), [](const Token& token) {
        return token.type == TokenType::Import;
    });
}

bool statementLoadsSource(const Stmt& statement)
{
    if (dynamic_cast<const ImportStmt*>(&statement)) {
        return true;
    }
    const auto* exportStmt = dynamic_cast<const ExportStmt*>(&statement);
    return exportStmt && exportStmt->sourcePath.has_value();
}

bool programLoadsSource(const Program& program)
{
    return std::any_of(program.statements.begin(), program.statements.end(), [](const StmtPtr& statement) {
        return statementLoadsSource(*statement);
    });
}

std::string importPath(const Token& token)
{
    if (token.lexeme.size() >= 2 && token.lexeme.front() == '"' && token.lexeme.back() == '"') {
        return token.lexeme.substr(1, token.lexeme.size() - 2);
    }
    return token.lexeme;
}

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

bool isExplicitImportPath(const std::string& value)
{
    const std::filesystem::path path(value);
    return path.is_absolute() || startsWith(value, "./") || startsWith(value, "../");
}

bool canOpenFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return input.good();
}

std::vector<std::filesystem::path> importCandidatesForBase(
    const std::filesystem::path& base,
    const std::filesystem::path& requested)
{
    std::filesystem::path raw = requested.is_absolute()
        ? requested
        : base / requested;
    raw = raw.lexically_normal();

    std::vector<std::filesystem::path> candidates;
    candidates.push_back(raw);
    if (requested.extension().empty()) {
        std::filesystem::path withCdExtension = raw;
        withCdExtension += ".cd";
        candidates.push_back(withCdExtension.lexically_normal());
    }
    return candidates;
}

std::string joinDisplayPaths(const std::vector<std::string>& paths)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << paths[index];
    }
    return output.str();
}

struct ParsedSource {
    std::vector<Token> tokens;
    std::vector<StmtPtr> statements;
};


FileDiagnosticError fileDiagnosticFromError(
    const DiagnosticError& error,
    const std::string& path,
    const std::string& source,
    bool pathlessDiagnostics)
{
    return FileDiagnosticError(
        error,
        DiagnosticSourceContext{path, source, pathlessDiagnostics});
}

FileDiagnosticErrorList fileDiagnosticListFromParseErrors(
    const ParseErrorList& errors,
    const std::string& path,
    const std::string& source,
    bool pathlessDiagnostics)
{
    std::vector<FileDiagnosticError> mapped;
    for (const ParseError& error : errors.errors()) {
        mapped.push_back(fileDiagnosticFromError(error, path, source, pathlessDiagnostics));
    }
    return FileDiagnosticErrorList(std::move(mapped));
}

ParsedSource parseSource(
    const std::string& path,
    const std::string& source,
    bool pathlessDiagnostics,
    std::optional<std::size_t> sourceId = std::nullopt)
{
    try {
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.scanTokens();
        if (sourceId) {
            for (Token& token : tokens) {
                token.source = sourceId;
                token.sourceLine = token.line;
                token.sourceId = SourceFileId{*sourceId};
                token.range = SourceRange{*token.sourceId, token.startOffset, token.endOffset};
            }
        }
        Parser parser(tokens);
        Program program = parser.parse();
        return ParsedSource{std::move(tokens), std::move(program.statements)};
    } catch (const ParseErrorList& errors) {
        throw fileDiagnosticListFromParseErrors(errors, path, source, pathlessDiagnostics);
    } catch (const FileDiagnosticError&) {
        throw;
    } catch (const DiagnosticError& error) {
        if (error.location()) {
            throw FileDiagnosticError(
                error,
                DiagnosticSourceContext{path, source, pathlessDiagnostics});
        }
        throw;
    }
}

int lineAtEnd(const std::string& source)
{
    int line = 1;
    for (const char ch : source) {
        if (ch == '\n') {
            ++line;
        }
    }
    return line;
}

Token endOfFileToken(const std::string& source)
{
    int line = 1;
    int column = 1;
    for (const char ch : source) {
        if (ch == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    Token token{TokenType::EndOfFile, "", line, column};
    token.startOffset = source.size();
    token.endOffset = source.size();
    return token;
}

std::size_t sourceLineSpan(const std::string& source)
{
    if (source.empty()) {
        return 0;
    }

    std::size_t lines = 1;
    for (const char ch : source) {
        if (ch == '\n') {
            ++lines;
        }
    }
    if (source.back() == '\n') {
        --lines;
    }
    return lines;
}

} // namespace

void FrontendSession::reset()
{
    units_.clear();
    canonicalToUnitId_.clear();
    loadingStack_.clear();
    entryUnitIds_.clear();
    directInputs_.clear();
    sourceFiles_.clear();
    directSourceLineStarts_.clear();
    directDisplayTokens_.clear();
    directEntryCanonicalPaths_.clear();
    combinedSource_.clear();
    moduleGraph_ = ModuleGraph{};
    preloadedModuleInterfaces_.clear();
    hasImports_ = false;
}

void FrontendSession::setImportSearchPaths(std::vector<std::string> paths)
{
    importSearchPaths_.clear();
    for (const std::string& path : paths) {
        importSearchPaths_.push_back(std::filesystem::path(path).lexically_normal());
    }
}

void FrontendSession::setModuleInterfaceCacheDirectory(std::filesystem::path path)
{
    moduleInterfaceCacheDirectory_ = path.lexically_normal();
}

const std::vector<ModuleInterface>& FrontendSession::preloadedModuleInterfaces() const
{
    return preloadedModuleInterfaces_;
}

std::optional<ModuleInterfaceArtifact> FrontendSession::loadCachedInterface(
    const std::string& canonicalPath,
    const std::string& source) const
{
    if (!moduleInterfaceCacheDirectory_) {
        return std::nullopt;
    }

    const ModuleInterfaceArtifactLoadResult loaded = readModuleInterfaceArtifact(
        moduleInterfaceArtifactPath(*moduleInterfaceCacheDirectory_, canonicalPath));
    if (!loaded.artifact) {
        return std::nullopt;
    }
    const ModuleInterfaceArtifact& artifact = *loaded.artifact;
    if (artifact.identity != canonicalPath
        || artifact.canonicalPath != canonicalPath
        || artifact.sourceHash != moduleCacheHash(source)) {
        return std::nullopt;
    }

    ModuleCacheModule module;
    module.identity = artifact.identity;
    module.sourceHash = artifact.sourceHash;
    module.interfaceHash = artifact.interfaceHash;
    module.isEntry = artifact.isEntry;
    module.entryOrder = artifact.entryOrder;
    for (const ModuleInterfaceArtifactDependency& dependency : artifact.dependencies) {
        module.dependencies.push_back(ModuleCacheDependency{
            dependency.identity,
            dependency.kind,
            dependency.requestedPath,
            dependency.interfaceHash});
    }
    const std::filesystem::path product = *moduleInterfaceCacheDirectory_
        / moduleCacheArtifactPath(module);
    if (!std::filesystem::exists(product)) {
        return std::nullopt;
    }
    return artifact;
}

void FrontendSession::annotateSourceTokens(std::vector<Token>& tokens, std::size_t sourceId) const
{
    for (Token& token : tokens) {
        token.source = sourceId;
        token.sourceLine = token.line;
        token.sourceId = SourceFileId{sourceId};
        token.range = SourceRange{*token.sourceId, token.startOffset, token.endOffset};
    }
}

void FrontendSession::annotateDirectTokens(std::vector<Token>& tokens) const
{
    for (Token& token : tokens) {
        std::size_t sourceIndex = directInputs_.empty() ? 0 : directInputs_.size() - 1;
        int sourceStart = 1;
        for (std::size_t index = 0; index < directSourceLineStarts_.size(); ++index) {
            if (token.line >= directSourceLineStarts_[index]) {
                sourceIndex = index;
                sourceStart = directSourceLineStarts_[index];
            } else {
                break;
            }
        }
        if (sourceIndex < directInputs_.size()) {
            const DirectInput& input = directInputs_[sourceIndex];
            token.source = input.sourceId;
            token.sourceLine = token.line - sourceStart + 1;
            token.sourceId = SourceFileId{input.sourceId};
            const std::size_t localStart = token.startOffset < input.combinedStartOffset
                ? 0
                : token.startOffset - input.combinedStartOffset;
            const std::size_t localEnd = token.endOffset < input.combinedStartOffset
                ? 0
                : token.endOffset - input.combinedStartOffset;
            token.range = SourceRange{
                *token.sourceId,
                std::min(localStart, input.source.size()),
                std::min(std::max(localEnd, localStart), input.source.size())};
        }
    }
}

FrontendSession::ImportResolution FrontendSession::resolveImportPath(
    const std::filesystem::path& importingPath,
    const Token& pathToken) const
{
    const std::string requestedText = importPath(pathToken);
    const std::filesystem::path requestedPath(requestedText);
    const bool explicitPath = isExplicitImportPath(requestedText);

    std::vector<std::filesystem::path> bases;
    if (requestedPath.is_absolute()) {
        bases.emplace_back();
    } else {
        bases.push_back(importingPath.parent_path());
    }
    if (!explicitPath) {
        bases.insert(bases.end(), importSearchPaths_.begin(), importSearchPaths_.end());
    }

    std::vector<std::string> triedDisplayPaths;
    for (const std::filesystem::path& base : bases) {
        for (const std::filesystem::path& candidate : importCandidatesForBase(base, requestedPath)) {
            const std::string displayPath = pathString(candidate);
            triedDisplayPaths.push_back(displayPath);
            if (canOpenFile(candidate)) {
                return ImportResolution{candidate, std::move(triedDisplayPaths)};
            }
        }
    }

    if (explicitPath) {
        const std::string displayPath = triedDisplayPaths.empty()
            ? requestedText
            : triedDisplayPaths.front();
        throw DiagnosticError(DiagnosticKind::Import, "failed to open import: " + displayPath);
    }

    throw DiagnosticError(
        DiagnosticKind::Import,
        "failed to resolve import `" + requestedText + "`; tried: " + joinDisplayPaths(triedDisplayPaths));
}

Program FrontendSession::loadStdin(std::istream& input)
{
    reset();

    std::string source = readAll(input);
    sourceFiles_.push_back(SourceFile{"<stdin>", source, SourceFileId{0}});
    ParsedSource parsed = parseSource("<stdin>", source, true, 0);
    if (hasImportToken(parsed.tokens)) {
        throw DiagnosticError(DiagnosticKind::Import, "import is not supported from stdin");
    }

    Program parsedProgram;
    parsedProgram.statements = std::move(parsed.statements);
    if (programLoadsSource(parsedProgram)) {
        throw DiagnosticError(DiagnosticKind::Import, "import is not supported from stdin");
    }

    units_.push_back(ParsedUnit{
        0,
        0,
        "<stdin>",
        "<stdin>",
        std::move(source),
        std::move(parsed.tokens),
        std::move(parsedProgram.statements),
        true,
        std::nullopt,
    });
    entryUnitIds_.push_back(0);
    rebuildCombinedSource();
    Program program = assembleProgram();
    program.sources = sourceFiles_;
    return program;
}

Program FrontendSession::loadFiles(const std::vector<std::string>& paths)
{
    reset();

    std::unordered_map<std::string, std::size_t> directInputIds;
    bool hasImports = false;
    for (const std::string& path : paths) {
        directEntryCanonicalPaths_.insert(pathString(normalizedExistingPath(path)));
    }
    for (const std::string& path : paths) {
        const std::filesystem::path requestedPath(path);
        const std::filesystem::path normalizedPath = normalizedExistingPath(requestedPath);
        const std::string canonicalPath = pathString(normalizedPath);
        if (directInputIds.find(canonicalPath) != directInputIds.end()) {
            continue;
        }

        const std::string displayPath = pathString(requestedPath);
        std::ifstream input(requestedPath);
        if (!input) {
            throw std::runtime_error("failed to open input file: " + displayPath);
        }

        std::string source = readAll(input);
        const std::size_t sourceId = sourceFiles_.size();
        sourceFiles_.push_back(SourceFile{
            sourceMetadataPath(displayPath),
            source,
            SourceFileId{sourceId}});
        directInputIds.emplace(canonicalPath, directInputs_.size());
        directInputs_.push_back(DirectInput{
            sourceId,
            displayPath,
            canonicalPath,
            std::move(source),
        });
    }

    for (std::size_t index = 0; index < directInputs_.size(); ++index) {
        DirectInput& input = directInputs_[index];
        int sourceStart = lineAtEnd(combinedSource_);
        if (!combinedSource_.empty() && combinedSource_.back() != '\n') {
            ++sourceStart;
        }
        directSourceLineStarts_.push_back(sourceStart);
        if (!combinedSource_.empty() && combinedSource_.back() != '\n') {
            combinedSource_.push_back('\n');
        }
        input.combinedStartOffset = combinedSource_.size();
        combinedSource_ += input.source;
        if (!combinedSource_.empty() && combinedSource_.back() != '\n') {
            combinedSource_.push_back('\n');
        }
    }
    Program directProgram;
    try {
        Lexer lexer(combinedSource_);
        directDisplayTokens_ = lexer.scanTokensUntil(TokenType::Import);
        hasImports = !directDisplayTokens_.empty()
            && directDisplayTokens_.back().type == TokenType::Import;
        if (!hasImports) {
            annotateDirectTokens(directDisplayTokens_);
            Parser parser(directDisplayTokens_);
            directProgram = parser.parse();
            hasImports = programLoadsSource(directProgram);
        }
    } catch (const ParseErrorList& errors) {
        if (const std::optional<FileDiagnosticErrorList> remapped = remapDirectDiagnostics(errors)) {
            throw *remapped;
        }
        throw;
    } catch (const DiagnosticError& error) {
        if (const std::optional<FileDiagnosticError> remapped = remapDirectDiagnostic(error)) {
            throw *remapped;
        }
        throw;
    }

    if (!hasImports) {
        directProgram.sources = sourceFiles_;
        finalizeSyntaxMetadata(directProgram);
        return directProgram;
    }

    directInputs_.clear();
    directDisplayTokens_.clear();
    sourceFiles_.clear();
    directSourceLineStarts_.clear();
    for (const std::string& path : paths) {
        const std::size_t id = loadFile(path, false, true, true);
        if (std::find(entryUnitIds_.begin(), entryUnitIds_.end(), id) == entryUnitIds_.end()) {
            entryUnitIds_.push_back(id);
        }
    }
    rebuildModuleGraph();
    rebuildCombinedSource();
    return assembleProgram();
}

std::size_t FrontendSession::loadFile(
    const std::string& path,
    bool isImport,
    bool isEntry,
    bool fileDiagnostics)
{
    const std::filesystem::path requestedPath(path);
    const std::filesystem::path normalizedPath = normalizedExistingPath(requestedPath);
    const std::string canonicalPath = pathString(normalizedPath);
    const std::string displayPath = pathString(requestedPath);

    if (std::find(loadingStack_.begin(), loadingStack_.end(), canonicalPath) != loadingStack_.end()) {
        throw DiagnosticError(
            DiagnosticKind::Import,
            "import cycle detected: " + displayCycle(loadingStack_, canonicalPath));
    }

    const auto loaded = canonicalToUnitId_.find(canonicalPath);
    if (loaded != canonicalToUnitId_.end()) {
        if (isEntry) {
            units_[loaded->second].isEntry = true;
        }
        return loaded->second;
    }

    std::ifstream input(requestedPath);
    if (!input) {
        if (isImport) {
            throw DiagnosticError(DiagnosticKind::Import, "failed to open import: " + displayPath);
        }
        throw std::runtime_error("failed to open input file: " + displayPath);
    }

    loadingStack_.push_back(canonicalPath);
    try {
        std::string source = readAll(input);
        if (isImport && directEntryCanonicalPaths_.find(canonicalPath) == directEntryCanonicalPaths_.end()) {
            if (std::optional<ModuleInterfaceArtifact> cached
                = loadCachedInterface(canonicalPath, source)) {
                hasImports_ = true;
                bool dependenciesCached = true;
                for (const ModuleInterfaceArtifactDependency& dependency : cached->dependencies) {
                    const std::size_t dependencyId = loadFile(dependency.identity, true, false, true);
                    const ParsedUnit& dependencyUnit = units_.at(dependencyId);
                    if (!dependencyUnit.interfaceArtifact
                        || dependencyUnit.interfaceArtifact->interfaceHash != dependency.interfaceHash) {
                        dependenciesCached = false;
                    }
                }
                if (!dependenciesCached) {
                    // A sidecar with a source-parsed or stale dependency cannot
                    // safely stand in for the dependency graph.  Continue into
                    // the ordinary parser below; the already loaded dependency
                    // units remain valid and are de-duplicated as before.
                } else {
                    const std::size_t sourceId = sourceFiles_.size();
                    sourceFiles_.push_back(SourceFile{
                        cached->path,
                        source,
                        SourceFileId{sourceId}});

                    ParsedUnit unit{
                        0,
                        sourceId,
                        cached->path,
                        canonicalPath,
                        std::move(source),
                        {},
                        {},
                        false,
                        std::move(cached),
                    };
                    unit.id = units_.size();
                    units_.push_back(std::move(unit));
                    canonicalToUnitId_.emplace(canonicalPath, units_.back().id);
                    loadingStack_.pop_back();
                    return units_.back().id;
                }
            }
        }

        const std::size_t sourceId = sourceFiles_.size();
        sourceFiles_.push_back(SourceFile{
            sourceMetadataPath(displayPath),
            source,
            SourceFileId{sourceId}});
        std::vector<Token> tokens;
        try {
            Lexer lexer(source);
            tokens = lexer.scanTokens();
            annotateSourceTokens(tokens, sourceId);
        } catch (const DiagnosticError& error) {
            if (error.location()) {
                throw FileDiagnosticError(
                    error,
                    DiagnosticSourceContext{displayPath, source, !fileDiagnostics && !isImport});
            }
            throw;
        }
        const bool thisUnitHasImport = hasImportToken(tokens);
        ParsedSource parsed;
        try {
            Parser parser(tokens);
            Program program = parser.parse();
            parsed = ParsedSource{std::move(tokens), std::move(program.statements)};
        } catch (const ParseErrorList& errors) {
            throw fileDiagnosticListFromParseErrors(
                errors,
                displayPath,
                source,
                !fileDiagnostics && !isImport && !thisUnitHasImport);
        } catch (const FileDiagnosticError&) {
            throw;
        } catch (const DiagnosticError& error) {
            if (error.location()) {
                throw FileDiagnosticError(
                    error,
                    DiagnosticSourceContext{
                        displayPath,
                        source,
                        !fileDiagnostics && !isImport && !thisUnitHasImport,
                    });
            }
            throw;
        }

        ParsedUnit unit{
            0,
            sourceId,
            displayPath,
            canonicalPath,
            std::move(source),
            std::move(parsed.tokens),
            std::move(parsed.statements),
            isEntry,
            std::nullopt,
        };

        for (StmtPtr& statement : unit.statements) {
            if (auto* import = dynamic_cast<ImportStmt*>(statement.get())) {
                hasImports_ = true;
                const ImportResolution resolution = resolveImportPath(normalizedPath, import->path);
                import->resolvedModuleId = loadFile(resolution.path.string(), true, false, true);
                continue;
            }

            auto* exportStmt = dynamic_cast<ExportStmt*>(statement.get());
            if (!exportStmt || !exportStmt->sourcePath) {
                continue;
            }
            hasImports_ = true;
            const ImportResolution resolution = resolveImportPath(normalizedPath, *exportStmt->sourcePath);
            exportStmt->resolvedModuleId = loadFile(resolution.path.string(), true, false, true);
        }

        unit.id = units_.size();
        units_.push_back(std::move(unit));
        canonicalToUnitId_.emplace(canonicalPath, units_.back().id);
        loadingStack_.pop_back();
        return units_.back().id;
    } catch (...) {
        loadingStack_.pop_back();
        throw;
    }
}

Program FrontendSession::assembleProgram()
{
    Program program;
    program.sources = sourceFiles_;
    if (hasImports_) {
        program.moduleGraph = moduleGraph_;
        rebuildPreloadedModuleInterfaces();
        for (ParsedUnit& unit : units_) {
            auto module = std::make_unique<ModuleStmt>(
                unit.id,
                unit.path,
                unit.source,
                std::move(unit.statements),
                unit.isEntry,
                SourceFileId{unit.sourceId});
            module->sourceHash = moduleCacheHash(module->source);
            program.statements.push_back(std::move(module));
        }
        finalizeSyntaxMetadata(program);
        return program;
    }

    for (const std::size_t id : entryUnitIds_) {
        ParsedUnit& unit = units_[id];
        for (StmtPtr& statement : unit.statements) {
            program.statements.push_back(std::move(statement));
        }
    }
    finalizeSyntaxMetadata(program);
    return program;
}

void FrontendSession::rebuildModuleGraph()
{
    moduleGraph_ = ModuleGraph{};
    moduleGraph_.nodes.reserve(units_.size());
    for (const ParsedUnit& unit : units_) {
        moduleGraph_.nodes.push_back(ModuleGraphNode{
            unit.id,
            SourceFileId{unit.sourceId},
            unit.path,
            unit.canonicalPath,
            unit.isEntry,
        });

        if (unit.interfaceArtifact && unit.statements.empty()) {
            for (const ModuleInterfaceArtifactDependency& dependency : unit.interfaceArtifact->dependencies) {
                const auto imported = canonicalToUnitId_.find(dependency.identity);
                if (imported == canonicalToUnitId_.end()) {
                    throw std::runtime_error("cached module interface references an unloaded dependency");
                }
                moduleGraph_.edges.push_back(ModuleGraphEdge{
                    unit.id,
                    imported->second,
                    dependency.kind,
                    dependency.requestedPath,
                });
            }
            continue;
        }

        for (const StmtPtr& statement : unit.statements) {
            if (const auto* import = dynamic_cast<const ImportStmt*>(statement.get())) {
                moduleGraph_.edges.push_back(ModuleGraphEdge{
                    unit.id,
                    import->resolvedModuleId,
                    ModuleGraphEdgeKind::Import,
                    importPath(import->path),
                });
                continue;
            }

            if (const auto* exportStmt = dynamic_cast<const ExportStmt*>(statement.get())) {
                if (!exportStmt->sourcePath) {
                    continue;
                }
                moduleGraph_.edges.push_back(ModuleGraphEdge{
                    unit.id,
                    exportStmt->resolvedModuleId,
                    ModuleGraphEdgeKind::ReExport,
                    importPath(*exportStmt->sourcePath),
                });
            }
        }
    }
}

void FrontendSession::rebuildPreloadedModuleInterfaces()
{
    preloadedModuleInterfaces_.clear();
    for (const ParsedUnit& unit : units_) {
        if (!unit.interfaceArtifact) {
            continue;
        }

        ModuleInterface interfaceInfo = unit.interfaceArtifact->interfaceInfo;
        interfaceInfo.moduleId = unit.id;
        interfaceInfo.sourceId = SourceFileId{unit.sourceId};
        interfaceInfo.path = unit.path;
        interfaceInfo.canonicalPath = unit.canonicalPath;
        interfaceInfo.isEntry = unit.isEntry;
        interfaceInfo.dependencies.clear();
        for (const ModuleInterfaceArtifactDependency& dependency : unit.interfaceArtifact->dependencies) {
            const auto imported = canonicalToUnitId_.find(dependency.identity);
            if (imported == canonicalToUnitId_.end()) {
                throw std::runtime_error("cached module interface references an unloaded dependency");
            }
            interfaceInfo.dependencies.push_back(ModuleInterfaceDependency{
                imported->second,
                dependency.kind,
                dependency.requestedPath,
            });
        }
        preloadedModuleInterfaces_.push_back(std::move(interfaceInfo));
    }
}

void FrontendSession::rebuildCombinedSource()
{
    combinedSource_.clear();
    for (const ParsedUnit& unit : units_) {
        appendWithNewlineSeparation(combinedSource_, unit.source);
    }
}

std::vector<Token> FrontendSession::displayTokens() const
{
    if (!directDisplayTokens_.empty()) {
        return directDisplayTokens_;
    }

    std::vector<Token> display;
    std::string combined;
    for (const ParsedUnit& unit : units_) {
        appendWithNewlineSeparation(combined, "");
        const int lineOffset = lineAtEnd(combined) - 1;
        for (const Token& token : unit.tokens) {
            if (token.type == TokenType::EndOfFile) {
                continue;
            }
            Token shifted = token;
            shifted.line += lineOffset;
            display.push_back(std::move(shifted));
        }
        appendWithNewlineSeparation(combined, unit.source);
    }
    Token eof = endOfFileToken(combined);
    if (!units_.empty()) {
        const ParsedUnit& last = units_.back();
        eof.source = last.sourceId;
        eof.sourceLine = lineAtEnd(last.source);
        eof.sourceId = SourceFileId{last.sourceId};
        eof.range = SourceRange{
            *eof.sourceId,
            last.source.size(),
            last.source.size()};
    }
    display.push_back(std::move(eof));
    return display;
}

LosslessSourceView FrontendSession::losslessSourceView() const
{
    std::vector<std::vector<Token>> tokensBySource(sourceFiles_.size());
    const auto collect = [&tokensBySource](const std::vector<Token>& tokens) {
        for (const Token& token : tokens) {
            std::optional<SourceFileId> sourceId = token.sourceId;
            if (!sourceId && token.source) {
                sourceId = SourceFileId{*token.source};
            }
            if (!sourceId || !sourceId->valid() || sourceId->value >= tokensBySource.size()) {
                continue;
            }
            tokensBySource[sourceId->value].push_back(token);
        }
    };

    if (!directDisplayTokens_.empty()) {
        collect(directDisplayTokens_);
    } else {
        for (const ParsedUnit& unit : units_) {
            collect(unit.tokens);
        }
    }

    std::vector<LosslessSourceFileView> files;
    files.reserve(sourceFiles_.size());
    for (std::size_t index = 0; index < sourceFiles_.size(); ++index) {
        files.push_back(buildLosslessSourceFileView(sourceFiles_[index], tokensBySource[index]));
    }
    return LosslessSourceView(std::move(files));
}

const std::string& FrontendSession::sourceForDiagnostics() const
{
    return combinedSource_;
}

std::optional<FileDiagnosticError> FrontendSession::remapDirectDiagnostic(const DiagnosticError& error) const
{
    if (!error.location() || directInputs_.size() < 2) {
        return std::nullopt;
    }

    std::size_t startLine = 1;
    for (const DirectInput& input : directInputs_) {
        const std::size_t span = sourceLineSpan(input.source);
        if (span == 0) {
            continue;
        }

        const std::size_t diagnosticLine = static_cast<std::size_t>(error.location()->line);
        if (diagnosticLine >= startLine && diagnosticLine < startLine + span) {
            DiagnosticError remapped(
                error.kind(),
                SourceLocation{
                    static_cast<int>(diagnosticLine - startLine + 1),
                    error.location()->column,
                },
                error.range(),
                error.message());
            return FileDiagnosticError(
                remapped,
                DiagnosticSourceContext{input.path, input.source, false});
        }
        startLine += span;
    }

    return std::nullopt;
}


std::optional<FileDiagnosticErrorList> FrontendSession::remapDirectDiagnostics(const ParseErrorList& errors) const
{
    if (directInputs_.size() < 2) {
        return std::nullopt;
    }

    std::vector<FileDiagnosticError> mapped;
    for (const ParseError& error : errors.errors()) {
        if (!error.location()) {
            return std::nullopt;
        }

        std::size_t startLine = 1;
        bool foundInput = false;
        for (const DirectInput& input : directInputs_) {
            const std::size_t span = sourceLineSpan(input.source);
            if (span == 0) {
                continue;
            }

            const std::size_t diagnosticLine = static_cast<std::size_t>(error.location()->line);
            if (diagnosticLine >= startLine && diagnosticLine < startLine + span) {
                DiagnosticError remapped(
                    error.kind(),
                    SourceLocation{
                        static_cast<int>(diagnosticLine - startLine + 1),
                        error.location()->column,
                    },
                    error.range(),
                    error.message());
                mapped.push_back(FileDiagnosticError(
                    remapped,
                    DiagnosticSourceContext{input.path, input.source, false}));
                foundInput = true;
                break;
            }
            startLine += span;
        }

        if (!foundInput) {
            return std::nullopt;
        }
    }

    return FileDiagnosticErrorList(std::move(mapped));
}

std::size_t FrontendSession::moduleCount() const
{
    return units_.size();
}

const ModuleGraph& FrontendSession::moduleGraph() const
{
    return moduleGraph_;
}
