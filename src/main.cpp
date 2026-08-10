#include "BytecodeCompiler.hpp"
#include "BytecodeTextEmitter.hpp"
#include "FrontendSession.hpp"
#include "Formatter.hpp"
#include "IRCompiler.hpp"
#include "LanguageServer.hpp"
#include "Lexer.hpp"
#include "ModuleCache.hpp"
#include "ModuleInterfaceArtifact.hpp"
#include "ModuleInterfaceEmitter.hpp"
#include "Optimizer.hpp"
#include "Parser.hpp"
#include "TypeChecker.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <string>
#include <vector>

namespace {

void printUsage(const char* executable)
{
    std::cerr << "Usage: " << executable << " [--tokens] [--ir] [--bytecode] [--opt-level 0|1] [--module-interface] [-I dir] [--import-path dir] file [...]\n"
              << "       " << executable << " [--format | --format-check] [--format-indent-width N] [-I dir] [--import-path dir] file [...]\n"
              << "       " << executable << " --lsp\n"
              << "       " << executable << " [--emit-bytecode output.cdbc] [--opt-level 0|1] [-I dir] [--import-path dir] file [...]\n"
              << "       " << executable << " [--emit-module-bytecode output-directory] [--opt-level 0|1] [--module-cache cache-directory] [--module-cache-strict] [--module-rebuild-report report.json] [-I dir] [--import-path dir] file [...]\n"
              << "       " << executable << " [--module-interface-cache cache-directory] [--module-cache-strict | --module-cache-fallback] [-I dir] [--import-path dir] file [...]\n"
              << "All non-LSP modes require at least one source file.\n"
              << "Import search paths are used for non-explicit string imports after the importing file's directory.\n";
}

const char* optimizationLevelName(SSAOptimizationLevel level)
{
    switch (level) {
    case SSAOptimizationLevel::O0:
        return "O0";
    case SSAOptimizationLevel::O1:
        return "O1";
    }
    throw std::runtime_error("unknown SSA optimization level");
}

IRProgram optimizeProgram(
    IRProgram program,
    SSAOptimizationLevel level)
{
    // O0 保持兼容的原始 IR；O1 才运行 SSA 优化并重建 IR。
    // O0 is the established linear-IR path.  The internal O0 adapter proves
    // its round-trip contract for already normalized streams, but ordinary
    // compiler IR may contain branch-local register redefinitions whose
    // de-SSA layout is intentionally not exposed at the compatibility level.
    if (level == SSAOptimizationLevel::O0) {
        return program;
    }
    return optimizeIRProgram(program, level).rebuild(program);
}

const ModuleStmt* findModule(const Program& program, std::size_t moduleId)
{
    for (const StmtPtr& statement : program.statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (module && module->moduleId == moduleId) {
            return module;
        }
    }
    return nullptr;
}

const ModuleGraphNode* findGraphNode(const ModuleGraph& graph, std::size_t moduleId)
{
    const auto found = std::find_if(
        graph.nodes.begin(),
        graph.nodes.end(),
        [moduleId](const ModuleGraphNode& node) { return node.moduleId == moduleId; });
    return found == graph.nodes.end() ? nullptr : &*found;
}

std::uint32_t checkedModuleArtifactNumber(std::size_t value, const char* message)
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(message);
    }
    return static_cast<std::uint32_t>(value);
}

const ModuleInterface* findModuleInterface(
    const std::vector<ModuleInterface>& interfaces,
    std::size_t moduleId)
{
    const auto found = std::find_if(
        interfaces.begin(),
        interfaces.end(),
        [moduleId](const ModuleInterface& interfaceInfo) {
            return interfaceInfo.moduleId == moduleId;
        });
    return found == interfaces.end() ? nullptr : &*found;
}

