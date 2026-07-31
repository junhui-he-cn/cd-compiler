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

void assertSourceSpanEqual(
    const std::optional<SourceSpan>& expected,
    const std::optional<SourceSpan>& actual)
{
    assert(expected.has_value() == actual.has_value());
    if (!expected) {
        return;
    }
    assert(expected->source == actual->source);
    assert(expected->line == actual->line);
    assert(expected->column == actual->column);
    assert(expected->range.has_value() == actual->range.has_value());
    if (expected->range) {
        assert(expected->range->start == actual->range->start);
        assert(expected->range->end == actual->range->end);
    }
}

void assertIRInstructionEqual(
    const IRInstruction& expected,
    const IRInstruction& actual)
{
    assert(expected.op == actual.op);
    assert(expected.dest.has_value() == actual.dest.has_value());
    if (expected.dest) {
        assert(expected.dest->index == actual.dest->index);
    }
    assert(expected.left.has_value() == actual.left.has_value());
    if (expected.left) {
        assert(expected.left->index == actual.left->index);
    }
    assert(expected.right.has_value() == actual.right.has_value());
    if (expected.right) {
        assert(expected.right->index == actual.right->index);
    }
    assert(expected.arguments.size() == actual.arguments.size());
    for (std::size_t index = 0; index < expected.arguments.size(); ++index) {
        assert(expected.arguments[index].index == actual.arguments[index].index);
    }
    assert(expected.operand == actual.operand);
    assert(expected.operands == actual.operands);
    assert(expected.typeNameOperand == actual.typeNameOperand);
    assert(expected.variantNameOperand == actual.variantNameOperand);
    assert(expected.bindingId == actual.bindingId);
    assertSourceSpanEqual(expected.span, actual.span);
}

void assertBindingVectorEqual(
    const std::vector<IRBinding>& expected,
    const std::vector<IRBinding>& actual)
{
    assert(expected.size() == actual.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        assert(expected[index].bindingId == actual[index].bindingId);
        assert(expected[index].resolvedName == actual[index].resolvedName);
        assert(expected[index].storage == actual[index].storage);
    }
}

void assertProgramMetadataUnchanged(
    const IRProgram& expected,
    const IRProgram& actual)
{
    assert(expected.constants().size() == actual.constants().size());
    for (std::size_t index = 0; index < expected.constants().size(); ++index) {
        assert(valuesEqual(expected.constants()[index], actual.constants()[index]));
    }
    assert(expected.names() == actual.names());
    assert(expected.sources().size() == actual.sources().size());
    for (std::size_t index = 0; index < expected.sources().size(); ++index) {
        const SourceFile& source = expected.sources()[index];
        const SourceFile& rebuilt = actual.sources()[index];
        assert(source.path == rebuilt.path);
        assert(source.text == rebuilt.text);
        assert(source.id == rebuilt.id);
        assert(source.moduleIdentity == rebuilt.moduleIdentity);
    }
    assertBindingVectorEqual(expected.bindings(), actual.bindings());
}

