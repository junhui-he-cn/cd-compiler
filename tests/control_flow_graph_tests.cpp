#include "ControlFlowGraph.hpp"

#include <cassert>
#include <functional>
#include <optional>
#include <stdexcept>
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

void assertThrowsCFG(const std::function<void()>& action, const std::string& fragment)
{
    try {
        action();
    } catch (const ControlFlowGraphError& error) {
        assert(std::string(error.what()).find(fragment) != std::string::npos);
        return;
    }
    assert(false && "expected ControlFlowGraphError");
}

void test_empty_stream_has_entry_and_exit()
{
    const ControlFlowGraph graph = buildControlFlowGraph(std::vector<IRInstruction>{});

    assert(graph.instructionCount == 0);
    assert(graph.blocks.size() == 1);
    assert(graph.entryBlock == graph.exitBlock);
    assert(graph.blocks[graph.exitBlock].syntheticExit);
    assert(graph.blocks[graph.exitBlock].reachable);
    graph.verify();
}

void test_diamond_has_deterministic_blocks_and_edges()
{
    const std::vector<IRInstruction> instructions{
        jump(IROp::JumpIfFalse, 3),
        instruction(IROp::Constant),
        jump(IROp::Jump, 4),
        instruction(IROp::Constant),
        instruction(IROp::Return),
    };
    const ControlFlowGraph graph = buildControlFlowGraph(instructions);

    assert(graph.blocks.size() == 5);
    assert(graph.blocks[0].firstInstruction == 0);
    assert(graph.blocks[0].endInstruction == 1);
    assert(graph.blocks[1].firstInstruction == 1);
    assert(graph.blocks[1].endInstruction == 3);
    assert(graph.blocks[2].firstInstruction == 3);
    assert(graph.blocks[2].endInstruction == 4);
    assert(graph.blocks[3].firstInstruction == 4);
    assert(graph.blocks[3].endInstruction == 5);
    assert(graph.blocks[4].syntheticExit);
    assert(graph.blocks[0].successors == std::vector<CFGBlockId>({1, 2}));
    assert(graph.blocks[1].successors == std::vector<CFGBlockId>({3}));
    assert(graph.blocks[2].successors == std::vector<CFGBlockId>({3}));
    assert(graph.blocks[3].successors == std::vector<CFGBlockId>({4}));
    assert(graph.blocks[4].successors.empty());
    assert(graph.blocks[3].predecessors == std::vector<CFGBlockId>({1, 2}));
    assert(graph.blockForInstruction(0) == std::optional<CFGBlockId>(0));
    assert(graph.blockForInstruction(4) == std::optional<CFGBlockId>(3));
    assert(!graph.blockForInstruction(5).has_value());
    graph.verify();
}

void test_loop_backedge_and_reachability_are_recorded()
{
    const std::vector<IRInstruction> instructions{
        jump(IROp::JumpIfFalse, 3),
        instruction(IROp::Print),
        jump(IROp::Jump, 0),
        instruction(IROp::Return),
    };
    const ControlFlowGraph graph = buildControlFlowGraph(instructions);

    assert(graph.blocks.size() == 4);
    assert(graph.blocks[1].successors == std::vector<CFGBlockId>({0}));
    assert(graph.blocks[0].predecessors == std::vector<CFGBlockId>({1}));
    assert(graph.blocks[0].reachable);
    assert(graph.blocks[1].reachable);
    assert(graph.blocks[2].reachable);
    assert(graph.blocks[3].reachable);
    graph.verify();
}

void test_unreachable_block_is_retained_for_later_simplification()
{
    const std::vector<IRInstruction> instructions{
        jump(IROp::Jump, 3),
        instruction(IROp::Constant),
        instruction(IROp::Print),
        instruction(IROp::Return),
    };
    const ControlFlowGraph graph = buildControlFlowGraph(instructions);

    assert(graph.blocks.size() == 4);
    assert(!graph.blocks[1].reachable);
    assert(graph.blocks[0].reachable);
    assert(graph.blocks[2].reachable);
    assert(graph.blocks[3].reachable);
    graph.verify();
}

void test_dependency_offsets_are_attached_to_ordered_anchors()
{
    const std::vector<IRInstruction> instructions{
        instruction(IROp::Constant),
        instruction(IROp::Return),
    };
    const std::vector<IRModuleDependency> dependencies{
        IRModuleDependency{7, ModuleGraphEdgeKind::Import, "./lib.cd", 1},
        IRModuleDependency{8, ModuleGraphEdgeKind::ReExport, "./api.cd", 2},
    };
    const ControlFlowGraph graph = buildControlFlowGraph(instructions, dependencies);

    assert(graph.dependencyAnchors.size() == 2);
    assert(graph.dependencyAnchors[0].dependency.importedModuleId == 7);
    assert(graph.dependencyAnchors[0].block == 0);
    assert(graph.dependencyAnchors[0].offsetInBlock == 1);
    assert(graph.dependencyAnchors[1].dependency.importedModuleId == 8);
    assert(graph.dependencyAnchors[1].block == graph.exitBlock);
    assert(graph.dependencyAnchors[1].offsetInBlock == 0);
    graph.verify();
}

void test_program_and_function_overloads_share_cfg_rules()
{
    IRProgram program;
    const IRRegister value = program.emitConstant(Value::number(1));
    program.emitReturn(value);
    program.addModuleDependency(
        IRModuleDependency{9, ModuleGraphEdgeKind::Import, "./tail.cd", program.instructionCount()});

    const ControlFlowGraph mainGraph = buildControlFlowGraph(program);
    assert(mainGraph.instructionCount == 2);
    assert(mainGraph.dependencyAnchors.size() == 1);
    assert(mainGraph.dependencyAnchors.front().block == mainGraph.exitBlock);
    mainGraph.verify();

    IRFunction function;
    function.name = "demo";
    function.instructions.push_back(instruction(IROp::Return));
    function.registerCount = 1;
    const ControlFlowGraph functionGraph = buildControlFlowGraph(function);
    assert(functionGraph.instructionCount == 1);
    assert(functionGraph.blocks.size() == 2);
    functionGraph.verify();
}

void test_invalid_targets_and_dependency_offsets_are_rejected()
{
    assertThrowsCFG(
        [] {
            buildControlFlowGraph({jump(IROp::Jump, 2)});
        },
        "jump target");

    assertThrowsCFG(
        [] {
            buildControlFlowGraph(
                {instruction(IROp::Return)},
                {IRModuleDependency{1, ModuleGraphEdgeKind::Import, "./lib.cd", 2}});
        },
        "dependency offset");
}

void test_verifier_rejects_broken_predecessor_symmetry()
{
    ControlFlowGraph graph = buildControlFlowGraph({instruction(IROp::Return)});
    graph.blocks[0].successors.clear();
    assertThrowsCFG([&graph] { graph.verify(); }, "predecessor");
}

} // namespace

int main()
{
    test_empty_stream_has_entry_and_exit();
    test_diamond_has_deterministic_blocks_and_edges();
    test_loop_backedge_and_reachability_are_recorded();
    test_unreachable_block_is_retained_for_later_simplification();
    test_dependency_offsets_are_attached_to_ordered_anchors();
    test_program_and_function_overloads_share_cfg_rules();
    test_invalid_targets_and_dependency_offsets_are_rejected();
    test_verifier_rejects_broken_predecessor_symmetry();
    return 0;
}