std::unordered_map<std::size_t, std::string> buildModuleInterfaceHashes(
    const ModuleGraph& graph,
    const std::vector<ModuleInterface>& interfaces)
{
    std::unordered_map<std::size_t, std::string> interfaceHashes;
    for (const ModuleGraphNode& node : graph.nodes) {
        const ModuleInterface* interfaceInfo = findModuleInterface(interfaces, node.moduleId);
        if (!interfaceInfo) {
            throw std::runtime_error("internal error: module cache is missing a module interface");
        }
        interfaceHashes.emplace(node.moduleId, moduleInterfaceArtifactHash(*interfaceInfo));
    }
    return interfaceHashes;
}

std::vector<ModuleCacheModule> buildModuleCacheModules(
    const Program& program,
    const std::vector<ModuleInterface>& interfaces,
    const std::unordered_map<std::size_t, std::optional<std::uint32_t>>& entryOrders,
    SSAOptimizationLevel optimizationLevel)
{
    if (!program.moduleGraph) {
        throw std::runtime_error("internal error: module cache requires a module graph");
    }
    const ModuleGraph& graph = *program.moduleGraph;
    const std::unordered_map<std::size_t, std::string> interfaceHashes
        = buildModuleInterfaceHashes(graph, interfaces);

    std::vector<ModuleCacheModule> modules;
    modules.reserve(graph.nodes.size());
    for (const ModuleGraphNode& node : graph.nodes) {
        const ModuleStmt* module = findModule(program, node.moduleId);
        if (!module) {
            throw std::runtime_error("internal error: module cache graph node has no module body");
        }
        ModuleCacheModule cacheModule;
        cacheModule.identity = node.canonicalPath;
        cacheModule.sourceHash = module->sourceHash.empty()
            ? moduleCacheHash(module->source)
            : module->sourceHash;
        cacheModule.interfaceHash = interfaceHashes.at(node.moduleId);
        cacheModule.optimizationLevel = optimizationLevelName(optimizationLevel);
        cacheModule.optimizerPipeline = ssaOptimizationPipelineFingerprint(optimizationLevel);
        cacheModule.isEntry = node.isEntry;
        const auto entryOrder = entryOrders.find(node.moduleId);
        if (entryOrder != entryOrders.end() && entryOrder->second) {
            cacheModule.entryOrder = *entryOrder->second;
        }
        for (const ModuleGraphEdge& edge : graph.edges) {
            if (edge.importingModuleId != node.moduleId) {
                continue;
            }
            const ModuleGraphNode* dependency = findGraphNode(graph, edge.importedModuleId);
            if (!dependency) {
                throw std::runtime_error("internal error: module cache dependency has no graph node");
            }
            cacheModule.dependencies.push_back(ModuleCacheDependency{
                dependency->canonicalPath,
                edge.kind,
                edge.requestedPath,
                interfaceHashes.at(edge.importedModuleId)});
        }
        modules.push_back(std::move(cacheModule));
    }
    return modules;
}

ModuleInterfaceArtifact buildModuleInterfaceArtifact(
    const ModuleGraph& graph,
    const ModuleGraphNode& node,
    const ModuleInterface& interfaceInfo,
    const std::string& sourceHash,
    const std::unordered_map<std::size_t, std::string>& interfaceHashes)
{
    ModuleInterfaceArtifact artifact;
    artifact.identity = node.canonicalPath;
    artifact.path = node.path;
    artifact.canonicalPath = node.canonicalPath;
    artifact.sourceHash = sourceHash;
    artifact.isEntry = node.isEntry;
    artifact.resolvedNameNext = interfaceInfo.resolvedNameNext;
    artifact.interfaceInfo = interfaceInfo;
    artifact.interfaceInfo.moduleId = node.moduleId;
    artifact.interfaceInfo.sourceId = node.sourceId;
    artifact.interfaceInfo.path = node.path;
    artifact.interfaceInfo.canonicalPath = node.canonicalPath;
    artifact.interfaceInfo.isEntry = node.isEntry;
    artifact.interfaceInfo.dependencies.clear();

    for (const ModuleGraphEdge& edge : graph.edges) {
        if (edge.importingModuleId != node.moduleId) {
            continue;
        }
        const ModuleGraphNode* dependency = findGraphNode(graph, edge.importedModuleId);
        if (!dependency) {
            throw std::runtime_error("internal error: module interface dependency has no graph node");
        }
        artifact.dependencies.push_back(ModuleInterfaceArtifactDependency{
            dependency->canonicalPath,
            edge.kind,
            edge.requestedPath,
            interfaceHashes.at(edge.importedModuleId),
        });
        artifact.interfaceInfo.dependencies.push_back(ModuleInterfaceDependency{
            edge.importedModuleId,
            edge.kind,
            edge.requestedPath,
        });
    }
    artifact.interfaceHash = moduleInterfaceArtifactHash(artifact.interfaceInfo);
    return artifact;
}

