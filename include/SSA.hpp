#pragma once

#include "ControlFlowGraph.hpp"
#include "Dominance.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using SSAValueId = std::size_t;
using SSAMemorySlotId = std::size_t;

using SSAMemoryStorage = BindingStorageClass;

struct SSAMemorySlot {
    SSAMemorySlotId id = 0;
    std::string name;
    SSAMemoryStorage storage = SSAMemoryStorage::Unknown;
    std::optional<BindingId> bindingId = std::nullopt;

    bool canPromote() const;
};

// A block-level write site collected by the future IR lowering boundary. The
// current phi-placement slice intentionally does not inspect IR instructions.
struct SSAMemoryDefinition {
    SSAMemorySlotId slot = 0;
    CFGBlockId block = 0;
};

struct SSAPhiPlacement {
    SSAMemorySlotId slot = 0;
    CFGBlockId block = 0;
};

struct SSAIncoming {
    CFGBlockId predecessor = 0;
    SSAValueId value = 0;
};

struct SSAPhi {
    SSAValueId result = 0;
    std::vector<SSAIncoming> incoming;
    std::optional<SSAMemorySlotId> memorySlot = std::nullopt;
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
    std::optional<SSAMemorySlotId> memorySlot = std::nullopt;
    std::optional<BindingId> bindingId = std::nullopt;
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
    std::optional<SSAMemorySlotId> memorySlot = std::nullopt;
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

// Rename a register-form SSA input along the dominator tree. In the input,
// SSAInstruction result/operand IDs are pre-SSA virtual register IDs and
// variable operations identify their memory slot through memorySlot. The
// output allocates single-definition SSA values, removes Local load/store/
// assign operations, fills Local-slot phi incoming values, and keeps all
// other memory operations explicit. Register-form virtual register IDs must
// be unique definitions; register joins are a separate construction slice.
SSAFunction renamePromotableMemorySlots(
    const ControlFlowGraph& cfg,
    const DominanceInfo& dominance,
    const SSAFunction& input);

// Place phis for promotable local slots using iterated dominance frontiers.
// Definitions in unreachable blocks are ignored, and synthetic exit blocks
// never receive values. This returns placement metadata only; it does not
// allocate SSA values, fill incoming operands, or rename uses.
std::vector<SSAPhiPlacement> placePromotableMemoryPhis(
    const ControlFlowGraph& cfg,
    const DominanceInfo& dominance,
    const std::vector<SSAMemorySlot>& memorySlots,
    const std::vector<SSAMemoryDefinition>& definitions);
