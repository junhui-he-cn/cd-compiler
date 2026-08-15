#include "BytecodeCompiler.hpp"
#include "BytecodeTextEmitter.hpp"
#include "CliConfig.hpp"
#include "FrontendSession.hpp"
#include "Formatter.hpp"
#include "IRCompiler.hpp"
#include "LanguageServer.hpp"
#include "ModuleCache.hpp"
#include "ModuleInterfaceArtifact.hpp"
#include "ModuleInterfaceEmitter.hpp"
#include "Optimizer.hpp"
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
    const std::optional<std::size_t> initFunction = compiler.moduleInitFunction();
    if (!initFunction) {
        throw std::runtime_error("internal error: module init function was not compiled");
    }
    artifact.initFunction = checkedModuleArtifactNumber(
        *initFunction, "module init function index out of range");
    artifact.program = bytecodeCompiler.compile(ir);

    for (const IRModuleDependency& dependency : ir.moduleDependencies()) {
        const ModuleGraphNode* imported = findGraphNode(graph, dependency.importedModuleId);
        if (!imported) {
            throw std::runtime_error("internal error: module dependency has no graph node");
        }
        artifact.dependencies.push_back(BytecodeModuleDependency{
            imported->canonicalPath,
            dependency.kind,
            dependency.requestedPath});
    }
    return artifact;
}

template <typename Writer>
void writeArtifactFile(const std::filesystem::path& path, const Writer& writer)
{
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("failed to open bytecode output file: " + path.string());
    }
    writer(output);
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
    bool moduleCacheStrict,
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
        if (moduleCacheStrict && cacheLoad.found && !cacheLoad.manifest) {
            throw std::runtime_error(
                "module cache manifest is invalid: "
                + (*cacheDirectory / "module-cache.cdbc").string()
                + "; delete the cache directory or rerun with "
                  "--module-cache-fallback to rebuild it");
        }
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
        const auto writeModuleArtifact = [&artifact](std::ostream& out) {
            writeBytecodeModuleText(out, artifact);
        };
        if (cached != cacheDecisionsByIdentity.end()) {
            writeArtifactFile(*cacheDirectory / cached->second->artifactPath, writeModuleArtifact);
        }
        writeArtifactFile(outputPath, writeModuleArtifact);
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
            ModuleCacheRecord record{
                decision.module,
                decision.artifactPath,
                moduleInterfaceArtifactPath({}, decision.module.identity).generic_string()};
            record.module.contentDigest = moduleCacheArtifactDigest(
                *cacheDirectory / decision.artifactPath);
            manifest.records.push_back(std::move(record));
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

} // namespace

int main(int argc, char** argv)
{
    CliConfig config;
    switch (parseCli(argc, argv, config)) {
    case CliParseStatus::Help:
        return 0;
    case CliParseStatus::Error:
        return 64;
    case CliParseStatus::Ok:
        break;
    }

    const bool formatMode = config.formatMode();

    // LSP 使用独立的 stdin/stdout 协议，不进入普通编译流水线。
    if (config.runLsp) {
        try {
            return runLanguageServer(std::cin, std::cout);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }

    // 前端负责读取源码、解析 import、构建模块图，并提供诊断源信息。
    FrontendSession frontend;
    frontend.setImportSearchPaths(config.importSearchPaths);
    if (config.moduleInterfaceCachePath) {
        frontend.setModuleInterfaceCacheDirectory(*config.moduleInterfaceCachePath);
    } else if (config.moduleCachePath) {
        frontend.setModuleInterfaceCacheDirectory(*config.moduleCachePath);
    }
    frontend.setModuleProductCacheMode(config.moduleCachePath.has_value());
    frontend.setModuleInterfaceCacheStrict(config.moduleCacheStrict);
    if (config.moduleCachePath) {
        frontend.setModuleCacheOptimizationIdentity(
            optimizationLevelName(config.optimizationLevel),
            ssaOptimizationPipelineFingerprint(config.optimizationLevel));
    }
    try {
        Program program = frontend.loadFiles(config.inputPaths);

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
                    FormatterOptions{config.formatIndentWidth});
                if (config.checkFormat) {
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
        if (config.showTokens) {
            for (const Token& token : frontend.displayTokens()) {
                std::cout << token << '\n';
            }
            std::cout << '\n';
        }

        // 类型检查同时填充 DeclarationIndex 和模块接口，供后续 lowering 使用。
        TypeChecker typeChecker;
        typeChecker.setPreloadedModuleInterfaces(frontend.preloadedModuleInterfaces());
        typeChecker.check(program);

        if (!config.emitBytecodePath && !config.emitModuleBytecodePath && !config.showIr && !config.showBytecode && !config.showModuleInterface) {
            program.print(std::cout);
        }

        if (config.showModuleInterface) {
            writeModuleInterfaceText(std::cout, typeChecker.moduleInterfaces());
        }

        // 模块字节码按图节点独立生成，必要时复用缓存并写出相关 sidecar。
        if (config.emitModuleBytecodePath) {
            writeModuleArtifacts(
                *config.emitModuleBytecodePath,
                program,
                typeChecker.declarationIndex(),
                typeChecker.moduleInterfaces(),
                config.moduleCachePath ? std::optional<std::filesystem::path>(*config.moduleCachePath) : std::nullopt,
                config.moduleRebuildReportPath ? std::optional<std::filesystem::path>(*config.moduleRebuildReportPath) : std::nullopt,
                config.moduleCacheStrict,
                config.optimizationLevel);
            return 0;
        }

        // 普通后端统一经过 IR；O1 优化发生在 IR 和 Bytecode 之间。
        if (config.emitBytecodePath || config.showIr || config.showBytecode) {
            IRCompiler compiler;
            IRProgram ir = compiler.compile(program, typeChecker.declarationIndex());
            ir = optimizeProgram(std::move(ir), config.optimizationLevel);

            std::optional<BytecodeProgram> bytecode;
            if (config.emitBytecodePath || config.showBytecode) {
                BytecodeCompiler bytecodeCompiler;
                bytecode = bytecodeCompiler.compile(ir);
            }

            if (config.emitBytecodePath) {
                writeArtifactFile(*config.emitBytecodePath, [&bytecode](std::ostream& out) {
                    writeBytecodeText(out, *bytecode);
                });
                return 0;
            }

            bool emittedSection = false;
            const auto separateSection = [&emittedSection]() {
                if (emittedSection) {
                    std::cout << '\n';
                }
                emittedSection = true;
            };

            if (config.showIr) {
                separateSection();
                ir.print(std::cout);
            }

            if (config.showBytecode) {
                separateSection();
                bytecode->print(std::cout);
            }

        }
    // 将不同诊断类型统一转换为带源码上下文的 CLI 输出。
    } catch (const FileDiagnosticErrorList& errors) {
        printFileDiagnosticErrorList(errors);
        return 1;
    } catch (const FileDiagnosticError& error) {
        std::cerr << formatDiagnosticWithSourceContext(error) << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