BytecodeModuleArtifact compileModuleArtifact(
    const Program& program,
    const ModuleGraph& graph,
    const ModuleGraphNode& node,
    const std::unordered_map<std::size_t, std::optional<std::uint32_t>>& entryOrders,
    const DeclarationIndex& declarationIndex,
    SSAOptimizationLevel optimizationLevel)
{
    // 独立模块按“模块 -> IR -> Bytecode”单独降低，并保留依赖信息。
    IRCompiler compiler;
    IRProgram ir = compiler.compileModule(program, node.moduleId, declarationIndex);
    ir = optimizeProgram(std::move(ir), optimizationLevel);
    BytecodeCompiler bytecodeCompiler;

    BytecodeModuleArtifact artifact;
    artifact.identity = node.canonicalPath;
    artifact.path = node.path;
    artifact.canonicalPath = node.canonicalPath;
    artifact.isEntry = node.isEntry;
    const auto entryOrder = entryOrders.find(node.moduleId);
    if (entryOrder != entryOrders.end()) {
        artifact.entryOrder = entryOrder->second;
    }
    artifact.program = bytecodeCompiler.compile(ir);

    for (const IRModuleDependency& dependency : ir.moduleDependencies()) {
        const ModuleGraphNode* imported = findGraphNode(graph, dependency.importedModuleId);
        if (!imported) {
            throw std::runtime_error("internal error: module dependency has no graph node");
        }
        artifact.dependencies.push_back(BytecodeModuleDependency{
            imported->canonicalPath,
            dependency.kind,
            dependency.requestedPath,
            checkedModuleArtifactNumber(dependency.instructionOffset, "module dependency offset out of range")});
    }
    return artifact;
}

void writeArtifactFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("failed to open bytecode output file: " + path.string());
    }
    output << text;
    if (!output) {
        throw std::runtime_error("failed to write bytecode output file: " + path.string());
    }
}

