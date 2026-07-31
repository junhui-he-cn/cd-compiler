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

IRInstruction cfgInstruction(IROp op, std::size_t operand = 0)
{
    IRInstruction result{op, std::nullopt, std::nullopt, std::nullopt, {}, operand};
    return result;
}

SSAInstruction valueInstruction(
    IROp op,
    SSAValueId result,
    std::size_t originalInstruction)
{
    SSAInstruction instruction;
    instruction.op = op;
    instruction.result = result;
    instruction.originalInstruction = originalInstruction;
    return instruction;
}

SSAInstruction plainInstruction(IROp op, std::size_t originalInstruction)
{
    SSAInstruction instruction;
    instruction.op = op;
    instruction.originalInstruction = originalInstruction;
    return instruction;
}

SSAInstruction memoryInstruction(
    IROp op,
    std::size_t originalInstruction,
    SSAMemorySlotId slot,
    std::optional<SSAValueId> result = std::nullopt,
    std::optional<SSAValueId> left = std::nullopt)
{
    SSAInstruction instruction;
    instruction.op = op;
    instruction.originalInstruction = originalInstruction;
    instruction.memorySlot = slot;
    instruction.result = result;
    instruction.left = left;
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

void test_rename_eliminates_local_memory_and_fills_diamond_phi()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::JumpIfFalse, 4),
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::StoreVar),
        cfgInstruction(IROp::Jump, 6),
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::StoreVar),
        cfgInstruction(IROp::LoadVar),
        cfgInstruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    SSAFunction input = makeSSAFunction(cfg);
    input.memorySlots.push_back(SSAMemorySlot{0, "local", SSAMemoryStorage::Local});

    input.parameters.push_back(SSAParameter{0, cfg.entryBlock});
    SSAInstruction branch = plainInstruction(IROp::JumpIfFalse, 0);
    branch.left = 0;
    input.blocks[0].instructions.push_back(branch);

    input.blocks[1].instructions.push_back(valueInstruction(IROp::Constant, 1, 1));
    input.blocks[1].instructions.push_back(memoryInstruction(IROp::StoreVar, 2, 0, std::nullopt, 1));
    input.blocks[1].instructions.push_back(plainInstruction(IROp::Jump, 3));
    input.blocks[2].instructions.push_back(valueInstruction(IROp::Constant, 2, 4));
    input.blocks[2].instructions.push_back(memoryInstruction(IROp::StoreVar, 5, 0, std::nullopt, 2));
    input.blocks[3].instructions.push_back(memoryInstruction(IROp::LoadVar, 6, 0, 3));
    input.blocks[3].instructions.push_back(useValue(IROp::Return, 3, 7));

    const SSAFunction renamed = renamePromotableMemorySlots(cfg, dominance, input);
    assert(renamed.blocks[3].phis.size() == 1);
    const SSAPhi& phi = renamed.blocks[3].phis.front();
    assert(phi.memorySlot == std::optional<SSAMemorySlotId>(0));
    assert(phi.incoming.size() == cfg.blocks[3].predecessors.size());
    assert(phi.incoming[0].predecessor == cfg.blocks[3].predecessors[0]);
    assert(phi.incoming[1].predecessor == cfg.blocks[3].predecessors[1]);
    assert(phi.incoming[0].value != phi.incoming[1].value);
    assert(renamed.blocks[3].instructions.size() == 1);
    assert(renamed.blocks[3].instructions.front().op == IROp::Return);
    assert(renamed.blocks[3].instructions.front().left == phi.result);

    for (const SSABlock& block : renamed.blocks) {
        for (const SSAInstruction& instruction : block.instructions) {
            assert(instruction.op != IROp::LoadVar);
            assert(instruction.op != IROp::StoreVar);
            assert(instruction.op != IROp::AssignVar);
        }
    }
    renamed.verify(cfg);
}

