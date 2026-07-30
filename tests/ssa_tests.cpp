#include "SSA.hpp"

#include "ControlFlowGraph.hpp"

#include <cassert>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {

SSAInstruction defineConstant(SSAValueId result, std::size_t originalInstruction)
{
    SSAInstruction instruction;
    instruction.op = IROp::Constant;
    instruction.result = result;
    instruction.originalInstruction = originalInstruction;
    return instruction;
}

SSAInstruction useValue(IROp op, SSAValueId value, std::size_t originalInstruction)
{
    SSAInstruction instruction;
    instruction.op = op;
    instruction.left = value;
    instruction.originalInstruction = originalInstruction;
    return instruction;
}

void assertThrowsSSA(const std::function<void()>& action, const std::string& fragment)
{
    try {
        action();
    } catch (const SSAError& error) {
        assert(std::string(error.what()).find(fragment) != std::string::npos);
        return;
    }
    assert(false && "expected SSAError");
}

ControlFlowGraph makeDiamondCFG()
{
    return buildControlFlowGraph({
        IRInstruction{IROp::JumpIfFalse, std::nullopt, std::nullopt, std::nullopt, {}, 3},
        IRInstruction{IROp::Constant, std::nullopt, std::nullopt, std::nullopt, {}, 0},
        IRInstruction{IROp::Jump, std::nullopt, std::nullopt, std::nullopt, {}, 4},
        IRInstruction{IROp::Constant, std::nullopt, std::nullopt, std::nullopt, {}, 0},
        IRInstruction{IROp::Return, std::nullopt, std::nullopt, std::nullopt, {}, 0},
    });
}

void test_empty_ssa_shell_matches_cfg()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        IRInstruction{IROp::Constant, std::nullopt, std::nullopt, std::nullopt, {}, 0},
        IRInstruction{IROp::Return, std::nullopt, std::nullopt, std::nullopt, {}, 0},
    });
    const SSAFunction ssa = makeSSAFunction(cfg);

    assert(ssa.blocks.size() == cfg.blocks.size());
    assert(ssa.blocks[0].id == cfg.blocks[0].id);
    assert(ssa.blocks[0].firstInstruction == 0);
    assert(ssa.blocks[0].endInstruction == 2);
    assert(ssa.blocks.back().syntheticExit);
    ssa.verify(cfg);
}

void test_phi_requires_ordered_incoming_value_for_each_predecessor()
{
    const ControlFlowGraph cfg = makeDiamondCFG();
    SSAFunction ssa = makeSSAFunction(cfg);
    ssa.blocks[0].instructions.push_back(defineConstant(0, 0));
    ssa.blocks[1].instructions.push_back(defineConstant(1, 1));
    ssa.blocks[2].instructions.push_back(defineConstant(2, 3));
    ssa.blocks[3].phis.push_back(SSAPhi{3, {{1, 1}, {2, 2}}});
    ssa.blocks[3].instructions.push_back(useValue(IROp::Return, 3, 4));

    ssa.verify(cfg);
}

void test_duplicate_definitions_are_rejected()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        IRInstruction{IROp::Constant, std::nullopt, std::nullopt, std::nullopt, {}, 0},
        IRInstruction{IROp::Constant, std::nullopt, std::nullopt, std::nullopt, {}, 0},
    });
    SSAFunction ssa = makeSSAFunction(cfg);
    ssa.blocks[0].instructions.push_back(defineConstant(0, 0));
    ssa.blocks[0].instructions.push_back(defineConstant(0, 1));

    assertThrowsSSA([&ssa, &cfg] { ssa.verify(cfg); }, "defined more than once");
}

void test_phi_predecessor_order_is_rejected()
{
    const ControlFlowGraph cfg = makeDiamondCFG();
    SSAFunction ssa = makeSSAFunction(cfg);
    ssa.blocks[0].instructions.push_back(defineConstant(0, 0));
    ssa.blocks[1].instructions.push_back(defineConstant(1, 1));
    ssa.blocks[2].instructions.push_back(defineConstant(2, 3));
    ssa.blocks[3].phis.push_back(SSAPhi{3, {{2, 2}, {1, 1}}});

    assertThrowsSSA([&ssa, &cfg] { ssa.verify(cfg); }, "phi incoming predecessors");
}

void test_undefined_uses_are_rejected()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        IRInstruction{IROp::Return, std::nullopt, std::nullopt, std::nullopt, {}, 0},
    });
    SSAFunction ssa = makeSSAFunction(cfg);
    ssa.blocks[0].instructions.push_back(useValue(IROp::Return, 9, 0));

    assertThrowsSSA([&ssa, &cfg] { ssa.verify(cfg); }, "undefined SSA value");
}

void test_incomplete_phi_is_rejected()
{
    const ControlFlowGraph cfg = makeDiamondCFG();
    SSAFunction ssa = makeSSAFunction(cfg);
    ssa.blocks[0].instructions.push_back(defineConstant(0, 0));
    ssa.blocks[1].instructions.push_back(defineConstant(1, 1));
    ssa.blocks[2].instructions.push_back(defineConstant(2, 3));
    ssa.blocks[3].phis.push_back(SSAPhi{3, {{1, 1}}});

    assertThrowsSSA([&ssa, &cfg] { ssa.verify(cfg); }, "phi incoming");
}

void test_memory_slots_keep_capture_and_unknown_storage_conservative()
{
    const ControlFlowGraph cfg = buildControlFlowGraph(std::vector<IRInstruction>{});
    SSAFunction ssa = makeSSAFunction(cfg);
    ssa.memorySlots.push_back(SSAMemorySlot{0, "local", SSAMemoryStorage::Local});
    ssa.memorySlots.push_back(SSAMemorySlot{1, "captured", SSAMemoryStorage::Captured});
    ssa.memorySlots.push_back(SSAMemorySlot{2, "unknown", SSAMemoryStorage::Unknown});

    assert(ssa.memorySlots[0].canPromote());
    assert(!ssa.memorySlots[1].canPromote());
    assert(!ssa.memorySlots[2].canPromote());
    ssa.verify(cfg);
}

void test_parameters_are_entry_definitions()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        IRInstruction{IROp::Return, std::nullopt, std::nullopt, std::nullopt, {}, 0},
    });
    SSAFunction ssa = makeSSAFunction(cfg);
    ssa.parameters.push_back(SSAParameter{0, cfg.entryBlock});
    ssa.blocks[0].instructions.push_back(useValue(IROp::Return, 0, 0));

    ssa.verify(cfg);
}

void test_ssa_blocks_must_match_cfg()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        IRInstruction{IROp::Return, std::nullopt, std::nullopt, std::nullopt, {}, 0},
    });
    SSAFunction ssa = makeSSAFunction(cfg);
    ssa.blocks.pop_back();

    assertThrowsSSA([&ssa, &cfg] { ssa.verify(cfg); }, "block count");
}

} // namespace

int main()
{
    test_empty_ssa_shell_matches_cfg();
    test_phi_requires_ordered_incoming_value_for_each_predecessor();
    test_duplicate_definitions_are_rejected();
    test_phi_predecessor_order_is_rejected();
    test_undefined_uses_are_rejected();
    test_incomplete_phi_is_rejected();
    test_memory_slots_keep_capture_and_unknown_storage_conservative();
    test_parameters_are_entry_definitions();
    test_ssa_blocks_must_match_cfg();
    return 0;
}