void test_program_adapter_preserves_order_and_o0_round_trip()
{
    IRProgram input;
    SourceFile source;
    source.path = "main.cd";
    source.text = "fun outer() { return nil; }";
    source.id = SourceFileId{5};
    source.moduleIdentity = "main-module";
    input.setSources({source});
    input.addName("outer#1");
    input.addName("field");
    input.addBinding(IRBinding{BindingId{10}, "outer#1", BindingStorageClass::Module});
    input.addBinding(IRBinding{BindingId{11}, "value#11", BindingStorageClass::Local});

    input.beginFunction("inner", {"value"});
    input.addFunctionBinding(IRBinding{BindingId{11}, "value#11", BindingStorageClass::Local});
    input.setCurrentSpan(SourceSpan{0, 1, 1, SourceSpanRange{0, 1}});
    const IRRegister innerValue = input.emitConstant(Value::number(4.0));
    input.setCurrentSpan(SourceSpan{0, 1, 2, SourceSpanRange{1, 2}});
    input.emitReturn(innerValue);
    const std::size_t innerIndex = input.endFunction();

    input.beginFunction("outer", {});
    input.setCurrentSpan(SourceSpan{0, 1, 3, SourceSpanRange{2, 3}});
    const IRRegister nested = input.emitMakeFunction(innerIndex);
    input.setCurrentSpan(SourceSpan{0, 1, 4, SourceSpanRange{3, 4}});
    input.emitReturn(nested);
    const std::size_t outerIndex = input.endFunction();

    input.setCurrentSpan(SourceSpan{0, 1, 5, SourceSpanRange{4, 5}});
    const IRRegister functionValue = input.emitMakeFunction(outerIndex);
    input.setCurrentSpan(SourceSpan{0, 1, 6, SourceSpanRange{5, 6}});
    input.emitReturn(functionValue);
    input.addModuleDependency(
        IRModuleDependency{7, ModuleGraphEdgeKind::Import, "./dep.cd", 1});

    const SSADeSSAProgramResult result = optimizeIRProgram(input, SSAOptimizationLevel::O0);
    result.verify(input);
    assert(result.functions.size() == 2);
    assert(result.functions[0].function.name == "inner");
    assert(result.functions[1].function.name == "outer");
    assert(result.mainStream.moduleDependencies.size() == 1);
    assert(result.mainStream.moduleDependencies.front().instructionOffset == 1);
    assert(result.mainStats.passesRun == 0);
    assert(result.functionStats.size() == 2);
    assert(result.functionStats[0].passesRun == 0);
    assert(result.functionStats[1].passesRun == 0);

    assert(result.mainStream.function.instructions.size() == input.instructions().size());
    for (std::size_t index = 0; index < input.instructions().size(); ++index) {
        assertIRInstructionEqual(
            input.instructions()[index],
            result.mainStream.function.instructions[index]);
    }
    for (std::size_t function = 0; function < input.functions().size(); ++function) {
        assert(result.functions[function].function.instructions.size()
            == input.functions()[function].instructions.size());
        for (std::size_t index = 0;
             index < input.functions()[function].instructions.size();
             ++index) {
            assertIRInstructionEqual(
                input.functions()[function].instructions[index],
                result.functions[function].function.instructions[index]);
        }
    }

    const IRProgram rebuilt = result.rebuild(input);
    assertProgramMetadataUnchanged(input, rebuilt);
    assert(rebuilt.instructions().size() == input.instructions().size());
    assert(rebuilt.functions().size() == input.functions().size());
    assert(rebuilt.moduleDependencies().size() == input.moduleDependencies().size());
    for (std::size_t index = 0; index < input.instructions().size(); ++index) {
        assertIRInstructionEqual(input.instructions()[index], rebuilt.instructions()[index]);
    }
    for (std::size_t index = 0; index < input.functions().size(); ++index) {
        assert(rebuilt.functions()[index].name == input.functions()[index].name);
        assert(rebuilt.functions()[index].parameters == input.functions()[index].parameters);
        assert(rebuilt.functions()[index].registerCount
            == input.functions()[index].registerCount);
        assertBindingVectorEqual(
            input.functions()[index].bindings,
            rebuilt.functions()[index].bindings);
    }
}

