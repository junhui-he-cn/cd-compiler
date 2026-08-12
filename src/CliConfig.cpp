#include "CliConfig.hpp"

#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

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

} // namespace

CliParseStatus parseCli(int argc, char** argv, CliConfig& config)
{
    // 第一阶段只收集 CLI 状态；具体编译在参数校验和前端配置之后进行。
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--tokens") {
            config.showTokens = true;
        } else if (arg == "--ir") {
            config.showIr = true;
        } else if (arg == "--bytecode") {
            config.showBytecode = true;
        } else if (arg == "--opt-level") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return CliParseStatus::Error;
            }
            const std::string level = argv[++i];
            if (level == "0") {
                config.optimizationLevel = SSAOptimizationLevel::O0;
            } else if (level == "1") {
                config.optimizationLevel = SSAOptimizationLevel::O1;
            } else {
                std::cerr << "--opt-level requires 0 or 1\n";
                return CliParseStatus::Error;
            }
            config.optimizationLevelSpecified = true;
        } else if (arg == "--module-interface") {
            config.showModuleInterface = true;
        } else if (arg == "--lsp") {
            config.runLsp = true;
        } else if (arg == "--format") {
            config.showFormat = true;
        } else if (arg == "--format-check") {
            config.checkFormat = true;
        } else if (arg == "--format-indent-width") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return CliParseStatus::Error;
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
                config.formatIndentWidth = static_cast<std::size_t>(parsed);
                config.formatIndentWidthSpecified = true;
            } catch (const std::exception&) {
                std::cerr << "--format-indent-width requires a positive integer\n";
                return CliParseStatus::Error;
            }
        } else if (arg == "-I" || arg == "--import-path") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return CliParseStatus::Error;
            }
            config.importSearchPaths.push_back(argv[++i]);
        } else if (arg == "--run") {
            printUsage(argv[0]);
            return CliParseStatus::Error;
        } else if (arg == "--emit-bytecode") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return CliParseStatus::Error;
            }
            config.emitBytecodePath = argv[++i];
        } else if (arg == "--emit-module-bytecode") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return CliParseStatus::Error;
            }
            config.emitModuleBytecodePath = argv[++i];
        } else if (arg == "--module-cache") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return CliParseStatus::Error;
            }
            config.moduleCachePath = argv[++i];
        } else if (arg == "--module-interface-cache") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return CliParseStatus::Error;
            }
            config.moduleInterfaceCachePath = argv[++i];
        } else if (arg == "--module-cache-strict") {
            config.moduleCacheStrict = true;
        } else if (arg == "--module-cache-fallback") {
            config.moduleCacheFallback = true;
        } else if (arg == "--module-rebuild-report") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return CliParseStatus::Error;
            }
            config.moduleRebuildReportPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return CliParseStatus::Help;
        } else {
            config.inputPaths.push_back(arg);
        }
    }

    // LSP 使用独立的 stdin/stdout 协议，不进入普通编译流水线。
    if (config.runLsp) {
        if (config.showTokens
            || config.showIr
            || config.showBytecode
            || config.showModuleInterface
            || config.showFormat
            || config.checkFormat
            || config.optimizationLevelSpecified
            || config.formatIndentWidthSpecified
            || config.emitBytecodePath
            || config.emitModuleBytecodePath
            || config.moduleCachePath
            || config.moduleInterfaceCachePath
            || config.moduleRebuildReportPath
            || config.moduleCacheStrict
            || config.moduleCacheFallback
            || !config.inputPaths.empty()
            || !config.importSearchPaths.empty()) {
            printUsage(argv[0]);
            return CliParseStatus::Error;
        }
        return CliParseStatus::Ok;
    }

    // 先拒绝互斥或缺少前置参数的组合，避免进入不完整的编译流程。
    if (config.moduleCacheStrict && config.moduleCacheFallback) {
        std::cerr << "--module-cache-strict and --module-cache-fallback are mutually exclusive\n";
        return CliParseStatus::Error;
    }

    const bool formatMode = config.formatMode();
    if (config.showFormat && config.checkFormat) {
        printUsage(argv[0]);
        return CliParseStatus::Error;
    }

    if (formatMode
        && (config.showTokens
            || config.showIr
            || config.showBytecode
            || config.showModuleInterface
            || config.optimizationLevelSpecified
            || config.emitBytecodePath
            || config.emitModuleBytecodePath
            || config.moduleCachePath
            || config.moduleInterfaceCachePath
            || config.moduleRebuildReportPath
            || config.moduleCacheStrict
            || config.moduleCacheFallback)) {
        printUsage(argv[0]);
        return CliParseStatus::Error;
    }

    if (config.formatIndentWidthSpecified && !formatMode) {
        std::cerr << "--format-indent-width requires --format or --format-check\n";
        return CliParseStatus::Error;
    }

    if (config.optimizationLevelSpecified
        && !config.showIr
        && !config.showBytecode
        && !config.emitBytecodePath
        && !config.emitModuleBytecodePath) {
        std::cerr << "--opt-level requires --ir, --bytecode, or bytecode emission\n";
        return CliParseStatus::Error;
    }

    if (config.moduleCacheFallback && !config.moduleInterfaceCachePath) {
        std::cerr << "--module-cache-fallback requires --module-interface-cache\n";
        return CliParseStatus::Error;
    }

    if (config.moduleCacheFallback && (config.moduleCachePath || config.emitModuleBytecodePath)) {
        std::cerr << "--module-cache-fallback is only valid for interface-only cache consumers\n";
        return CliParseStatus::Error;
    }

    if (config.moduleInterfaceCachePath
        && (config.emitBytecodePath || (config.emitModuleBytecodePath && !config.moduleCachePath))) {
        std::cerr << "--module-interface-cache cannot provide bytecode bodies; use --module-cache with --emit-module-bytecode\n";
        return CliParseStatus::Error;
    }

    if (config.moduleCacheStrict && !config.moduleCachePath && !config.moduleInterfaceCachePath) {
        std::cerr << "--module-cache-strict requires --module-cache or --module-interface-cache\n";
        return CliParseStatus::Error;
    }

    if (config.emitBytecodePath || config.emitModuleBytecodePath || config.moduleCachePath
        || config.moduleInterfaceCachePath || config.moduleRebuildReportPath
        || config.moduleCacheStrict || config.moduleCacheFallback) {
        if (config.inputPaths.empty()
            || formatMode
            || config.showTokens
            || config.showIr
            || config.showBytecode
            || config.showModuleInterface
            || (config.emitBytecodePath && config.emitModuleBytecodePath)
            || (!config.emitModuleBytecodePath && (config.moduleCachePath || config.moduleRebuildReportPath))
            || (config.moduleRebuildReportPath && !config.moduleCachePath)) {
            printUsage(argv[0]);
            return CliParseStatus::Error;
        }
    }

    if (config.moduleCachePath && config.moduleInterfaceCachePath
        && std::filesystem::path(*config.moduleCachePath).lexically_normal()
            != std::filesystem::path(*config.moduleInterfaceCachePath).lexically_normal()) {
        std::cerr << "--module-cache and --module-interface-cache must use the same directory\n";
        return CliParseStatus::Error;
    }

    const bool interfaceOnlyCacheConsumer = config.moduleInterfaceCachePath
        && !config.emitModuleBytecodePath
        && !config.moduleCachePath;
    if (interfaceOnlyCacheConsumer && !config.moduleCacheFallback) {
        config.moduleCacheStrict = true;
    }

    // 普通 CLI 必须显式提供源码文件；LSP 的 stdin/stdout 协议已在上方独立处理。
    if (config.inputPaths.empty()) {
        printUsage(argv[0]);
        return CliParseStatus::Error;
    }

    return CliParseStatus::Ok;
}
