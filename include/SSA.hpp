#pragma once

#include "ControlFlowGraph.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using SSAValueId = std::size_t;
using SSAMemorySlotId = std::size_t;

enum class SSAMemoryStorage {
    Unknown,
    Local,
    Captured,
    Module,
    Exported,
    Synthetic,
};

struct SSAMemorySlot {
    SSAMemorySlotId id = 0;
    std::string name;
    SSAMemoryStorage storage = SSAMemoryStorage::Unknown;

    bool canPromote() const;
};

struct SSAIncoming {
    CFGBlockId predecessor = 0;
    SSAValueId value = 0;
};

struct SSAPhi {
    SSAValueId result = 0;
    std::vector<SSAIncoming> incoming;
};

struct SSAInstruction {
    IROp op = IROp::Constant;
    std::optional<SSAValueId> result;
    std::optional<SSAValueId> left;
    std::optional<SSAValueId> right;
    std::vector<SSAValueId> arguments;
    std::size_t originalInstruction = 0;
    std::size_t operand = 0;
    std::vector<std::size_t> operands;
    std::optional<std::size_t> typeNameOperand = std::nullopt;
    std::optional<std::size_t> variantNameOperand = std::nullopt;
    std::optional<SourceSpan> span = std::nullopt;
};

struct SSABlock {
    CFGBlockId id = 0;
    std::size_t firstInstruction = 0;
    std::size_t endInstruction = 0;
    bool syntheticExit = false;
    std::vector<SSAPhi> phis;
    std::vector<SSAInstruction> instructions;
};

struct SSAParameter {
    SSAValueId value = 0;
    CFGBlockId block = 0;
};

class SSAError final : public std::runtime_error {
public:
    explicit SSAError(std::string message);
};

struct SSAFunction {
    std::vector<SSABlock> blocks;
    std::vector<SSAParameter> parameters;
    std::vector<SSAMemorySlot> memorySlots;

    // Validate the structural SSA contract. SSA construction and renaming are
    // later passes; this validator checks definitions, uses, phi edges, and
    // shape.
    void verify(const ControlFlowGraph& cfg) const;
};

SSAFunction makeSSAFunction(const ControlFlowGraph& cfg);
