#include "SSA.hpp"

#include <cassert>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {

IRInstruction instruction(IROp op)
{
    return IRInstruction{op, std::nullopt, std::nullopt, std::nullopt, {}, 0};
}

IRInstruction jump(IROp op, std::size_t target)
{
    IRInstruction result = instruction(op);
    result.operand = target;
    return result;
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
        jump(IROp::JumpIfFalse, 3),
        instruction(IROp::Constant),
        jump(IROp::Jump, 4),
        instruction(IROp::Constant),
        instruction(IROp::Return),
    });
}

void test_local_definitions_place_join_phi()
{
    const ControlFlowGraph cfg = makeDiamondCFG();
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    const std::vector<SSAMemorySlot> slots{
        SSAMemorySlot{0, "local", SSAMemoryStorage::Local},
        SSAMemorySlot{1, "captured", SSAMemoryStorage::Captured},
    };
    const std::vector<SSAPhiPlacement> placements = placePromotableMemoryPhis(
        cfg,
        dominance,
        slots,
        {
            SSAMemoryDefinition{0, 2},
            SSAMemoryDefinition{0, 1},
            SSAMemoryDefinition{1, 2},
        });

    assert(placements.size() == 1);
    assert(placements[0].slot == 0);
    assert(placements[0].block == 3);
}

void test_loop_backedge_places_header_phi()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        jump(IROp::JumpIfFalse, 3),
        instruction(IROp::Print),
        jump(IROp::Jump, 0),
        instruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    const std::vector<SSAPhiPlacement> placements = placePromotableMemoryPhis(
        cfg,
        dominance,
        {SSAMemorySlot{0, "counter", SSAMemoryStorage::Local}},
        {SSAMemoryDefinition{0, 0}, SSAMemoryDefinition{0, 1}});

    assert(placements.size() == 1);
    assert(placements[0].slot == 0);
    assert(placements[0].block == 0);
}

void test_unreachable_definitions_are_ignored()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        jump(IROp::Jump, 3),
        instruction(IROp::Constant),
        instruction(IROp::Print),
        instruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    const std::vector<SSAPhiPlacement> placements = placePromotableMemoryPhis(
        cfg,
        dominance,
        {SSAMemorySlot{0, "local", SSAMemoryStorage::Local}},
        {SSAMemoryDefinition{0, 1}});

    assert(placements.empty());
}

void test_synthetic_exit_never_receives_a_phi()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        jump(IROp::JumpIfFalse, 2),
        instruction(IROp::Return),
        instruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    const std::vector<SSAPhiPlacement> placements = placePromotableMemoryPhis(
        cfg,
        dominance,
        {SSAMemorySlot{0, "local", SSAMemoryStorage::Local}},
        {SSAMemoryDefinition{0, 1}, SSAMemoryDefinition{0, 2}});

    assert(placements.empty());
}

void test_only_local_slots_are_promoted_and_output_is_deterministic()
{
    const ControlFlowGraph cfg = makeDiamondCFG();
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    const std::vector<SSAPhiPlacement> placements = placePromotableMemoryPhis(
        cfg,
        dominance,
        {
            SSAMemorySlot{0, "local", SSAMemoryStorage::Local},
            SSAMemorySlot{1, "module", SSAMemoryStorage::Module},
        },
        {
            SSAMemoryDefinition{1, 2},
            SSAMemoryDefinition{0, 2},
            SSAMemoryDefinition{1, 1},
            SSAMemoryDefinition{0, 1},
        });

    assert(placements.size() == 1);
    assert(placements[0].slot == 0);
    assert(placements[0].block == 3);
}

void test_invalid_definition_references_are_rejected()
{
    const ControlFlowGraph cfg = makeDiamondCFG();
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    const std::vector<SSAMemorySlot> slots{
        SSAMemorySlot{0, "local", SSAMemoryStorage::Local},
    };

    assertThrowsSSA(
        [&] {
            placePromotableMemoryPhis(
                cfg,
                dominance,
                slots,
                {SSAMemoryDefinition{1, 1}});
        },
        "invalid slot");
    assertThrowsSSA(
        [&] {
            placePromotableMemoryPhis(
                cfg,
                dominance,
                slots,
                {SSAMemoryDefinition{0, cfg.blocks.size()}});
        },
        "invalid block");
}

} // namespace

int main()
{
    test_local_definitions_place_join_phi();
    test_loop_backedge_places_header_phi();
    test_unreachable_definitions_are_ignored();
    test_synthetic_exit_never_receives_a_phi();
    test_only_local_slots_are_promoted_and_output_is_deterministic();
    test_invalid_definition_references_are_rejected();
    return 0;
}
