#include "Optimizer.hpp"

#include <cassert>
#include <functional>
#include <string>
#include <vector>

namespace {

IRInstruction cfgInstruction(IROp op, std::size_t operand = 0)
{
    return IRInstruction{op, std::nullopt, std::nullopt, std::nullopt, {}, operand};
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

SSAInstruction useValue(IROp op, SSAValueId value, std::size_t originalInstruction)
{
    SSAInstruction instruction;
    instruction.op = op;
    instruction.left = value;
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

SSAInstruction binaryInstruction(
    IROp op,
    SSAValueId result,
    SSAValueId left,
    SSAValueId right,
    std::size_t originalInstruction)
{
    SSAInstruction instruction = valueInstruction(op, result, originalInstruction);
    instruction.left = left;
    instruction.right = right;
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

void test_o0_is_verified_identity()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::Return),
    });
    SSAFunction input = makeSSAFunction(cfg);
    input.blocks[0].instructions.push_back(valueInstruction(IROp::Constant, 0, 0));
    input.blocks[0].instructions.push_back(useValue(IROp::Return, 0, 1));

    const SSAOptimizationResult result = optimizeSSA(
        cfg,
        input,
        SSAOptimizationLevel::O0);
    assert(result.function.blocks.size() == input.blocks.size());
    assert(result.function.parameters.size() == input.parameters.size());
    assert(result.function.blocks[0].instructions.size()
        == input.blocks[0].instructions.size());
    assert(result.function.blocks[0].instructions[0].op == IROp::Constant);
    assert(result.function.blocks[0].instructions[1].op == IROp::Return);
    assert(result.stats.copiesPropagated == 0);
    assert(result.stats.phisSimplified == 0);
    assert(result.stats.instructionsRemoved == 0);
    assert(result.stats.passesRun == 0);
    assert(ssaOptimizationPipelineFingerprint(SSAOptimizationLevel::O0)
        == "m7-ssa-o0-v1");
}

void test_o1_propagates_copy_and_removes_copy_instruction()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::Copy),
        cfgInstruction(IROp::Return),
    });
    SSAFunction input = makeSSAFunction(cfg);
    input.blocks[0].instructions.push_back(valueInstruction(IROp::Constant, 0, 0));
    SSAInstruction copy = valueInstruction(IROp::Copy, 1, 1);
    copy.left = 0;
    input.blocks[0].instructions.push_back(copy);
    input.blocks[0].instructions.push_back(useValue(IROp::Return, 1, 2));

    const SSAOptimizationResult result = optimizeSSA(
        cfg,
        input,
        SSAOptimizationLevel::O1);
    assert(result.function.blocks[0].instructions.size() == 2);
    assert(result.function.blocks[0].instructions[0].op == IROp::Constant);
    assert(result.function.blocks[0].instructions[1].op == IROp::Return);
    assert(result.function.blocks[0].instructions[1].left == std::optional<SSAValueId>(0));
    assert(result.stats.copiesPropagated == 1);
    assert(result.stats.phisSimplified == 0);
    assert(result.stats.instructionsRemoved == 1);
    assert(result.stats.passesRun == 2);
    result.function.verify(cfg);
}

void test_o1_simplifies_trivial_phi_when_source_dominates_join()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::JumpIfFalse, 3),
        cfgInstruction(IROp::Jump, 4),
        cfgInstruction(IROp::Jump, 4),
        cfgInstruction(IROp::Jump, 4),
        cfgInstruction(IROp::Return),
    });
    SSAFunction input = makeSSAFunction(cfg);
    input.parameters.push_back(SSAParameter{0, cfg.entryBlock});
    SSAInstruction branch = plainInstruction(IROp::JumpIfFalse, 0);
    branch.left = 0;
    branch.operand = 3;
    input.blocks[0].instructions.push_back(branch);
    SSAInstruction firstJump = plainInstruction(IROp::Jump, 1);
    firstJump.operand = 4;
    input.blocks[1].instructions.push_back(firstJump);
    SSAInstruction secondJump = plainInstruction(IROp::Jump, 2);
    secondJump.operand = 4;
    input.blocks[2].instructions.push_back(secondJump);
    SSAInstruction thirdJump = plainInstruction(IROp::Jump, 3);
    thirdJump.operand = 4;
    input.blocks[3].instructions.push_back(thirdJump);
    input.blocks[4].phis.push_back(SSAPhi{1, {{1, 0}, {2, 0}, {3, 0}}});
    input.blocks[4].instructions.push_back(useValue(IROp::Return, 1, 4));

    const SSAOptimizationResult result = optimizeSSA(
        cfg,
        input,
        SSAOptimizationLevel::O1);
    assert(result.function.blocks[4].phis.empty());
    assert(result.function.blocks[4].instructions.size() == 1);
    assert(result.function.blocks[4].instructions.front().left
        == std::optional<SSAValueId>(0));
    assert(result.stats.phisSimplified == 1);
    assert(result.stats.instructionsRemoved == 0);
    result.function.verify(cfg);
}

void test_o1_removes_unused_pure_definition()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::Constant),
        cfgInstruction(IROp::Return),
    });
    SSAFunction input = makeSSAFunction(cfg);
    input.parameters.push_back(SSAParameter{0, cfg.entryBlock});
    input.blocks[0].instructions.push_back(valueInstruction(IROp::Constant, 1, 0));
    input.blocks[0].instructions.push_back(useValue(IROp::Return, 0, 1));

    const SSAOptimizationResult result = optimizeSSA(
        cfg,
        input,
        SSAOptimizationLevel::O1);
    assert(result.function.blocks[0].instructions.size() == 1);
    assert(result.function.blocks[0].instructions.front().op == IROp::Return);
    assert(result.stats.instructionsRemoved == 1);
    result.function.verify(cfg);
}

void test_o1_retains_trapping_pure_result_when_unused()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::Divide),
        cfgInstruction(IROp::Return),
    });
    SSAFunction input = makeSSAFunction(cfg);
    input.parameters.push_back(SSAParameter{0, cfg.entryBlock});
    input.blocks[0].instructions.push_back(
        binaryInstruction(IROp::Divide, 1, 0, 0, 0));
    input.blocks[0].instructions.push_back(useValue(IROp::Return, 0, 1));

    const SSAOptimizationResult result = optimizeSSA(
        cfg,
        input,
        SSAOptimizationLevel::O1);
    assert(result.function.blocks[0].instructions.size() == 2);
    assert(result.stats.instructionsRemoved == 0);
    result.function.verify(cfg);
}

void test_o1_rejects_invalid_input_before_transforming()
{
    const ControlFlowGraph cfg = buildControlFlowGraph({
        cfgInstruction(IROp::Return),
    });
    SSAFunction input = makeSSAFunction(cfg);
    input.blocks[0].instructions.push_back(useValue(IROp::Return, 7, 0));

    assertThrowsSSA(
        [&] { optimizeSSA(cfg, input, SSAOptimizationLevel::O1); },
        "undefined SSA value");
}

} // namespace

int main()
{
    test_o0_is_verified_identity();
    test_o1_propagates_copy_and_removes_copy_instruction();
    test_o1_simplifies_trivial_phi_when_source_dominates_join();
    test_o1_removes_unused_pure_definition();
    test_o1_retains_trapping_pure_result_when_unused();
    test_o1_rejects_invalid_input_before_transforming();
}