void test_program_adapter_o1_preserves_metadata_and_function_indices()
{
    IRProgram input;
    SourceFile source;
    source.path = "optimized.cd";
    source.text = "fun outer() { return nil; }";
    source.id = SourceFileId{8};
    source.moduleIdentity = "optimized-module";
    input.setSources({source});
    input.addName("outer#20");
    input.addName("inner#21");
    input.addBinding(IRBinding{BindingId{20}, "outer#20", BindingStorageClass::Module});
    input.addBinding(IRBinding{BindingId{21}, "inner#21", BindingStorageClass::Captured});

    input.beginFunction("inner", {"value"});
    input.addFunctionBinding(IRBinding{BindingId{21}, "inner#21", BindingStorageClass::Captured});
    input.setCurrentSpan(SourceSpan{0, 2, 1, SourceSpanRange{0, 1}});
    const IRRegister innerLive = input.emitConstant(Value::number(1.0));
    input.setCurrentSpan(SourceSpan{0, 2, 2, SourceSpanRange{1, 2}});
    input.emitConstant(Value::number(2.0));
    input.setCurrentSpan(SourceSpan{0, 2, 3, SourceSpanRange{2, 3}});
    input.emitReturn(innerLive);
    const std::size_t innerIndex = input.endFunction();

    input.beginFunction("outer", {});
    input.addFunctionBinding(IRBinding{BindingId{20}, "outer#20", BindingStorageClass::Module});
    input.setCurrentSpan(SourceSpan{0, 3, 1, SourceSpanRange{3, 4}});
    const IRRegister nested = input.emitMakeFunction(innerIndex);
    input.setCurrentSpan(SourceSpan{0, 3, 2, SourceSpanRange{4, 5}});
    input.emitConstant(Value::number(3.0));
    input.setCurrentSpan(SourceSpan{0, 3, 3, SourceSpanRange{5, 6}});
    input.emitReturn(nested);
    const std::size_t outerIndex = input.endFunction();

    input.setCurrentSpan(SourceSpan{0, 4, 1, SourceSpanRange{6, 7}});
    input.emitConstant(Value::number(4.0));
    input.setCurrentSpan(SourceSpan{0, 4, 2, SourceSpanRange{7, 8}});
    const IRRegister functionValue = input.emitMakeFunction(outerIndex);
    input.setCurrentSpan(SourceSpan{0, 4, 3, SourceSpanRange{8, 9}});
    input.emitReturn(functionValue);
    input.addModuleDependency(
        IRModuleDependency{9, ModuleGraphEdgeKind::ReExport, "./dep.cd", 2});

    // The four emitted values also populate the shared constant pool; the
    // pool itself is part of the program-level metadata contract.
    assert(input.constants().size() == 4);

    const SSADeSSAProgramResult result = optimizeIRProgram(input, SSAOptimizationLevel::O1);
    result.verify(input);
    assert(result.mainStats.instructionsRemoved == 1);
    assert(result.functionStats.size() == 2);
    assert(result.functionStats[0].instructionsRemoved == 1);
    assert(result.functionStats[1].instructionsRemoved == 1);
    assert(result.mainStream.function.instructions.size() == 2);
    assert(result.mainStream.function.instructions[0].op == IROp::MakeFunction);
    assert(result.mainStream.function.instructions[0].operand == outerIndex);
    assert(result.mainStream.moduleDependencies.front().instructionOffset == 1);
    assert(result.functions[0].function.instructions.size() == 2);
    assert(result.functions[0].function.instructions[0].op == IROp::Constant);
    assert(result.functions[1].function.instructions.size() == 2);
    assert(result.functions[1].function.instructions[0].op == IROp::MakeFunction);
    assert(result.functions[1].function.instructions[0].operand == innerIndex);

    const IRProgram rebuilt = result.rebuild(input);
    assertProgramMetadataUnchanged(input, rebuilt);
    assert(rebuilt.moduleDependencies().front().instructionOffset == 1);
    assert(rebuilt.functions().size() == 2);
    assert(rebuilt.functions()[0].name == "inner");
    assert(rebuilt.functions()[1].name == "outer");
    assert(rebuilt.instructions()[0].op == IROp::MakeFunction);
    assert(rebuilt.instructions()[0].operand == outerIndex);
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
    test_program_adapter_preserves_order_and_o0_round_trip();
    test_program_adapter_o1_preserves_metadata_and_function_indices();
    test_o0_is_verified_identity();
    test_o1_propagates_copy_and_removes_copy_instruction();
    test_o1_simplifies_trivial_phi_when_source_dominates_join();
    test_o1_removes_unused_pure_definition();
    test_o1_retains_trapping_pure_result_when_unused();
    test_o1_rejects_invalid_input_before_transforming();
}