void test_rename_handles_loop_backedge_and_initial_definition()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::StoreVar),
        cfgInstruction(IROp::Jump, 3),
        cfgInstruction(IROp::LoadVar),
        cfgInstruction(IROp::JumpIfFalse, 8),
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::StoreVar),
        cfgInstruction(IROp::Jump, 3),
        cfgInstruction(IROp::LoadVar),
        cfgInstruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    SSAFunction input = makeSSAFunction(cfg);
    input.memorySlots.push_back(SSAMemorySlot{0, "counter", SSAMemoryStorage::Local});
    input.blocks[0].instructions.push_back(valueInstruction(IROp::Constant, 1, 0));
    input.blocks[0].instructions.push_back(memoryInstruction(IROp::StoreVar, 1, 0, std::nullopt, 1));
    input.blocks[0].instructions.push_back(plainInstruction(IROp::Jump, 2));
    input.blocks[1].instructions.push_back(memoryInstruction(IROp::LoadVar, 3, 0, 3));
    input.blocks[1].instructions.push_back(useValue(IROp::JumpIfFalse, 3, 4));
    input.blocks[2].instructions.push_back(valueInstruction(IROp::Constant, 4, 5));
    input.blocks[2].instructions.push_back(memoryInstruction(IROp::StoreVar, 6, 0, std::nullopt, 4));
    input.blocks[2].instructions.push_back(plainInstruction(IROp::Jump, 7));
    input.blocks[3].instructions.push_back(memoryInstruction(IROp::LoadVar, 8, 0, 8));
    input.blocks[3].instructions.push_back(useValue(IROp::Return, 8, 9));

    const SSAFunction renamed = renamePromotableMemorySlots(cfg, dominance, input);
    assert(renamed.blocks[1].phis.size() == 1);
    const SSAPhi& phi = renamed.blocks[1].phis.front();
    assert(phi.memorySlot == std::optional<SSAMemorySlotId>(0));
    assert(phi.incoming.size() == 2);
    assert(phi.incoming[0].predecessor == cfg.blocks[1].predecessors[0]);
    assert(phi.incoming[1].predecessor == cfg.blocks[1].predecessors[1]);
    assert(renamed.blocks[1].instructions.front().op == IROp::JumpIfFalse);
    assert(renamed.blocks[1].instructions.front().left == phi.result);
    assert(renamed.blocks[3].instructions.front().op == IROp::Return);
    assert(renamed.blocks[3].instructions.front().left == phi.result);
    renamed.verify(cfg);
}

void test_rename_uses_local_parameter_as_initial_slot_value()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::StoreVar),
        cfgInstruction(IROp::LoadVar),
        cfgInstruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    SSAFunction input = makeSSAFunction(cfg);
    input.memorySlots.push_back(SSAMemorySlot{0, "value", SSAMemoryStorage::Local});
    SSAParameter parameter{0, cfg.entryBlock};
    parameter.memorySlot = 0;
    input.parameters.push_back(parameter);
    input.blocks[0].instructions.push_back(memoryInstruction(IROp::StoreVar, 0, 0, std::nullopt, 0));
    input.blocks[0].instructions.push_back(memoryInstruction(IROp::LoadVar, 1, 0, 1));
    input.blocks[0].instructions.push_back(useValue(IROp::Return, 1, 2));

    const SSAFunction renamed = renamePromotableMemorySlots(cfg, dominance, input);
    assert(renamed.parameters.size() == 1);
    assert(renamed.blocks[0].instructions.size() == 1);
    assert(renamed.blocks[0].instructions.front().op == IROp::Return);
    assert(renamed.blocks[0].instructions.front().left == renamed.parameters.front().value);
    renamed.verify(cfg);
}

void test_rename_keeps_non_promotable_memory_explicit()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::LoadVar),
        cfgInstruction(IROp::StoreVar),
        cfgInstruction(IROp::AssignVar),
        cfgInstruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    SSAFunction input = makeSSAFunction(cfg);
    input.memorySlots.push_back(SSAMemorySlot{0, "captured", SSAMemoryStorage::Captured});
    input.blocks[0].instructions.push_back(memoryInstruction(IROp::LoadVar, 0, 0, 0));
    input.blocks[0].instructions.push_back(memoryInstruction(IROp::StoreVar, 1, 0, std::nullopt, 0));
    input.blocks[0].instructions.push_back(memoryInstruction(IROp::AssignVar, 2, 0, std::nullopt, 0));
    input.blocks[0].instructions.push_back(useValue(IROp::Return, 0, 3));

    const SSAFunction renamed = renamePromotableMemorySlots(cfg, dominance, input);
    assert(renamed.blocks[0].instructions.size() == 4);
    assert(renamed.blocks[0].instructions[0].op == IROp::LoadVar);
    assert(renamed.blocks[0].instructions[1].op == IROp::StoreVar);
    assert(renamed.blocks[0].instructions[2].op == IROp::AssignVar);
    assert(renamed.blocks[0].instructions[3].op == IROp::Return);
    for (const SSAInstruction& instruction : renamed.blocks[0].instructions) {
        if (instruction.op == IROp::LoadVar || instruction.op == IROp::StoreVar
            || instruction.op == IROp::AssignVar) {
            assert(instruction.memorySlot == std::optional<SSAMemorySlotId>(0));
        }
    }
    renamed.verify(cfg);
}

