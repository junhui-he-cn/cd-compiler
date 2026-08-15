#pragma once

#include "Bytecode.hpp"
#include "Diagnostic.hpp"
#include "IR.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct FunctionPlan {
    std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>> locals;
    std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>> upvalues;
    std::vector<BytecodeUpvalue> upvalueSources;
    std::uint32_t localCount = 0;
};

struct TypeTables {
    std::unordered_map<std::string, std::uint32_t> structTypeIds;
    std::unordered_map<std::string, std::vector<std::string>> structFields;
    std::unordered_map<std::string, std::uint32_t> enumTypeIds;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::uint32_t>>> enumVariants;
    std::vector<BytecodeType> types;
};

class BytecodeCompileError final : public DiagnosticError {
public:
    explicit BytecodeCompileError(std::string message);
};

class BytecodeCompiler {
public:
    BytecodeProgram compile(const IRProgram& ir);

private:
    std::vector<BytecodeInstruction> lowerInstructions(
        const std::vector<IRInstruction>& instructions,
        const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
        const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
        const std::vector<std::string>& names,
        TypeTables& types,
        const std::unordered_map<std::string, std::uint32_t>& nativeImports,
        std::uint32_t printScratch,
        const std::unordered_set<BindingId, SnapshotIdHash<BindingIdTag>>& moduleBindings,
        bool moduleInit,
        const FunctionPlan* plan);
    BytecodeInstruction lowerInstruction(
        const IRInstruction& instruction,
        const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
        const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
        const std::vector<std::string>& names,
        TypeTables& types,
        const std::unordered_map<std::string, std::uint32_t>& nativeImports,
        std::uint32_t printScratch,
        const std::unordered_set<BindingId, SnapshotIdHash<BindingIdTag>>& moduleBindings,
        bool moduleInit,
        const FunctionPlan* plan);
    BytecodeFunction lowerFunction(
        const IRFunction& function,
        const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
        const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
        const std::vector<std::string>& names,
        TypeTables& types,
        const std::unordered_map<std::string, std::uint32_t>& nativeImports,
        const std::unordered_set<BindingId, SnapshotIdHash<BindingIdTag>>& moduleBindings,
        bool moduleInit,
        const FunctionPlan& plan);
};
