#include "Optimizer.hpp"

#include <cassert>
#include <functional>
#include <limits>
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

void assertConstantKind(
    const SSAConstantEvaluation& result,
    SSAConstantEvaluationKind expected)
{
    assert(result.kind == expected);
    assert(!result.value.has_value());
}

void test_constant_evaluation_folds_only_representable_successes()
{
    const SSAConstantEvaluation sum = evaluateSSAConstantBinary(
        IROp::Add,
        Value::number(2.0),
        Value::number(3.0));
    assert(sum.isFolded());
    assert(sum.value->type() == Value::Type::Number);
    assert(sum.value->asNumber() == 5.0);

    const SSAConstantEvaluation text = evaluateSSAConstantBinary(
        IROp::Add,
        Value::string("hello "),
        Value::string("world"));
    assert(text.isFolded());
    assert(text.value->asString() == "hello world");

    const SSAConstantEvaluation truth = evaluateSSAConstantUnary(
        IROp::Not,
        Value::nil());
    assert(truth.isFolded());
    assert(truth.value->type() == Value::Type::Bool);
    assert(truth.value->asBool());

    const SSAConstantEvaluation comparison = evaluateSSAConstantBinary(
        IROp::Less,
        Value::number(2.0),
        Value::number(3.0));
    assert(comparison.isFolded());
    assert(comparison.value->asBool());
}

void test_constant_evaluation_keeps_runtime_traps_runtime()
{
    const SSAConstantEvaluation divisionByZero = evaluateSSAConstantBinary(
        IROp::Divide,
        Value::number(1.0),
        Value::number(0.0));
    assertConstantKind(divisionByZero, SSAConstantEvaluationKind::RuntimeTrap);

    const SSAConstantEvaluation typeMismatch = evaluateSSAConstantBinary(
        IROp::Subtract,
        Value::string("one"),
        Value::number(1.0));
    assertConstantKind(typeMismatch, SSAConstantEvaluationKind::RuntimeTrap);

    const SSAConstantEvaluation comparisonMismatch = evaluateSSAConstantBinary(
        IROp::Greater,
        Value::boolean(true),
        Value::number(1.0));
    assertConstantKind(comparisonMismatch, SSAConstantEvaluationKind::RuntimeTrap);
}

void test_constant_evaluation_rejects_nonfinite_and_nonprimitive_values()
{
    const SSAConstantEvaluation overflow = evaluateSSAConstantBinary(
        IROp::Multiply,
        Value::number(std::numeric_limits<double>::max()),
        Value::number(2.0));
    assertConstantKind(overflow, SSAConstantEvaluationKind::NonFinite);

    const SSAConstantEvaluation nonfiniteInput = evaluateSSAConstantUnary(
        IROp::Negate,
        Value::number(std::numeric_limits<double>::infinity()));
    assertConstantKind(nonfiniteInput, SSAConstantEvaluationKind::NonFinite);

    const SSAConstantEvaluation unsupportedValue = evaluateSSAConstantUnary(
        IROp::Not,
        Value::function(FunctionValue{}));
    assertConstantKind(unsupportedValue, SSAConstantEvaluationKind::Unsupported);

    const SSAConstantEvaluation unsupportedOp = evaluateSSAConstantUnary(
        IROp::Print,
        Value::boolean(true));
    assertConstantKind(unsupportedOp, SSAConstantEvaluationKind::Unsupported);
}