void test_rename_rejects_undefined_local_slot()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::LoadVar),
        cfgInstruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    SSAFunction input = makeSSAFunction(cfg);
    input.memorySlots.push_back(SSAMemorySlot{0, "missing", SSAMemoryStorage::Local});
    input.blocks[0].instructions.push_back(memoryInstruction(IROp::LoadVar, 0, 0, 0));
    input.blocks[0].instructions.push_back(useValue(IROp::Return, 0, 1));

    assertThrowsSSA(
        [&] { renamePromotableMemorySlots(cfg, dominance, input); },
        "used before definition");
}

void test_rename_rejects_missing_phi_predecessor_definition()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::JumpIfFalse, 4),
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::StoreVar),
        cfgInstruction(IROp::Jump, 6),
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::Copy),
        cfgInstruction(IROp::LoadVar),
        cfgInstruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    SSAFunction input = makeSSAFunction(cfg);
    input.memorySlots.push_back(SSAMemorySlot{0, "partial", SSAMemoryStorage::Local});
    input.blocks[0].instructions.push_back(plainInstruction(IROp::JumpIfFalse, 0));
    input.blocks[1].instructions.push_back(valueInstruction(IROp::Constant, 1, 1));
    input.blocks[1].instructions.push_back(memoryInstruction(IROp::StoreVar, 2, 0, std::nullopt, 1));
    input.blocks[1].instructions.push_back(plainInstruction(IROp::Jump, 3));
    input.blocks[2].instructions.push_back(valueInstruction(IROp::Constant, 3, 4));
    input.blocks[2].instructions.push_back(useValue(IROp::Copy, 3, 5));
    input.blocks[3].instructions.push_back(memoryInstruction(IROp::LoadVar, 6, 0, 4));
    input.blocks[3].instructions.push_back(useValue(IROp::Return, 6, 7));

    assertThrowsSSA(
        [&] { renamePromotableMemorySlots(cfg, dominance, input); },
        "has no value on predecessor");
}

void test_rename_rejects_invalid_slots_and_duplicate_raw_definitions()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::LoadVar),
        cfgInstruction(IROp::Constant),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);

    SSAFunction invalidSlot = makeSSAFunction(cfg);
    invalidSlot.memorySlots.push_back(SSAMemorySlot{0, "local", SSAMemoryStorage::Local});
    invalidSlot.blocks[0].instructions.push_back(memoryInstruction(IROp::LoadVar, 0, 1, 0));
    assertThrowsSSA(
        [&] { renamePromotableMemorySlots(cfg, dominance, invalidSlot); },
        "invalid memory slot");

    SSAFunction duplicate = makeSSAFunction(cfg);
    duplicate.blocks[0].instructions.push_back(valueInstruction(IROp::Constant, 0, 0));
    duplicate.blocks[0].instructions.push_back(valueInstruction(IROp::Constant, 0, 1));
    assertThrowsSSA(
        [&] { renamePromotableMemorySlots(cfg, dominance, duplicate); },
        "defined more than once");
}

void test_rename_rejects_non_dominating_raw_register_use()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::JumpIfFalse, 2),
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    SSAFunction input = makeSSAFunction(cfg);
    input.blocks[0].instructions.push_back(useValue(IROp::JumpIfFalse, 0, 0));
    input.blocks[1].instructions.push_back(valueInstruction(IROp::Constant, 0, 1));
    input.blocks[2].instructions.push_back(useValue(IROp::Return, 0, 2));

    assertThrowsSSA(
        [&] { renamePromotableMemorySlots(cfg, dominance, input); },
        "does not dominate");
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
    test_rename_eliminates_local_memory_and_fills_diamond_phi();
    test_rename_handles_loop_backedge_and_initial_definition();
    test_rename_uses_local_parameter_as_initial_slot_value();
    test_rename_keeps_non_promotable_memory_explicit();
    test_rename_rejects_undefined_local_slot();
    test_rename_rejects_missing_phi_predecessor_definition();
    test_rename_rejects_invalid_slots_and_duplicate_raw_definitions();
    test_rename_rejects_non_dominating_raw_register_use();
    return 0;
}
