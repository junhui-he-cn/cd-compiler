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

// A single entry source keeps pathless diagnostics only when it contains no
// import token before the first lexer failure, matching the historical
// combined-source decision without re-parsing the file.
bool entryFileContainsImportToken(const std::string& source)
{
    try {
        Lexer lexer(source);
        const std::vector<Token> tokens = lexer.scanTokensUntil(TokenType::Import);
        return !tokens.empty() && tokens.back().type == TokenType::Import;
    } catch (const LexErrorList&) {
        return false;
    } catch (const DiagnosticError&) {
        return false;
    }
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

bool pathWithinRoot(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(
        normalizedExistingPath(candidate),
        normalizedExistingPath(root),
        error);
    if (error || relative.is_absolute()) {
        return false;
    }
    for (const std::filesystem::path& component : relative) {
        if (component == "..") {
            return false;
        }
    }
    return true;
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

FileDiagnosticErrorList fileDiagnosticListFromLexErrors(
    const LexErrorList& errors,
    const std::string& path,
    const std::string& source,
    bool pathlessDiagnostics)
{
    std::vector<FileDiagnosticError> mapped;
    for (const DiagnosticError& error : errors.errors()) {
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
    } catch (const LexErrorList& errors) {
        throw fileDiagnosticListFromLexErrors(errors, path, source, pathlessDiagnostics);
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

} // namespace

void FrontendSession::reset()
{
    units_.clear();
    canonicalToUnitId_.clear();
    loadingStack_.clear();
    entryUnitIds_.clear();
    sourceFiles_.clear();
    directEntryCanonicalPaths_.clear();
    combinedSource_.clear();
    moduleGraph_ = ModuleGraph{};
    preloadedModuleInterfaces_.clear();
    moduleProductCacheLoad_.reset();
    virtualSources_.clear();
    virtualSourceMode_ = false;
    singleEntrySource_ = false;
}

void FrontendSession::setImportSearchPaths(std::vector<std::string> paths)
{
    importSearchPaths_.clear();
    for (const std::string& path : paths) {
        importSearchPaths_.push_back(std::filesystem::path(path).lexically_normal());
    }
}

void FrontendSession::setVirtualImportRoots(std::vector<std::string> paths)
{
    virtualImportRoots_.clear();
    for (const std::string& path : paths) {
        virtualImportRoots_.push_back(normalizedExistingPath(std::filesystem::path(path)));
    }
}

void FrontendSession::setModuleInterfaceCacheDirectory(std::filesystem::path path)
{
    moduleInterfaceCacheDirectory_ = path.lexically_normal();
    moduleProductCacheLoad_.reset();
}

void FrontendSession::setModuleInterfaceCacheStrict(bool strict)
{
    moduleInterfaceCacheStrict_ = strict;
}

void FrontendSession::setModuleProductCacheMode(bool enabled)
{
    moduleProductCacheMode_ = enabled;
    moduleProductCacheLoad_.reset();
}

const std::vector<ModuleInterface>& FrontendSession::preloadedModuleInterfaces() const
{
    return preloadedModuleInterfaces_;
}

FrontendSession::CachedInterfaceLoad FrontendSession::loadCachedInterface(
    const std::string& canonicalPath,
    const std::string& source) const
{
    if (!moduleInterfaceCacheDirectory_) {
        return CachedInterfaceLoad{};
    }

    const ModuleInterfaceArtifactLoadResult loaded = readModuleInterfaceArtifact(
        moduleInterfaceArtifactPath(*moduleInterfaceCacheDirectory_, canonicalPath));
    if (!loaded.artifact) {
        const bool identityMetadataError = loaded.error.find("invalid identity metadata") != std::string::npos;
        return CachedInterfaceLoad{
            std::nullopt,
            loaded.found
                ? (identityMetadataError ? "identity/canonical path mismatch" : "malformed sidecar")
                : "missing sidecar",
        };
    }
    const ModuleInterfaceArtifact& artifact = *loaded.artifact;
    if (artifact.identity != canonicalPath || artifact.canonicalPath != canonicalPath) {
        return CachedInterfaceLoad{std::nullopt, "identity/canonical path mismatch"};
    }
    if (artifact.sourceHash != moduleCacheHash(source)) {
        return CachedInterfaceLoad{std::nullopt, "source hash mismatch"};
    }

    const std::string productCacheRejection = moduleProductCacheRejection(canonicalPath, artifact);
    if (!productCacheRejection.empty()) {
        return CachedInterfaceLoad{std::nullopt, productCacheRejection};
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
    std::error_code productError;
    if (!std::filesystem::is_regular_file(product, productError)) {
        return CachedInterfaceLoad{std::nullopt, "missing paired product"};
    }
    return CachedInterfaceLoad{artifact, {}};
}

std::string FrontendSession::moduleProductCacheRejection(
    const std::string& canonicalPath,
    const ModuleInterfaceArtifact& artifact) const
{
    if (!moduleProductCacheMode_) {
        return {};
    }
    if (!moduleInterfaceCacheDirectory_) {
        return "missing module cache directory";
    }

    if (!moduleProductCacheLoad_) {
        moduleProductCacheLoad_ = readModuleCache(
            *moduleInterfaceCacheDirectory_ / "module-cache.cdbc");
    }
    const ModuleCacheLoadResult& loaded = *moduleProductCacheLoad_;
    if (!loaded.manifest) {
        return loaded.found ? "invalid module cache manifest" : "missing module cache manifest";
    }

    const ModuleCacheRecord* record = nullptr;
    for (const ModuleCacheRecord& candidate : loaded.manifest->records) {
        if (candidate.module.identity == canonicalPath) {
            record = &candidate;
            break;
        }
    }
    if (!record) {
        return "missing module cache record";
    }

    ModuleCacheModule expected;
    expected.identity = artifact.identity;
    expected.sourceHash = artifact.sourceHash;
    expected.interfaceHash = artifact.interfaceHash;
    expected.isEntry = artifact.isEntry;
    expected.entryOrder = artifact.entryOrder;
    for (const ModuleInterfaceArtifactDependency& dependency : artifact.dependencies) {
        expected.dependencies.push_back(ModuleCacheDependency{
            dependency.identity,
            dependency.kind,
            dependency.requestedPath,
            dependency.interfaceHash,
        });
    }
    const std::string expectedKey = moduleCacheKey(expected);
    if (record->module.cacheKey != expectedKey
        || record->artifactPath != moduleCacheArtifactPath(expected)
        || record->interfaceArtifactPath != moduleInterfaceArtifactPath({}, canonicalPath).generic_string()) {
        return "module cache record mismatch";
    }
    return {};
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
            const bool virtualAvailable = virtualSources_.find(
                pathString(normalizedExistingPath(candidate))) != virtualSources_.end();
            const bool diskAvailable = canOpenFile(candidate)
                && (!virtualSourceMode_ || std::any_of(
                    virtualImportRoots_.begin(),
                    virtualImportRoots_.end(),
                    [&candidate](const std::filesystem::path& root) {
                        return pathWithinRoot(candidate, root);
                    }));
            const bool available = virtualAvailable || diskAvailable;
            if (available) {
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
    singleEntrySource_ = true;

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
    rebuildModuleGraph();
    rebuildCombinedSource();
    Program program = assembleProgram();
    program.sources = sourceFiles_;
    return program;
}

Program FrontendSession::loadFiles(const std::vector<std::string>& paths)
{
    reset();

    singleEntrySource_ = paths.size() == 1;
    for (const std::string& path : paths) {
        directEntryCanonicalPaths_.insert(pathString(normalizedExistingPath(path)));
    }
    std::vector<FileDiagnosticError> entryErrors;
    for (const std::string& path : paths) {
        try {
            const std::size_t id = loadFile(path, false, true, true);
            if (std::find(entryUnitIds_.begin(), entryUnitIds_.end(), id) == entryUnitIds_.end()) {
                entryUnitIds_.push_back(id);
            }
        } catch (const FileDiagnosticErrorList& errors) {
            entryErrors.insert(entryErrors.end(), errors.errors().begin(), errors.errors().end());
        } catch (const FileDiagnosticError& error) {
            entryErrors.push_back(error);
        }
    }
    if (!entryErrors.empty()) {
        throw FileDiagnosticErrorList(std::move(entryErrors));
    }
    rebuildModuleGraph();
    rebuildCombinedSource();
    return assembleProgram();
}

Program FrontendSession::loadVirtualFiles(const std::vector<FrontendVirtualFile>& files)
{
    reset();
    virtualSourceMode_ = true;
    for (const FrontendVirtualFile& file : files) {
        virtualSources_.insert_or_assign(
            pathString(normalizedExistingPath(file.path)),
            file.source);
    }

    for (const FrontendVirtualFile& file : files) {
        const std::size_t id = loadFile(file.path, false, true, true);
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

    const auto virtualSource = virtualSources_.find(canonicalPath);
    if (virtualSource == virtualSources_.end()
        && virtualSourceMode_
        && (isImport && !std::any_of(
            virtualImportRoots_.begin(),
            virtualImportRoots_.end(),
            [&requestedPath](const std::filesystem::path& root) {
                return pathWithinRoot(requestedPath, root);
            }))) {
        if (isImport) {
            throw DiagnosticError(DiagnosticKind::Import, "failed to open import: " + displayPath);
        }
        throw std::runtime_error("failed to open input file: " + displayPath);
    }
    if (virtualSource == virtualSources_.end() && !canOpenFile(requestedPath)) {
        if (isImport) {
            throw DiagnosticError(DiagnosticKind::Import, "failed to open import: " + displayPath);
        }
        throw std::runtime_error("failed to open input file: " + displayPath);
    }

    loadingStack_.push_back(canonicalPath);
    std::string source;
    try {
        if (virtualSource != virtualSources_.end()) {
            source = virtualSource->second;
        } else {
            std::ifstream input(requestedPath);
            source = readAll(input);
        }
        if (isImport && directEntryCanonicalPaths_.find(canonicalPath) == directEntryCanonicalPaths_.end()) {
            CachedInterfaceLoad cached = loadCachedInterface(canonicalPath, source);
            if (!cached.artifact) {
                if (moduleInterfaceCacheStrict_ && !cached.rejectionReason.empty()) {
                    throw DiagnosticError(
                        DiagnosticKind::Import,
                        "module interface cache rejected for " + canonicalPath + ": "
                            + cached.rejectionReason);
                }
            } else {
                bool dependenciesCached = true;
                for (const ModuleInterfaceArtifactDependency& dependency : cached.artifact->dependencies) {
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
                    if (moduleInterfaceCacheStrict_) {
                        throw DiagnosticError(
                            DiagnosticKind::Import,
                            "module interface cache rejected for " + canonicalPath
                                + ": dependency interface hash mismatch");
                    }
                } else {
                    const std::size_t sourceId = sourceFiles_.size();
                    sourceFiles_.push_back(SourceFile{
                        cached.artifact->path,
                        source,
                        SourceFileId{sourceId}});

                    ParsedUnit unit{
                        0,
                        sourceId,
                        cached.artifact->path,
                        canonicalPath,
                        std::move(source),
                        {},
                        {},
                        false,
                        std::move(cached.artifact),
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
        } catch (const LexErrorList& errors) {
            throw fileDiagnosticListFromLexErrors(
                errors,
                displayPath,
                source,
                (!fileDiagnostics && !isImport)
                    || (isEntry && singleEntrySource_ && !entryFileContainsImportToken(source)));
        } catch (const DiagnosticError& error) {
            if (error.location()) {
                throw FileDiagnosticError(
                    error,
                    DiagnosticSourceContext{
                        displayPath,
                        source,
                        (!fileDiagnostics && !isImport)
                            || (isEntry
                                && singleEntrySource_
                                && !entryFileContainsImportToken(source))});
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
                (!fileDiagnostics && !isImport && !thisUnitHasImport)
                    || (isEntry && singleEntrySource_ && !thisUnitHasImport));
        } catch (const FileDiagnosticError&) {
            throw;
        } catch (const DiagnosticError& error) {
            if (error.location()) {
                throw FileDiagnosticError(
                    error,
                    DiagnosticSourceContext{
                        displayPath,
                        source,
                        (!fileDiagnostics && !isImport && !thisUnitHasImport)
                            || (isEntry && singleEntrySource_ && !thisUnitHasImport),
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
                const ImportResolution resolution = resolveImportPath(normalizedPath, import->path);
                import->resolvedModuleId = loadFile(resolution.path.string(), true, false, true);
                continue;
            }

            auto* exportStmt = dynamic_cast<ExportStmt*>(statement.get());
            if (!exportStmt || !exportStmt->sourcePath) {
                continue;
            }
            const ImportResolution resolution = resolveImportPath(normalizedPath, *exportStmt->sourcePath);
            exportStmt->resolvedModuleId = loadFile(resolution.path.string(), true, false, true);
        }

        unit.id = units_.size();
        units_.push_back(std::move(unit));
        canonicalToUnitId_.emplace(canonicalPath, units_.back().id);
        loadingStack_.pop_back();
        return units_.back().id;
    } catch (const FileDiagnosticError& error) {
        loadingStack_.pop_back();
        const auto currentVirtualSource = virtualSources_.find(canonicalPath);
        const bool errorBelongsToVirtualSource = !error.sourceContext().path.empty()
            && virtualSources_.find(pathString(normalizedExistingPath(error.sourceContext().path)))
                != virtualSources_.end();
        if (virtualSourceMode_
            && error.kind() == DiagnosticKind::Import
            && currentVirtualSource != virtualSources_.end()
            && !errorBelongsToVirtualSource) {
            throw FileDiagnosticError(
                error,
                DiagnosticSourceContext{displayPath, source, false});
        }
        throw;
    } catch (const DiagnosticError& error) {
        loadingStack_.pop_back();
        if (virtualSourceMode_ && error.kind() == DiagnosticKind::Import) {
            throw FileDiagnosticError(
                error,
                DiagnosticSourceContext{displayPath, source, false});
        }
        throw;
    } catch (...) {
        loadingStack_.pop_back();
        throw;
    }
}

Program FrontendSession::assembleProgram()
{
    Program program;
    program.sources = sourceFiles_;
    program.moduleGraph = moduleGraph_;
    // Single-module programs keep the historical artifact/debug metadata
    // surface: module identity is recorded only when the graph is module-aware
    // (multiple modules or dependency edges).
    const bool moduleAware = moduleGraph_.nodes.size() > 1 || !moduleGraph_.edges.empty();
    for (const ModuleGraphNode& node : program.moduleGraph->nodes) {
        for (SourceFile& source : program.sources) {
            if (source.id == node.sourceId) {
                if (moduleAware) {
                    source.moduleIdentity = node.canonicalPath;
                }
                break;
            }
        }
    }
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
        module->bodySourceBacked = !unit.interfaceArtifact.has_value();
        program.statements.push_back(std::move(module));
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

    for (const ParsedUnit& unit : units_) {
        collect(unit.tokens);
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

std::size_t FrontendSession::moduleCount() const
{
    return units_.size();
}

const ModuleGraph& FrontendSession::moduleGraph() const
{
    return moduleGraph_;
}
