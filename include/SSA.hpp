#pragma once

#include "ControlFlowGraph.hpp"
#include "Dominance.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
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

struct SSAMove {
    SSAValueId destination = 0;
    SSAValueId source = 0;
};

// A deterministic, edge-local copy bundle produced while planning de-SSA.
// Moves are already ordered for sequential execution; a critical edge is
// marked for a later CFG split, but this slice does not rewrite block ranges
// or instruction offsets.
struct SSAEdgeCopyBundle {
    CFGBlockId predecessor = 0;
    CFGBlockId successor = 0;
    bool requiresCriticalEdgeSplit = false;
    std::vector<SSAMove> moves;
};

struct SSADeSSACopyPlan {
    std::vector<SSAEdgeCopyBundle> edgeCopies;
    std::vector<SSAValueId> temporaryValues;
};

struct SSADeSSAInstruction {
    SSAInstruction instruction;
    bool synthetic = false;
};

struct SSADeSSABlock {
    std::size_t id = 0;
    std::optional<CFGBlockId> sourceBlock = std::nullopt;
    std::optional<std::pair<CFGBlockId, CFGBlockId>> splitEdge = std::nullopt;
    bool syntheticExit = false;
    std::size_t firstInstruction = 0;
    std::size_t endInstruction = 0;
};

struct SSADeSSALinearFunction {
    std::vector<SSAParameter> parameters;
    std::vector<SSAMemorySlot> memorySlots;
    std::vector<SSADeSSAInstruction> instructions;
    std::vector<SSADeSSABlock> blocks;
    // One entry per original CFG block, including the synthetic exit.
    std::vector<std::size_t> blockEntryOffsets;
    // A removed source instruction has no corresponding linear instruction.
    std::vector<std::optional<std::size_t>> originalInstructionOffsets;
    // Maps an original instruction insertion boundary to a legal output
    // offset. The final entry maps the end boundary.
    std::vector<std::size_t> originalInsertionOffsets;
    std::vector<IRModuleDependency> moduleDependencies;
    std::vector<SSAValueId> temporaryValues;

    void verify(const ControlFlowGraph& cfg) const;
};

struct SSADeSSAIRResult {
    IRFunction function;
    std::vector<IRModuleDependency> moduleDependencies;
    std::vector<bool> syntheticInstructions;
    std::vector<std::optional<std::size_t>> originalInstructionOffsets;
    std::vector<std::size_t> originalInsertionOffsets;

    void verify() const;
};

class SSAError final : public std::runtime_error {
public:
    explicit SSAError(std::string message);
};

struct SSAFunction {
    std::vector<SSABlock> blocks;
    std::vector<SSAParameter> parameters;
    std::vector<SSAMemorySlot> memorySlots;

    // Validate definitions, uses, dominance, phi edges, and instruction
    // shape for one CFG-aligned SSA function.
    void verify(const ControlFlowGraph& cfg) const;
};

SSAFunction makeSSAFunction(const ControlFlowGraph& cfg);

// Lift one ordinary IR stream into the conservative SSA shell. Register
// operands and source metadata are copied without promoting memory slots; the
// caller can then select a verified optimizer level before de-SSA lowering.
SSAFunction liftIRToSSA(
    const ControlFlowGraph& cfg,
    const std::vector<IRInstruction>& instructions);
SSAFunction liftIRToSSA(const ControlFlowGraph& cfg, const IRFunction& function);

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

// Plan phi lowering as sequential edge-local parallel copies. Bundles are
// ordered by successor block ID and then by the CFG predecessor order. A
// fresh temporary is allocated for each copy cycle. The plan is internal
// block/edge metadata; it does not split critical edges or remap linear IR
// offsets yet.
SSADeSSACopyPlan planSSADeSSACopies(
    const ControlFlowGraph& cfg,
    const SSAFunction& input);

// Materialize the copy plan into a deterministic linear block layout. Phi
// nodes are omitted, copy instructions are marked synthetic, critical edges
// receive internal split blocks, and branch/dependency offsets are remapped.
// This remains an internal SSA result and does not alter the original CFG or
// connect to the default IR/bytecode pipeline.
SSADeSSALinearFunction lowerSSADeSSACopies(
    const ControlFlowGraph& cfg,
    const SSAFunction& input);

// Adapt an already verified linear de-SSA result to the existing ordinary IR
// instruction representation. SSA value IDs remain stable as virtual-register
// indices; source parameter names are supplied by the caller because the SSA
// parameter records intentionally carry only value/block identity.
SSADeSSAIRResult lowerSSADeSSAToIR(
    const ControlFlowGraph& cfg,
    const SSADeSSALinearFunction& input,
    std::string name,
    std::vector<std::string> parameters,
    std::vector<IRBinding> bindings = {});

// Place phis for promotable local slots using iterated dominance frontiers.
// Definitions in unreachable blocks are ignored, and synthetic exit blocks
// never receive values. This returns placement metadata only; it does not
// allocate SSA values, fill incoming operands, or rename uses.
std::vector<SSAPhiPlacement> placePromotableMemoryPhis(
    const ControlFlowGraph& cfg,
    const DominanceInfo& dominance,
    const std::vector<SSAMemorySlot>& memorySlots,
    const std::vector<SSAMemoryDefinition>& definitions);