void writeModuleArtifacts(
    const std::filesystem::path& outputDirectory,
    const Program& program,
    const DeclarationIndex& declarationIndex,
    const std::vector<ModuleInterface>& interfaces,
    const std::optional<std::filesystem::path>& cacheDirectory,
    const std::optional<std::filesystem::path>& reportPath,
    SSAOptimizationLevel optimizationLevel)
{
    // 模块产物模式遍历模块图，并在可用时复用缓存中的产品。
    if (!program.moduleGraph || program.moduleGraph->nodes.empty()) {
        throw std::runtime_error("internal error: --emit-module-bytecode requires a module graph");
    }

    const ModuleGraph& graph = *program.moduleGraph;
    std::filesystem::create_directories(outputDirectory);

    std::size_t nextEntryOrder = 0;
    std::unordered_map<std::size_t, std::optional<std::uint32_t>> entryOrders;
    for (const ModuleGraphNode& node : graph.nodes) {
        if (node.isEntry) {
            entryOrders.emplace(node.moduleId, checkedModuleArtifactNumber(nextEntryOrder++, "module entry order out of range"));
        }
    }

    std::vector<ModuleCacheDecision> cacheDecisions;
    ModuleCacheLoadResult cacheLoad;
    std::unordered_map<std::size_t, std::string> interfaceHashes;
    if (cacheDirectory) {
        std::error_code pathError;
        const std::filesystem::path normalizedOutput = std::filesystem::weakly_canonical(outputDirectory, pathError);
        pathError.clear();
        const std::filesystem::path normalizedCache = std::filesystem::weakly_canonical(*cacheDirectory, pathError);
        if (!pathError && normalizedOutput == normalizedCache) {
            throw std::runtime_error("module cache directory must differ from module output directory");
        }
        std::filesystem::create_directories(*cacheDirectory);
        cacheLoad = readModuleCache(*cacheDirectory / "module-cache.cdbc");
        const ModuleCacheManifest previous = cacheLoad.manifest.value_or(ModuleCacheManifest{});
        const std::string emptyReason = cacheLoad.error.empty()
            ? (cacheLoad.found ? "new_module" : "cache_miss")
            : "cache_manifest_invalid";
        interfaceHashes = buildModuleInterfaceHashes(graph, interfaces);
        cacheDecisions = planModuleCacheBuild(
            buildModuleCacheModules(program, interfaces, entryOrders, optimizationLevel),
            previous,
            *cacheDirectory,
            emptyReason);
        std::filesystem::create_directories(*cacheDirectory / "products");
    }

    std::unordered_map<std::string, const ModuleCacheDecision*> cacheDecisionsByIdentity;
    for (const ModuleCacheDecision& decision : cacheDecisions) {
        cacheDecisionsByIdentity.emplace(decision.module.identity, &decision);
    }

    for (const ModuleGraphNode& node : graph.nodes) {
        if (!findModule(program, node.moduleId)) {
            throw std::runtime_error("internal error: module graph node has no module body");
        }
        const std::filesystem::path outputPath = outputDirectory / ("module-" + std::to_string(node.moduleId) + ".cdbc");
        const auto cached = cacheDecisionsByIdentity.find(node.canonicalPath);
        if (cached != cacheDecisionsByIdentity.end()
            && cached->second->status == ModuleCacheDecisionStatus::Reused) {
            const std::filesystem::path cachedPath = *cacheDirectory / cached->second->artifactPath;
            std::error_code copyError;
            std::filesystem::copy_file(
                cachedPath,
                outputPath,
                std::filesystem::copy_options::overwrite_existing,
                copyError);
            if (copyError) {
                throw std::runtime_error("failed to reuse cached module artifact: " + copyError.message());
            }
            continue;
        }

        const BytecodeModuleArtifact artifact = compileModuleArtifact(
            program,
            graph,
            node,
            entryOrders,
            declarationIndex,
            optimizationLevel);
        std::ostringstream text;
        writeBytecodeModuleText(text, artifact);
        if (cached != cacheDecisionsByIdentity.end()) {
            writeArtifactFile(*cacheDirectory / cached->second->artifactPath, text.str());
        }
        writeArtifactFile(outputPath, text.str());
    }

    if (cacheDirectory) {
        for (const ModuleGraphNode& node : graph.nodes) {
            const ModuleStmt* module = findModule(program, node.moduleId);
            const ModuleInterface* interfaceInfo = findModuleInterface(interfaces, node.moduleId);
            if (!module || !interfaceInfo) {
                throw std::runtime_error("internal error: module interface sidecar is missing module metadata");
            }
            ModuleInterfaceArtifact artifact = buildModuleInterfaceArtifact(
                graph,
                node,
                *interfaceInfo,
                module->sourceHash.empty() ? moduleCacheHash(module->source) : module->sourceHash,
                interfaceHashes);
            const auto entryOrder = entryOrders.find(node.moduleId);
            if (entryOrder != entryOrders.end()) {
                artifact.entryOrder = entryOrder->second;
            }
            writeModuleInterfaceArtifact(
                moduleInterfaceArtifactPath(*cacheDirectory, node.canonicalPath),
                artifact);
        }

        ModuleCacheManifest manifest;
        manifest.records.reserve(cacheDecisions.size());
        for (const ModuleCacheDecision& decision : cacheDecisions) {
            manifest.records.push_back(ModuleCacheRecord{
                decision.module,
                decision.artifactPath,
                moduleInterfaceArtifactPath({}, decision.module.identity).generic_string()});
        }
        writeModuleCache(*cacheDirectory / "module-cache.cdbc", manifest);
        if (reportPath) {
            if (!reportPath->parent_path().empty()) {
                std::filesystem::create_directories(reportPath->parent_path());
            }
            std::ofstream report(*reportPath);
            if (!report) {
                throw std::runtime_error("failed to open module rebuild report: " + reportPath->string());
            }
            writeModuleRebuildReport(report, cacheDecisions, cacheLoad.found, cacheLoad.error);
            if (!report) {
                throw std::runtime_error("failed to write module rebuild report: " + reportPath->string());
            }
        }
    }
}

