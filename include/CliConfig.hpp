#pragma once

#include "Optimizer.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

enum class CliParseStatus {
    Ok,
    Help,
    Error,
};

struct CliConfig {
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

    bool formatMode() const
    {
        return showFormat || checkFormat;
    }
};

// Parses and validates the CLI arguments, printing usage or error messages to
// stderr. Help returns CliParseStatus::Help; invalid invocations return
// CliParseStatus::Error and must be reported with exit code 64.
CliParseStatus parseCli(int argc, char** argv, CliConfig& config);