void test_internal_ir_adapter_preserves_o0_stream_contract()
{
    IRFunction input;
    input.name = "identity";
    input.parameters = {"value"};
    input.registerCount = 4;
    input.bindings = {IRBinding{BindingId{1}, "value#1", BindingStorageClass::Local}};

    IRInstruction constant;
    constant.op = IROp::Constant;
    constant.dest = IRRegister{0};
    constant.operand = 0;
    constant.span = SourceSpan{0, 1, 1, SourceSpanRange{0, 1}};
    input.instructions.push_back(constant);

    IRInstruction copy;
    copy.op = IROp::Copy;
    copy.dest = IRRegister{1};
    copy.left = IRRegister{0};
    copy.span = SourceSpan{0, 1, 2, SourceSpanRange{1, 2}};
    input.instructions.push_back(copy);

    IRInstruction result;
    result.op = IROp::Return;
    result.left = IRRegister{1};
    result.span = SourceSpan{0, 1, 3, SourceSpanRange{2, 3}};
    input.instructions.push_back(result);

    const SSADeSSAIRResult lowered = optimizeIRFunction(
        input,
        {IRModuleDependency{7, ModuleGraphEdgeKind::Import, "./dep.cd", 1}},
        SSAOptimizationLevel::O0);
    lowered.verify();
    assert(lowered.function.name == input.name);
    assert(lowered.function.parameters == input.parameters);
    assert(lowered.function.bindings.size() == 1);
    assert(lowered.function.registerCount == input.registerCount);
    assert(lowered.function.instructions.size() == input.instructions.size());
    assert(lowered.function.instructions[1].dest.has_value());
    assert(lowered.function.instructions[1].dest->index == copy.dest->index);
    assert(lowered.function.instructions[1].left.has_value());
    assert(lowered.function.instructions[1].left->index == copy.left->index);
    assert(lowered.function.instructions[0].span.has_value());
    assert(lowered.function.instructions[0].span->line == constant.span->line);
    assert(lowered.function.instructions[0].span->column == constant.span->column);
    assert(lowered.function.instructions[2].span.has_value());
    assert(lowered.function.instructions[2].span->line == result.span->line);
    assert(lowered.function.instructions[2].span->column == result.span->column);
    assert(lowered.moduleDependencies.size() == 1);
    assert(lowered.moduleDependencies.front().instructionOffset == 1);
    assert(!lowered.syntheticInstructions[0]);
    assert(!lowered.syntheticInstructions[1]);
    assert(!lowered.syntheticInstructions[2]);
}

void test_internal_ir_adapter_runs_o1_without_physical_register_allocation()
{
    IRFunction input;
    input.name = "dead";
    input.registerCount = 3;

    IRInstruction dead;
    dead.op = IROp::Constant;
    dead.dest = IRRegister{0};
    dead.operand = 0;
    input.instructions.push_back(dead);

    IRInstruction live;
    live.op = IROp::Constant;
    live.dest = IRRegister{1};
    live.operand = 1;
    input.instructions.push_back(live);

    IRInstruction result;
    result.op = IROp::Return;
    result.left = IRRegister{1};
    input.instructions.push_back(result);

    const SSADeSSAIRResult lowered = optimizeIRFunction(
        input,
        {},
        SSAOptimizationLevel::O1);
    lowered.verify();
    assert(lowered.function.instructions.size() == 2);
    assert(lowered.function.instructions[0].op == IROp::Constant);
    assert(lowered.function.instructions[0].dest.has_value());
    assert(lowered.function.instructions[0].dest->index == live.dest->index);
    assert(lowered.function.instructions[1].op == IROp::Return);
    assert(lowered.function.instructions[1].left.has_value());
    assert(lowered.function.instructions[1].left->index == result.left->index);
    assert(lowered.function.registerCount == input.registerCount);
    assert(!lowered.originalInstructionOffsets[0].has_value());
    assert(lowered.originalInstructionOffsets[1] == std::optional<std::size_t>(0));
    assert(lowered.originalInstructionOffsets[2] == std::optional<std::size_t>(1));
    assert(!lowered.syntheticInstructions[0]);
    assert(!lowered.syntheticInstructions[1]);
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
    test_constant_evaluation_folds_only_representable_successes();
    test_constant_evaluation_keeps_runtime_traps_runtime();
    test_constant_evaluation_rejects_nonfinite_and_nonprimitive_values();
    test_internal_ir_adapter_preserves_o0_stream_contract();
    test_internal_ir_adapter_runs_o1_without_physical_register_allocation();
    test_o0_is_verified_identity();
    test_o1_propagates_copy_and_removes_copy_instruction();
    test_o1_simplifies_trivial_phi_when_source_dominates_join();
    test_o1_removes_unused_pure_definition();
    test_o1_retains_trapping_pure_result_when_unused();
    test_o1_rejects_invalid_input_before_transforming();
}