void printParseErrorList(const ParseErrorList& errors, const std::string& source)
{
    bool first = true;
    for (const ParseError& error : errors.errors()) {
        if (!first) {
            std::cerr << '\n';
        }
        first = false;
        std::cerr << formatDiagnosticWithSource(error, source);
    }
    std::cerr << '\n';
}

void printLexErrorList(const LexErrorList& errors, const std::string& source)
{
    bool first = true;
    for (const DiagnosticError& error : errors.errors()) {
        if (!first) {
            std::cerr << '\n';
        }
        first = false;
        std::cerr << formatDiagnosticWithSource(error, source);
    }
    std::cerr << '\n';
}

void printFileDiagnosticErrors(const std::vector<FileDiagnosticError>& errors)
{
    bool first = true;
    for (const FileDiagnosticError& error : errors) {
        if (!first) {
            std::cerr << '\n';
        }
        first = false;
        std::cerr << formatDiagnosticWithSourceContext(error);
    }
    std::cerr << '\n';
}

void printFileDiagnosticErrorList(const FileDiagnosticErrorList& errors)
{
    printFileDiagnosticErrors(errors.errors());
}

void printTypeErrorList(const TypeErrorList& errors)
{
    printFileDiagnosticErrors(errors.errors());
}

} // namespace

