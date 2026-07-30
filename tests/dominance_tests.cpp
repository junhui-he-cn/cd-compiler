#include "Dominance.hpp"

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

void assertThrowsDominance(
    const std::function<void()>& action,
    const std::string& fragment)
{
    try {
        action();
    } catch (const DominanceError& error) {
        assert(std::string(error.what()).find(fragment) != std::string::npos);
        return;
    }
    assert(false && "expected DominanceError");
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

void test_straight_line_dominance_includes_synthetic_exit()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        instruction(IROp::Constant),
        instruction(IROp::Print),
        instruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);

    assert(dominance.dominators == std::vector<std::vector<CFGBlockId>>({
        {0},
        {0, 1},
    }));
    assert(dominance.immediateDominators == std::vector<std::optional<CFGBlockId>>({
        std::nullopt,
        CFGBlockId{0},
    }));
    assert(dominance.dominanceChildren == std::vector<std::vector<CFGBlockId>>({
        {1},
        {},
    }));
    assert(dominance.dominanceFrontiers == std::vector<std::vector<CFGBlockId>>({
        {},
        {},
    }));
    assert(dominance.dominates(0, 1));
    assert(dominance.dominates(1, 1));
    assert(!dominance.dominates(1, 0));
    dominance.verify(cfg);
}

void test_diamond_has_join_frontiers()
{
    const ControlFlowGraph cfg = makeDiamondCFG();
    const DominanceInfo dominance = buildDominanceInfo(cfg);

    assert(dominance.dominators == std::vector<std::vector<CFGBlockId>>({
        {0},
        {0, 1},
        {0, 2},
        {0, 3},
        {0, 3, 4},
    }));
    assert(dominance.immediateDominators == std::vector<std::optional<CFGBlockId>>({
        std::nullopt,
        CFGBlockId{0},
        CFGBlockId{0},
        CFGBlockId{0},
        CFGBlockId{3},
    }));
    assert(dominance.dominanceChildren[0] == std::vector<CFGBlockId>({1, 2, 3}));
    assert(dominance.dominanceFrontiers[1] == std::vector<CFGBlockId>({3}));
    assert(dominance.dominanceFrontiers[2] == std::vector<CFGBlockId>({3}));
    assert(dominance.dominanceFrontiers[0].empty());
    dominance.verify(cfg);
}

void test_loop_backedge_adds_header_to_body_frontier()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        jump(IROp::JumpIfFalse, 3),
        instruction(IROp::Print),
        jump(IROp::Jump, 0),
        instruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);

    assert(dominance.dominators[0] == std::vector<CFGBlockId>({0}));
    assert(dominance.dominators[1] == std::vector<CFGBlockId>({0, 1}));
    assert(dominance.dominators[2] == std::vector<CFGBlockId>({0, 2}));
    assert(dominance.dominators[3] == std::vector<CFGBlockId>({0, 2, 3}));
    assert(dominance.immediateDominators[1] == std::optional<CFGBlockId>(0));
    assert(dominance.immediateDominators[2] == std::optional<CFGBlockId>(0));
    assert(dominance.immediateDominators[3] == std::optional<CFGBlockId>(2));
    assert(dominance.dominanceFrontiers[1] == std::vector<CFGBlockId>({0}));
    assert(dominance.dominanceFrontiers[0] == std::vector<CFGBlockId>({0}));
    dominance.verify(cfg);
}

void test_unreachable_blocks_have_no_dominance_metadata()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        jump(IROp::Jump, 3),
        instruction(IROp::Constant),
        instruction(IROp::Print),
        instruction(IROp::Return),
    });
    const DominanceInfo dominance = buildDominanceInfo(cfg);

    assert(!cfg.blocks[1].reachable);
    assert(dominance.dominators[1].empty());
    assert(!dominance.immediateDominators[1]);
    assert(dominance.dominanceChildren[1].empty());
    assert(dominance.dominanceFrontiers[1].empty());
    assert(!dominance.dominates(1, 2));
    dominance.verify(cfg);
}

void test_empty_cfg_entry_is_also_the_exit()
{
    const ControlFlowGraph cfg = buildControlFlowGraph(std::vector<IRInstruction>{});
    const DominanceInfo dominance = buildDominanceInfo(cfg);

    assert(dominance.dominators == std::vector<std::vector<CFGBlockId>>({{0}}));
    assert(dominance.immediateDominators == std::vector<std::optional<CFGBlockId>>({
        std::nullopt,
    }));
    assert(dominance.dominates(0, 0));
    dominance.verify(cfg);
}

void test_verifier_rejects_tampered_dominance_metadata()
{
    const ControlFlowGraph cfg = makeDiamondCFG();
    DominanceInfo dominance = buildDominanceInfo(cfg);
    dominance.immediateDominators[3] = CFGBlockId{1};

    assertThrowsDominance(
        [&dominance, &cfg] { dominance.verify(cfg); },
        "invalid immediate dominator");
}

void test_verifier_rejects_invalid_immediate_dominator_id()
{
    const ControlFlowGraph cfg = makeDiamondCFG();
    DominanceInfo dominance = buildDominanceInfo(cfg);
    dominance.immediateDominators[3] = cfg.blocks.size();

    assertThrowsDominance(
        [&dominance, &cfg] { dominance.verify(cfg); },
        "invalid block");
}

} // namespace

int main()
{
    test_straight_line_dominance_includes_synthetic_exit();
    test_diamond_has_join_frontiers();
    test_loop_backedge_adds_header_to_body_frontier();
    test_unreachable_blocks_have_no_dominance_metadata();
    test_empty_cfg_entry_is_also_the_exit();
    test_verifier_rejects_tampered_dominance_metadata();
    test_verifier_rejects_invalid_immediate_dominator_id();
    return 0;
}
