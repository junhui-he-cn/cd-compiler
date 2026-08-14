#pragma once

#include "Bytecode.hpp"
#include "Diagnostic.hpp"
#include "IR.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct FunctionPlan {
    std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>> locals;
    std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>> upvalues;
    std::vector<BytecodeUpvalue> upvalueSources;
    std::uint32_t localCount = 0;
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
        const FunctionPlan* plan);
    BytecodeInstruction lowerInstruction(
        const IRInstruction& instruction,
        const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
        const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
        const std::vector<std::string>& names,
        const FunctionPlan* plan);
    BytecodeFunction lowerFunction(
        const IRFunction& function,
        const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
        const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
        const std::vector<std::string>& names,
        const FunctionPlan& plan);
};