int main(int argc, char** argv)
{
    bool showTokens = false;
    bool showIr = false;
    bool showBytecode = false;
    bool showModuleInterface = false;
    bool runLsp = false;
    bool showFormat = false;
    bool checkFormat = false;
    SSAOptimizationLevel optimizationLevel = SSAOptimizationLevel::O0;
    bool optimizationLevelSpecified = false;
    std::size_t formatIndentWidth = 2;
    bool formatIndentWidthSpecified = false;
    std::optional<std::string> emitBytecodePath;
    std::optional<std::string> emitModuleBytecodePath;
    std::optional<std::string> moduleCachePath;
    std::optional<std::string> moduleInterfaceCachePath;
    std::optional<std::string> moduleRebuildReportPath;
    bool moduleCacheStrict = false;
    bool moduleCacheFallback = false;
    std::vector<std::string> inputPaths;
    std::vector<std::string> importSearchPaths;

    // 第一阶段只收集 CLI 状态；具体编译在参数校验和前端配置之后进行。
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--tokens") {
            showTokens = true;
        } else if (arg == "--ir") {
            showIr = true;
        } else if (arg == "--bytecode") {
            showBytecode = true;
        } else if (arg == "--opt-level") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            const std::string level = argv[++i];
            if (level == "0") {
                optimizationLevel = SSAOptimizationLevel::O0;
            } else if (level == "1") {
                optimizationLevel = SSAOptimizationLevel::O1;
            } else {
                std::cerr << "--opt-level requires 0 or 1\n";
                return 64;
            }
            optimizationLevelSpecified = true;
        } else if (arg == "--module-interface") {
            showModuleInterface = true;
        } else if (arg == "--lsp") {
            runLsp = true;
        } else if (arg == "--format") {
            showFormat = true;
        } else if (arg == "--format-check") {
            checkFormat = true;
        } else if (arg == "--format-indent-width") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            const std::string widthText = argv[++i];
            try {
                std::size_t parsedCharacters = 0;
                const unsigned long long parsed = std::stoull(widthText, &parsedCharacters);
                if (parsedCharacters != widthText.size()
                    || parsed == 0
                    || parsed > std::numeric_limits<std::size_t>::max()) {
                    throw std::invalid_argument("invalid formatter indentation width");
                }
                formatIndentWidth = static_cast<std::size_t>(parsed);
                formatIndentWidthSpecified = true;
            } catch (const std::exception&) {
                std::cerr << "--format-indent-width requires a positive integer\n";
                return 64;
            }
        } else if (arg == "-I" || arg == "--import-path") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            importSearchPaths.push_back(argv[++i]);
        } else if (arg == "--run") {
            printUsage(argv[0]);
            return 64;
        } else if (arg == "--emit-bytecode") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            emitBytecodePath = argv[++i];
        } else if (arg == "--emit-module-bytecode") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            emitModuleBytecodePath = argv[++i];
        } else if (arg == "--module-cache") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            moduleCachePath = argv[++i];
        } else if (arg == "--module-interface-cache") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            moduleInterfaceCachePath = argv[++i];
        } else if (arg == "--module-cache-strict") {
            moduleCacheStrict = true;
        } else if (arg == "--module-cache-fallback") {
            moduleCacheFallback = true;
        } else if (arg == "--module-rebuild-report") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            moduleRebuildReportPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            inputPaths.push_back(arg);
        }
    }

    // LSP 使用独立的 stdin/stdout 协议，不进入普通编译流水线。
    if (runLsp) {
        if (showTokens
            || showIr
            || showBytecode
            || showModuleInterface
            || showFormat
            || checkFormat
            || optimizationLevelSpecified
            || formatIndentWidthSpecified
            || emitBytecodePath
            || emitModuleBytecodePath
            || moduleCachePath
            || moduleInterfaceCachePath
            || moduleRebuildReportPath
            || moduleCacheStrict
            || moduleCacheFallback
            || !inputPaths.empty()
            || !importSearchPaths.empty()) {
            printUsage(argv[0]);
            return 64;
        }
        try {
            return runLanguageServer(std::cin, std::cout);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }

    // 先拒绝互斥或缺少前置参数的组合，避免进入不完整的编译流程。
    if (moduleCacheStrict && moduleCacheFallback) {
        std::cerr << "--module-cache-strict and --module-cache-fallback are mutually exclusive\n";
        return 64;
    }

    const bool formatMode = showFormat || checkFormat;
    if (showFormat && checkFormat) {
        printUsage(argv[0]);
        return 64;
    }

    if (formatMode
        && (showTokens
            || showIr
            || showBytecode
            || showModuleInterface
            || optimizationLevelSpecified
            || emitBytecodePath
            || emitModuleBytecodePath
            || moduleCachePath
            || moduleInterfaceCachePath
            || moduleRebuildReportPath
            || moduleCacheStrict
            || moduleCacheFallback)) {
        printUsage(argv[0]);
        return 64;
    }

    if (formatIndentWidthSpecified && !formatMode) {
        std::cerr << "--format-indent-width requires --format or --format-check\n";
        return 64;
    }

    if (optimizationLevelSpecified
        && !showIr
        && !showBytecode
        && !emitBytecodePath
        && !emitModuleBytecodePath) {
        std::cerr << "--opt-level requires --ir, --bytecode, or bytecode emission\n";
        return 64;
    }

    if (moduleCacheFallback && !moduleInterfaceCachePath) {
        std::cerr << "--module-cache-fallback requires --module-interface-cache\n";
        return 64;
    }

    if (moduleCacheFallback && (moduleCachePath || emitModuleBytecodePath)) {
        std::cerr << "--module-cache-fallback is only valid for interface-only cache consumers\n";
        return 64;
    }

    if (moduleInterfaceCachePath
        && (emitBytecodePath || (emitModuleBytecodePath && !moduleCachePath))) {
        std::cerr << "--module-interface-cache cannot provide bytecode bodies; use --module-cache with --emit-module-bytecode\n";
        return 64;
    }

    if (moduleCacheStrict && !moduleCachePath && !moduleInterfaceCachePath) {
        std::cerr << "--module-cache-strict requires --module-cache or --module-interface-cache\n";
        return 64;
    }

    if (emitBytecodePath || emitModuleBytecodePath || moduleCachePath || moduleInterfaceCachePath
        || moduleRebuildReportPath || moduleCacheStrict || moduleCacheFallback) {
        if (inputPaths.empty()
            || formatMode
            || showTokens
            || showIr
            || showBytecode
            || showModuleInterface
            || (emitBytecodePath && emitModuleBytecodePath)
            || (!emitModuleBytecodePath && (moduleCachePath || moduleRebuildReportPath))
            || (moduleRebuildReportPath && !moduleCachePath)) {
            printUsage(argv[0]);
            return 64;
        }
    }

    if (moduleCachePath && moduleInterfaceCachePath
        && std::filesystem::path(*moduleCachePath).lexically_normal()
            != std::filesystem::path(*moduleInterfaceCachePath).lexically_normal()) {
        std::cerr << "--module-cache and --module-interface-cache must use the same directory\n";
        return 64;
    }

    const bool interfaceOnlyCacheConsumer = moduleInterfaceCachePath
        && !emitModuleBytecodePath
        && !moduleCachePath;
    if (interfaceOnlyCacheConsumer && !moduleCacheFallback) {
        moduleCacheStrict = true;
    }

    // 普通 CLI 必须显式提供源码文件；LSP 的 stdin/stdout 协议已在上方独立处理。
    if (inputPaths.empty()) {
        printUsage(argv[0]);
        return 64;
    }

    // 前端负责读取源码、解析 import、构建模块图，并提供诊断源信息。
    FrontendSession frontend;
    frontend.setImportSearchPaths(importSearchPaths);
    if (moduleInterfaceCachePath) {
        frontend.setModuleInterfaceCacheDirectory(*moduleInterfaceCachePath);
    } else if (moduleCachePath) {
        frontend.setModuleInterfaceCacheDirectory(*moduleCachePath);
    }
    frontend.setModuleProductCacheMode(moduleCachePath.has_value());
    frontend.setModuleInterfaceCacheStrict(moduleCacheStrict);
    try {
        Program program = frontend.loadFiles(inputPaths);

        // 格式化直接使用无损源码视图，完成后提前返回，不做类型检查和代码生成。
        if (formatMode) {
            const LosslessSourceView view = frontend.losslessSourceView();
            std::vector<SourceFileId> outputSourceIds;
            for (const ModuleGraphNode& node : program.moduleGraph->nodes) {
                if (node.isEntry) {
                    outputSourceIds.push_back(node.sourceId);
                }
            }

            bool emittedSource = false;
            bool formatCheckFailed = false;
            for (const SourceFileId sourceId : outputSourceIds) {
                const auto source = std::find_if(
                    program.sources.begin(),
                    program.sources.end(),
                    [sourceId](const SourceFile& candidate) { return candidate.id == sourceId; });
                if (source == program.sources.end()) {
                    throw std::runtime_error("internal error: formatter source ID is not present");
                }
                const std::string formatted = formatLosslessSource(
                    view.file(sourceId),
                    FormatterOptions{formatIndentWidth});
                if (checkFormat) {
                    if (formatted != source->text) {
                        std::cerr << "format check failed: " << source->path << '\n';
                        formatCheckFailed = true;
                    }
                    continue;
                }
                if (emittedSource) {
                    std::cout << '\n';
                }
                emittedSource = true;
                std::cout << formatted;
            }
            return formatCheckFailed ? 1 : 0;
        }

        // Token 是观察性输出；普通流程仍继续到类型检查以验证输入。
        if (showTokens) {
            for (const Token& token : frontend.displayTokens()) {
                std::cout << token << '\n';
            }
            std::cout << '\n';
        }

        // 类型检查同时填充 DeclarationIndex 和模块接口，供后续 lowering 使用。
        TypeChecker typeChecker;
        typeChecker.setPreloadedModuleInterfaces(frontend.preloadedModuleInterfaces());
        typeChecker.check(program);

        if (!emitBytecodePath && !emitModuleBytecodePath && !showIr && !showBytecode && !showModuleInterface) {
            program.print(std::cout);
        }

        if (showModuleInterface) {
            writeModuleInterfaceText(std::cout, typeChecker.moduleInterfaces());
        }

        // 模块字节码按图节点独立生成，必要时复用缓存并写出相关 sidecar。
        if (emitModuleBytecodePath) {
            writeModuleArtifacts(
                *emitModuleBytecodePath,
                program,
                typeChecker.declarationIndex(),
                typeChecker.moduleInterfaces(),
                moduleCachePath ? std::optional<std::filesystem::path>(*moduleCachePath) : std::nullopt,
                moduleRebuildReportPath ? std::optional<std::filesystem::path>(*moduleRebuildReportPath) : std::nullopt,
                optimizationLevel);
            return 0;
        }

        // 普通后端统一经过 IR；O1 优化发生在 IR 和 Bytecode 之间。
        if (emitBytecodePath || showIr || showBytecode) {
            IRCompiler compiler;
            IRProgram ir = compiler.compile(program, typeChecker.declarationIndex());
            ir = optimizeProgram(std::move(ir), optimizationLevel);

            std::optional<BytecodeProgram> bytecode;
            if (emitBytecodePath || showBytecode) {
                BytecodeCompiler bytecodeCompiler;
                bytecode = bytecodeCompiler.compile(ir);
            }

            if (emitBytecodePath) {
                std::ostringstream artifact;
                writeBytecodeText(artifact, *bytecode);
                std::ofstream output(*emitBytecodePath);
                if (!output) {
                    throw std::runtime_error("failed to open bytecode output file: " + *emitBytecodePath);
                }
                output << artifact.str();
                if (!output) {
                    throw std::runtime_error("failed to write bytecode output file: " + *emitBytecodePath);
                }
                return 0;
            }

            bool emittedSection = false;
            const auto separateSection = [&emittedSection]() {
                if (emittedSection) {
                    std::cout << '\n';
                }
                emittedSection = true;
            };

            if (showIr) {
                separateSection();
                ir.print(std::cout);
            }

            if (showBytecode) {
                separateSection();
                bytecode->print(std::cout);
            }

        }
    // 将不同诊断类型统一转换为带源码上下文的 CLI 输出。
    } catch (const TypeErrorList& errors) {
        printTypeErrorList(errors);
        return 1;
    } catch (const FileDiagnosticErrorList& errors) {
        printFileDiagnosticErrorList(errors);
        return 1;
    } catch (const ParseErrorList& errors) {
        printParseErrorList(errors, frontend.sourceForDiagnostics());
        return 1;
    } catch (const LexErrorList& errors) {
        printLexErrorList(errors, frontend.sourceForDiagnostics());
        return 1;
    } catch (const FileDiagnosticError& error) {
        std::cerr << formatDiagnosticWithSourceContext(error) << '\n';
        return 1;
    } catch (const DiagnosticError& error) {
        std::cerr << formatDiagnosticWithSource(error, frontend.sourceForDiagnostics()) << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
