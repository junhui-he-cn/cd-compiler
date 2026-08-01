#include "Optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

using ValueAliases = std::unordered_map<SSAValueId, SSAValueId>;
using ValueDefinitions = std::unordered_map<SSAValueId, CFGBlockId>;
using KnownConstants = std::map<SSAValueId, Value>;

void normalizeKnownConditionBranches(
    IRFunction& function,
    const std::vector<Value>* constantPool,
    const std::map<SSAValueId, Value>& foldedConstants,
    SSAOptimizationStats& stats);

void pruneUnreachableIRInstructions(
    SSADeSSAIRResult& result,
    SSAOptimizationStats& stats);

void removeRedundantFallthroughJumps(
    SSADeSSAIRResult& result,
    SSAOptimizationStats& stats);

void threadEmptyJumpBlocks(
    SSADeSSAIRResult& result,
    SSAOptimizationStats& stats);

void mergeLinearBlocks(
    SSADeSSAIRResult& result,
    SSAOptimizationStats& stats);

std::optional<SSAValueId> resolveAlias(
    SSAValueId value,
    const ValueAliases& aliases)
{
    std::unordered_set<SSAValueId> visited;
    SSAValueId current = value;
    while (true) {
        const auto alias = aliases.find(current);
        if (alias == aliases.end()) {
            return current;
        }
        if (alias->second == current || !visited.insert(current).second) {
            return std::nullopt;
        }
        current = alias->second;
    }
}

void observeDefinition(
    ValueDefinitions& definitions,
    SSAValueId value,
    CFGBlockId block)
{
    if (!definitions.emplace(value, block).second) {
        throw SSAError(
            "SSA optimizer encountered duplicate definition for value "
            + std::to_string(value));
    }
}

ValueDefinitions collectDefinitions(
    const SSAFunction& function)
{
    ValueDefinitions definitions;
    for (const SSAParameter& parameter : function.parameters) {
        observeDefinition(definitions, parameter.value, parameter.block);
    }
    for (const SSABlock& block : function.blocks) {
        for (const SSAPhi& phi : block.phis) {
            observeDefinition(definitions, phi.result, block.id);
        }
        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.result) {
                observeDefinition(definitions, *instruction.result, block.id);
            }
        }
    }
    return definitions;
}

bool isPureValueInstruction(const SSAInstruction& instruction)
{
    return instruction.result.has_value() && irEffectSummary(instruction.op).isPure();
}

bool isSerializablePrimitive(const Value& value)
{
    switch (value.type()) {
    case Value::Type::Nil:
    case Value::Type::Bool:
    case Value::Type::String:
        return true;
    case Value::Type::Number:
        return std::isfinite(value.asNumber());
    default:
        return false;
    }
}

SSAConstantEvaluation constantStatus(SSAConstantEvaluationKind kind)
{
    return SSAConstantEvaluation{kind, std::nullopt};
}

SSAConstantEvaluation foldedConstant(Value value)
{
    return SSAConstantEvaluation{
        SSAConstantEvaluationKind::Folded,
        std::move(value),
    };
}

SSAConstantEvaluationKind validateConstantOperands(
    const Value& left,
    const Value* right = nullptr)
{
    if (!isSerializablePrimitive(left)
        || (right && !isSerializablePrimitive(*right))) {
        const Value* values[] = {&left, right};
        for (const Value* value : values) {
            if (value && value->type() == Value::Type::Number
                && !std::isfinite(value->asNumber())) {
                return SSAConstantEvaluationKind::NonFinite;
            }
        }
        return SSAConstantEvaluationKind::Unsupported;
    }
    return SSAConstantEvaluationKind::Folded;
}

std::optional<double> finiteNumber(
    const Value& value,
    SSAConstantEvaluation& failure)
{
    if (value.type() != Value::Type::Number) {
        failure = constantStatus(SSAConstantEvaluationKind::RuntimeTrap);
        return std::nullopt;
    }
    if (!std::isfinite(value.asNumber())) {
        failure = constantStatus(SSAConstantEvaluationKind::NonFinite);
        return std::nullopt;
    }
    return value.asNumber();
}

SSAConstantEvaluation finiteNumberResult(double value)
{
    if (!std::isfinite(value)) {
        return constantStatus(SSAConstantEvaluationKind::NonFinite);
    }
    return foldedConstant(Value::number(value));
}

void replaceOperand(
    std::optional<SSAValueId>& operand,
    const ValueAliases& aliases)
{
    if (!operand) {
        return;
    }
    const std::optional<SSAValueId> resolved = resolveAlias(*operand, aliases);
    if (resolved) {
        *operand = *resolved;
    }
}

void replaceOperands(
    SSAInstruction& instruction,
    const ValueAliases& aliases)
{
    replaceOperand(instruction.result, aliases);
    replaceOperand(instruction.left, aliases);
    replaceOperand(instruction.right, aliases);
    for (SSAValueId& argument : instruction.arguments) {
        const std::optional<SSAValueId> resolved = resolveAlias(argument, aliases);
        if (resolved) {
            argument = *resolved;
        }
    }
}

bool canSimplifyPhi(
    const ControlFlowGraph& cfg,
    const DominanceInfo& dominance,
    const ValueDefinitions& definitions,
    const SSABlock& block,
    const SSAPhi& phi,
    const ValueAliases& aliases,
    SSAValueId& replacement)
{
    if (!cfg.blocks[block.id].reachable || phi.incoming.empty()) {
        return false;
    }

    std::optional<SSAValueId> first;
    for (const SSAIncoming& incoming : phi.incoming) {
        const std::optional<SSAValueId> resolved = resolveAlias(incoming.value, aliases);
        if (!resolved) {
            return false;
        }
        if (!first) {
            first = *resolved;
        } else if (*first != *resolved) {
            return false;
        }
    }
    if (!first || *first == phi.result) {
        return false;
    }

    const auto definition = definitions.find(*first);
    if (definition == definitions.end() || definition->second == block.id
        || !cfg.blocks[definition->second].reachable
        || !dominance.dominates(definition->second, block.id)) {
        return false;
    }
    replacement = *first;
    return true;
}

std::set<SSAValueId> collectUsedValues(const SSAFunction& function)
{
    std::set<SSAValueId> used;
    for (const SSABlock& block : function.blocks) {
        for (const SSAPhi& phi : block.phis) {
            for (const SSAIncoming& incoming : phi.incoming) {
                used.insert(incoming.value);
            }
        }
        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.left) {
                used.insert(*instruction.left);
            }
            if (instruction.right) {
                used.insert(*instruction.right);
            }
            used.insert(instruction.arguments.begin(), instruction.arguments.end());
        }
    }
    return used;
}

void simplifyCopiesAndPhis(
    const ControlFlowGraph& cfg,
    SSAFunction& function,
    SSAOptimizationStats& stats)
{
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    const ValueDefinitions definitions = collectDefinitions(function);
    ValueAliases aliases;
    std::set<SSAValueId> propagatedCopies;
    std::set<SSAValueId> simplifiedPhis;

    for (const SSABlock& block : function.blocks) {
        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.op != IROp::Copy || !instruction.result || !instruction.left
                || *instruction.result == *instruction.left) {
                continue;
            }
            aliases[*instruction.result] = *instruction.left;
            propagatedCopies.insert(*instruction.result);
        }
    }

    const std::size_t maxIterations = function.blocks.size() + 1;
    for (std::size_t iteration = 0; iteration < maxIterations; ++iteration) {
        bool changed = false;
        for (const SSABlock& block : function.blocks) {
            for (const SSAPhi& phi : block.phis) {
                SSAValueId replacement = 0;
                if (!canSimplifyPhi(
                        cfg,
                        dominance,
                        definitions,
                        block,
                        phi,
                        aliases,
                        replacement)) {
                    continue;
                }
                const auto existing = aliases.find(phi.result);
                if (existing == aliases.end() || existing->second != replacement) {
                    aliases[phi.result] = replacement;
                    changed = true;
                }
                simplifiedPhis.insert(phi.result);
            }
        }
        if (!changed) {
            break;
        }
    }

    for (SSABlock& block : function.blocks) {
        for (SSAPhi& phi : block.phis) {
            for (SSAIncoming& incoming : phi.incoming) {
                const std::optional<SSAValueId> resolved
                    = resolveAlias(incoming.value, aliases);
                if (resolved) {
                    incoming.value = *resolved;
                }
            }
        }
        std::vector<SSAPhi> remainingPhis;
        remainingPhis.reserve(block.phis.size());
        for (const SSAPhi& phi : block.phis) {
            if (simplifiedPhis.find(phi.result) == simplifiedPhis.end()) {
                remainingPhis.push_back(phi);
            }
        }
        block.phis = std::move(remainingPhis);

        std::vector<SSAInstruction> remainingInstructions;
        remainingInstructions.reserve(block.instructions.size());
        for (const SSAInstruction& source : block.instructions) {
            if (source.op == IROp::Copy && source.result
                && propagatedCopies.find(*source.result) != propagatedCopies.end()) {
                ++stats.instructionsRemoved;
                continue;
            }
            SSAInstruction instruction = source;
            replaceOperands(instruction, aliases);
            remainingInstructions.push_back(instruction);
        }
        block.instructions = std::move(remainingInstructions);
    }

    stats.copiesPropagated += propagatedCopies.size();
    stats.phisSimplified += simplifiedPhis.size();
}

void eliminateDeadPureInstructions(
    SSAFunction& function,
    SSAOptimizationStats& stats)
{
    const std::set<SSAValueId> used = collectUsedValues(function);
    for (SSABlock& block : function.blocks) {
        std::vector<SSAInstruction> remaining;
        remaining.reserve(block.instructions.size());
        for (const SSAInstruction& instruction : block.instructions) {
            if (isPureValueInstruction(instruction)
                && used.find(*instruction.result) == used.end()) {
                ++stats.instructionsRemoved;
                continue;
            }
            remaining.push_back(instruction);
        }
        block.instructions = std::move(remaining);
    }
}

bool recordKnownConstant(
    KnownConstants& known,
    SSAValueId value,
    const Value& constant)
{
    const auto existing = known.find(value);
    if (existing == known.end()) {
        known.emplace(value, constant);
        return true;
    }
    if (!valuesEqual(existing->second, constant)) {
        existing->second = constant;
        return true;
    }
    return false;
}

bool constantForPhi(
    const SSAPhi& phi,
    const KnownConstants& known,
    Value& result)
{
    if (phi.incoming.empty()) {
        return false;
    }

    std::optional<Value> first;
    for (const SSAIncoming& incoming : phi.incoming) {
        const auto value = known.find(incoming.value);
        if (value == known.end()) {
            return false;
        }
        if (!first) {
            first = value->second;
        } else if (!valuesEqual(*first, value->second)) {
            return false;
        }
    }
    result = *first;
    return true;
}

void foldKnownConstantExpressions(
    SSAFunction& function,
    const std::vector<Value>* constantPool,
    SSAOptimizationStats& stats,
    std::map<SSAValueId, Value>& foldedConstants)
{
    KnownConstants known;
    const std::size_t maxIterations = function.blocks.size()
        + std::accumulate(
            function.blocks.begin(),
            function.blocks.end(),
            std::size_t{0},
            [](std::size_t count, const SSABlock& block) {
                return count + block.instructions.size() + block.phis.size();
            })
        + 1;

    for (std::size_t iteration = 0; iteration < maxIterations; ++iteration) {
        bool changed = false;
        for (SSABlock& block : function.blocks) {
            for (const SSAPhi& phi : block.phis) {
                Value value = Value::nil();
                if (constantForPhi(phi, known, value)) {
                    changed = recordKnownConstant(known, phi.result, value) || changed;
                }
            }

            for (SSAInstruction& instruction : block.instructions) {
                if (!instruction.result) {
                    continue;
                }

                const SSAValueId result = *instruction.result;
                if (instruction.op == IROp::Constant) {
                    const auto folded = foldedConstants.find(result);
                    if (folded != foldedConstants.end()) {
                        changed = recordKnownConstant(known, result, folded->second) || changed;
                    } else if (constantPool && instruction.operand < constantPool->size()) {
                        const Value& value = (*constantPool)[instruction.operand];
                        if (isSerializablePrimitive(value)) {
                            changed = recordKnownConstant(known, result, value) || changed;
                        }
                    }
                    continue;
                }

                if (instruction.op == IROp::Copy && instruction.left) {
                    const auto source = known.find(*instruction.left);
                    if (source != known.end()) {
                        changed = recordKnownConstant(known, result, source->second) || changed;
                    }
                    continue;
                }

                std::optional<SSAConstantEvaluation> evaluation;
                if ((instruction.op == IROp::Negate || instruction.op == IROp::Not)
                    && instruction.left) {
                    const auto operand = known.find(*instruction.left);
                    if (operand != known.end()) {
                        evaluation = evaluateSSAConstantUnary(instruction.op, operand->second);
                    }
                } else if (instruction.left && instruction.right) {
                    const auto left = known.find(*instruction.left);
                    const auto right = known.find(*instruction.right);
                    if (left != known.end() && right != known.end()) {
                        evaluation = evaluateSSAConstantBinary(
                            instruction.op,
                            left->second,
                            right->second);
                    }
                }

                if (!evaluation || !evaluation->isFolded()) {
                    continue;
                }

                const Value& value = *evaluation->value;
                if (instruction.op != IROp::Constant) {
                    instruction.op = IROp::Constant;
                    instruction.left.reset();
                    instruction.right.reset();
                    instruction.arguments.clear();
                    instruction.operands.clear();
                    instruction.typeNameOperand.reset();
                    instruction.variantNameOperand.reset();
                    // The replacement pool index is assigned when the
                    // program-level result is rebuilt.
                    instruction.operand = 0;
                    foldedConstants.insert_or_assign(result, value);
                    ++stats.constantsFolded;
                    changed = true;
                }
                changed = recordKnownConstant(known, result, value) || changed;
            }
        }
        if (!changed) {
            break;
        }
    }
}

void retainFoldedConstantsForOutput(
    const SSAFunction& function,
    std::map<SSAValueId, Value>& foldedConstants)
{
    std::set<SSAValueId> surviving;
    for (const SSABlock& block : function.blocks) {
        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.op == IROp::Constant && instruction.result) {
                surviving.insert(*instruction.result);
            }
        }
    }
    for (auto it = foldedConstants.begin(); it != foldedConstants.end();) {
        if (surviving.find(it->first) == surviving.end()) {
            it = foldedConstants.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

SSAConstantEvaluation evaluateSSAConstantUnary(IROp op, const Value& operand)
{
    const SSAConstantEvaluationKind operandKind = validateConstantOperands(operand);
    if (operandKind != SSAConstantEvaluationKind::Folded) {
        return constantStatus(operandKind);
    }

    switch (op) {
    case IROp::Negate: {
        SSAConstantEvaluation failure;
        const std::optional<double> number = finiteNumber(operand, failure);
        if (!number) {
            return failure;
        }
        return finiteNumberResult(-*number);
    }
    case IROp::Not:
        return foldedConstant(Value::boolean(!isTruthy(operand)));
    default:
        return constantStatus(SSAConstantEvaluationKind::Unsupported);
    }
}

SSAConstantEvaluation evaluateSSAConstantBinary(
    IROp op,
    const Value& left,
    const Value& right)
{
    const SSAConstantEvaluationKind operandKind = validateConstantOperands(left, &right);
    if (operandKind != SSAConstantEvaluationKind::Folded) {
        return constantStatus(operandKind);
    }

    switch (op) {
    case IROp::Add:
        if (left.type() == Value::Type::Number && right.type() == Value::Type::Number) {
            return finiteNumberResult(left.asNumber() + right.asNumber());
        }
        if (left.type() == Value::Type::String && right.type() == Value::Type::String) {
            return foldedConstant(Value::string(left.asString() + right.asString()));
        }
        return constantStatus(SSAConstantEvaluationKind::RuntimeTrap);
    case IROp::Subtract:
    case IROp::Multiply:
    case IROp::Divide: {
        SSAConstantEvaluation failure;
        const std::optional<double> leftNumber = finiteNumber(left, failure);
        if (!leftNumber) {
            return failure;
        }
        const std::optional<double> rightNumber = finiteNumber(right, failure);
        if (!rightNumber) {
            return failure;
        }
        if (op == IROp::Divide && *rightNumber == 0.0) {
            return constantStatus(SSAConstantEvaluationKind::RuntimeTrap);
        }
        if (op == IROp::Subtract) {
            return finiteNumberResult(*leftNumber - *rightNumber);
        }
        if (op == IROp::Multiply) {
            return finiteNumberResult(*leftNumber * *rightNumber);
        }
        return finiteNumberResult(*leftNumber / *rightNumber);
    }
    case IROp::Equal:
    case IROp::NotEqual: {
        const bool equal = valuesEqual(left, right);
        return foldedConstant(Value::boolean(op == IROp::Equal ? equal : !equal));
    }
    case IROp::Greater:
    case IROp::GreaterEqual:
    case IROp::Less:
    case IROp::LessEqual: {
        SSAConstantEvaluation failure;
        const std::optional<double> leftNumber = finiteNumber(left, failure);
        if (!leftNumber) {
            return failure;
        }
        const std::optional<double> rightNumber = finiteNumber(right, failure);
        if (!rightNumber) {
            return failure;
        }
        bool result = false;
        if (op == IROp::Greater) {
            result = *leftNumber > *rightNumber;
        } else if (op == IROp::GreaterEqual) {
            result = *leftNumber >= *rightNumber;
        } else if (op == IROp::Less) {
            result = *leftNumber < *rightNumber;
        } else {
            result = *leftNumber <= *rightNumber;
        }
        return foldedConstant(Value::boolean(result));
    }
    default:
        return constantStatus(SSAConstantEvaluationKind::Unsupported);
    }
}

void SSAOptimizationResult::verify(const ControlFlowGraph& cfg) const
{
    function.verify(cfg);
    std::set<SSAValueId> constantResults;
    for (const SSABlock& block : function.blocks) {
        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.op == IROp::Constant && instruction.result) {
                constantResults.insert(*instruction.result);
            }
        }
    }
    for (const auto& [value, constant] : foldedConstants) {
        if (constantResults.find(value) == constantResults.end()) {
            throw SSAError(
                "SSA optimizer folded-constant metadata has no surviving Constant result");
        }
        if (!isSerializablePrimitive(constant)) {
            throw SSAError(
                "SSA optimizer folded-constant metadata is not serializable");
        }
    }
}

std::string ssaOptimizationPipelineFingerprint(SSAOptimizationLevel level)
{
    switch (level) {
    case SSAOptimizationLevel::O0:
        return "m7-ssa-o0-v1";
    case SSAOptimizationLevel::O1:
        return "m7-ssa-o1-copy-phi-const-branch-dce-reach-thread-merge-v7";
    }
    throw SSAError("unknown SSA optimization level");
}

SSAOptimizationResult optimizeSSA(
    const ControlFlowGraph& cfg,
    const SSAFunction& input,
    SSAOptimizationLevel level,
    const std::vector<Value>* constantPool)
{
    cfg.verify();
    input.verify(cfg);

    SSAOptimizationResult result;
    result.function = input;
    if (level == SSAOptimizationLevel::O0) {
        result.verify(cfg);
        return result;
    }
    if (level != SSAOptimizationLevel::O1) {
        throw SSAError("unknown SSA optimization level");
    }

    simplifyCopiesAndPhis(cfg, result.function, result.stats);
    ++result.stats.passesRun;
    result.verify(cfg);

    foldKnownConstantExpressions(
        result.function,
        constantPool,
        result.stats,
        result.foldedConstants);
    ++result.stats.passesRun;
    result.verify(cfg);

    eliminateDeadPureInstructions(result.function, result.stats);
    ++result.stats.passesRun;
    retainFoldedConstantsForOutput(result.function, result.foldedConstants);
    result.verify(cfg);
    return result;
}

namespace {

SSADeSSAIRResult optimizeIRFunctionWithStats(
    const IRFunction& input,
    const std::vector<IRModuleDependency>& moduleDependencies,
    SSAOptimizationLevel level,
    SSAOptimizationStats* stats,
    const std::vector<Value>* constantPool)
{
    const auto identityResult = [&input, &moduleDependencies, stats]() {
        SSADeSSAIRResult identity;
        identity.function = input;
        identity.moduleDependencies = moduleDependencies;
        identity.syntheticInstructions.assign(input.instructions.size(), false);
        identity.originalInstructionOffsets.resize(input.instructions.size());
        for (std::size_t index = 0; index < input.instructions.size(); ++index) {
            identity.originalInstructionOffsets[index] = index;
        }
        identity.originalInsertionOffsets.resize(input.instructions.size() + 1);
        for (std::size_t index = 0; index < identity.originalInsertionOffsets.size(); ++index) {
            identity.originalInsertionOffsets[index] = index;
        }
        if (stats) {
            *stats = SSAOptimizationStats{};
        }
        identity.verify();
        return identity;
    };

    try {
        const ControlFlowGraph cfg = buildControlFlowGraph(
            input.instructions,
            moduleDependencies);
        const SSAFunction lifted = liftIRToSSA(cfg, input);
        const SSAOptimizationResult optimized = optimizeSSA(
            cfg,
            lifted,
            level,
            constantPool);
        SSAOptimizationStats resultStats = optimized.stats;
        const SSADeSSALinearFunction linear = lowerSSADeSSACopies(cfg, optimized.function);
        SSADeSSAIRResult result = lowerSSADeSSAToIR(
            cfg,
            linear,
            input.name,
            {},
            input.bindings);
        // The conservative lift keeps function parameters as runtime-cell
        // bindings, so they are not SSAParameter definitions yet. Restore the
        // ordinary IR function signature after the SSA-owned lowering contract
        // has validated its empty SSA parameter list.
        result.function.parameters = input.parameters;
        result.function.registerCount = std::max(
            input.registerCount,
            result.function.registerCount);
        result.foldedConstants = optimized.foldedConstants;
        if (level == SSAOptimizationLevel::O1) {
            normalizeKnownConditionBranches(
                result.function,
                constantPool,
                result.foldedConstants,
                resultStats);
            pruneUnreachableIRInstructions(result, resultStats);
            threadEmptyJumpBlocks(result, resultStats);
            pruneUnreachableIRInstructions(result, resultStats);
            removeRedundantFallthroughJumps(result, resultStats);
            mergeLinearBlocks(result, resultStats);
        }
        if (stats) {
            *stats = resultStats;
        }
        result.verify();
        return result;
    } catch (const SSAError& error) {
        const std::string message = error.what();
        if (message.find("ordinary IR register ") == 0
            && message.find(" has no value on predecessor ") != std::string::npos) {
            // Exhaustive source constructs may leave a statically impossible
            // linear-IR path without a runtime register value.  The current
            // IR has no undef value or unreachable-edge marker, so retaining
            // the validated stream is the only semantics-preserving result.
            return identityResult();
        }
        throw;
    }
}

bool sameBinding(const IRBinding& left, const IRBinding& right)
{
    return left.bindingId == right.bindingId
        && left.resolvedName == right.resolvedName
        && left.storage == right.storage;
}

bool sameBindings(
    const std::vector<IRBinding>& left,
    const std::vector<IRBinding>& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!sameBinding(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

bool sameFunctionIdentity(
    const IRFunction& expected,
    const IRFunction& actual)
{
    return expected.name == actual.name
        && expected.parameters == actual.parameters
        && sameBindings(expected.bindings, actual.bindings);
}

std::vector<std::size_t> makeFunctionReferences(
    const std::vector<IRInstruction>& instructions)
{
    std::vector<std::size_t> references;
    for (const IRInstruction& instruction : instructions) {
        if (instruction.op == IROp::MakeFunction) {
            references.push_back(instruction.operand);
        }
    }
    return references;
}

bool sameDependencyAnchor(
    const IRModuleDependency& expected,
    const IRModuleDependency& actual)
{
    return expected.importedModuleId == actual.importedModuleId
        && expected.kind == actual.kind
        && expected.requestedPath == actual.requestedPath;
}

std::size_t internFoldedConstant(IRProgram& program, const Value& value)
{
    for (std::size_t index = 0; index < program.constants().size(); ++index) {
        if (valuesEqual(program.constants()[index], value)) {
            return index;
        }
    }
    return program.addConstant(value);
}

void normalizeKnownConditionBranches(
    IRFunction& function,
    const std::vector<Value>* constantPool,
    const std::map<SSAValueId, Value>& foldedConstants,
    SSAOptimizationStats& stats)
{
    std::map<SSAValueId, Value> known;
    for (std::size_t index = 0; index < function.instructions.size(); ++index) {
        IRInstruction& instruction = function.instructions[index];
        if ((instruction.op == IROp::JumpIfFalse || instruction.op == IROp::JumpIfTrue)
            && instruction.left) {
            const auto condition = known.find(instruction.left->index);
            if (condition != known.end()) {
                const bool truthy = isTruthy(condition->second);
                const bool jumpsToTarget = instruction.op == IROp::JumpIfTrue
                    ? truthy
                    : !truthy;
                const std::size_t target = instruction.operand;
                instruction.op = IROp::Jump;
                instruction.operand = jumpsToTarget ? target : index + 1;
                instruction.left.reset();
                ++stats.branchesSimplified;
            }
            continue;
        }

        if (!instruction.dest) {
            continue;
        }
        const SSAValueId result = instruction.dest->index;
        if (instruction.op == IROp::Constant) {
            const auto folded = foldedConstants.find(result);
            if (folded != foldedConstants.end()) {
                known.insert_or_assign(result, folded->second);
            } else if (constantPool && instruction.operand < constantPool->size()) {
                const Value& value = (*constantPool)[instruction.operand];
                if (isSerializablePrimitive(value)) {
                    known.insert_or_assign(result, value);
                }
            }
        } else if (instruction.op == IROp::Copy && instruction.left) {
            const auto source = known.find(instruction.left->index);
            if (source != known.end()) {
                known.insert_or_assign(result, source->second);
            }
        }
    }
}

void compactIRInstructions(
    SSADeSSAIRResult& result,
    const std::vector<bool>& retained,
    const char* passName)
{
    const std::size_t instructionCount = result.function.instructions.size();
    if (retained.size() != instructionCount
        || result.syntheticInstructions.size() != instructionCount) {
        throw SSAError(std::string(passName) + " received inconsistent instruction metadata");
    }

    std::vector<std::size_t> boundary(instructionCount + 1, 0);
    for (std::size_t instruction = 0; instruction < instructionCount; ++instruction) {
        boundary[instruction + 1] = boundary[instruction]
            + (retained[instruction] ? 1 : 0);
    }

    const auto remapBoundary = [&boundary, instructionCount, passName](std::size_t offset) {
        if (offset > instructionCount) {
            throw SSAError(std::string(passName) + " encountered an invalid offset");
        }
        return boundary[offset];
    };

    std::vector<IRInstruction> instructions;
    instructions.reserve(boundary.back());
    std::vector<bool> synthetic;
    synthetic.reserve(boundary.back());
    for (std::size_t oldOffset = 0; oldOffset < instructionCount; ++oldOffset) {
        if (!retained[oldOffset]) {
            continue;
        }
        IRInstruction instruction = result.function.instructions[oldOffset];
        if (instruction.op == IROp::Jump
            || instruction.op == IROp::JumpIfFalse
            || instruction.op == IROp::JumpIfTrue) {
            instruction.operand = remapBoundary(instruction.operand);
        }
        instructions.push_back(std::move(instruction));
        synthetic.push_back(result.syntheticInstructions[oldOffset]);
    }

    for (std::optional<std::size_t>& offset : result.originalInstructionOffsets) {
        if (!offset) {
            continue;
        }
        if (*offset >= instructionCount || !retained[*offset]) {
            offset.reset();
        } else {
            *offset = boundary[*offset];
        }
    }
    for (std::size_t& offset : result.originalInsertionOffsets) {
        offset = remapBoundary(offset);
    }
    for (IRModuleDependency& dependency : result.moduleDependencies) {
        dependency.instructionOffset = remapBoundary(dependency.instructionOffset);
    }

    result.function.instructions = std::move(instructions);
    result.syntheticInstructions = std::move(synthetic);
    for (auto it = result.foldedConstants.begin(); it != result.foldedConstants.end();) {
        const bool survives = std::any_of(
            result.function.instructions.begin(),
            result.function.instructions.end(),
            [value = it->first](const IRInstruction& instruction) {
                return instruction.op == IROp::Constant
                    && instruction.dest
                    && instruction.dest->index == value;
            });
        if (!survives) {
            it = result.foldedConstants.erase(it);
        } else {
            ++it;
        }
    }
    result.verify();
}

void pruneUnreachableIRInstructions(
    SSADeSSAIRResult& result,
    SSAOptimizationStats& stats)
{
    const ControlFlowGraph cfg = buildControlFlowGraph(
        result.function.instructions,
        result.moduleDependencies);
    std::size_t unreachableBlocks = 0;
    std::vector<bool> retained(result.function.instructions.size(), false);
    for (const CFGBlock& block : cfg.blocks) {
        if (!block.syntheticExit && !block.reachable) {
            ++unreachableBlocks;
        }
        if (!block.syntheticExit && block.reachable) {
            for (std::size_t instruction = block.firstInstruction;
                 instruction < block.endInstruction;
                 ++instruction) {
                retained[instruction] = true;
            }
        }
    }
    if (unreachableBlocks == 0) {
        return;
    }

    compactIRInstructions(result, retained, "unreachable-block pruning");
    stats.blocksRemoved += unreachableBlocks;
}

void removeRedundantFallthroughJumps(
    SSADeSSAIRResult& result,
    SSAOptimizationStats& stats)
{
    const std::size_t instructionCount = result.function.instructions.size();
    std::vector<bool> retained(instructionCount, true);
    std::size_t removed = 0;
    for (std::size_t instruction = 0; instruction < instructionCount; ++instruction) {
        const IRInstruction& current = result.function.instructions[instruction];
        if (current.op == IROp::Jump && current.operand == instruction + 1) {
            retained[instruction] = false;
            ++removed;
        }
    }
    if (removed == 0) {
        return;
    }

    compactIRInstructions(result, retained, "fallthrough-jump removal");
    stats.jumpsRemoved += removed;
}

void threadEmptyJumpBlocks(
    SSADeSSAIRResult& result,
    SSAOptimizationStats& stats)
{
    const ControlFlowGraph cfg = buildControlFlowGraph(
        result.function.instructions,
        result.moduleDependencies);
    bool changed = false;
    for (std::size_t instruction = 0;
         instruction < result.function.instructions.size();
         ++instruction) {
        IRInstruction& current = result.function.instructions[instruction];
        if (current.op != IROp::Jump || current.operand >= result.function.instructions.size()) {
            continue;
        }

        std::set<std::size_t> visited;
        std::size_t target = current.operand;
        while (target < result.function.instructions.size()
            && visited.insert(target).second) {
            const auto targetBlockId = cfg.blockForInstruction(target);
            if (!targetBlockId) {
                break;
            }
            const CFGBlock& targetBlock = cfg.blocks[*targetBlockId];
            if (targetBlock.firstInstruction != target
                || targetBlock.endInstruction != target + 1
                || result.function.instructions[target].op != IROp::Jump) {
                break;
            }
            target = result.function.instructions[target].operand;
        }

        if (target != current.operand) {
            current.operand = target;
            ++stats.jumpsThreaded;
            changed = true;
        }
    }
    if (changed) {
        result.verify();
    }
}

bool isExplicitControlFlowTerminator(IROp op)
{
    return op == IROp::Jump || op == IROp::JumpIfFalse || op == IROp::JumpIfTrue
        || op == IROp::Return;
}

std::vector<CFGBlockId> blockOrderForMerge(
    const ControlFlowGraph& cfg,
    CFGBlockId predecessor,
    CFGBlockId successor)
{
    std::vector<CFGBlockId> order;
    order.reserve(cfg.exitBlock);
    for (CFGBlockId block = 0; block < cfg.exitBlock; ++block) {
        order.push_back(block);
    }

    const auto successorPosition = std::find(order.begin(), order.end(), successor);
    if (successorPosition == order.end()) {
        throw SSAError("block merge successor is not in the linear order");
    }
    order.erase(successorPosition);

    const auto predecessorPosition = std::find(order.begin(), order.end(), predecessor);
    if (predecessorPosition == order.end()) {
        throw SSAError("block merge predecessor is not in the linear order");
    }
    order.insert(predecessorPosition + 1, successor);
    return order;
}

bool preservesFallthroughEdges(
    const ControlFlowGraph& cfg,
    const IRFunction& function,
    const std::vector<CFGBlockId>& order,
    CFGBlockId mergedPredecessor,
    CFGBlockId mergedSuccessor)
{
    std::vector<std::size_t> positions(cfg.exitBlock, std::numeric_limits<std::size_t>::max());
    for (std::size_t position = 0; position < order.size(); ++position) {
        if (order[position] >= cfg.exitBlock
            || positions[order[position]] != std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        positions[order[position]] = position;
    }

    const auto nextBlock = [&order, &cfg](std::size_t position) {
        return position + 1 < order.size() ? order[position + 1] : cfg.exitBlock;
    };
    if (order.size() != cfg.exitBlock) {
        return false;
    }
    for (CFGBlockId block = 0; block < cfg.exitBlock; ++block) {
        if (positions[block] == std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        const CFGBlock& cfgBlock = cfg.blocks[block];
        if (block == mergedPredecessor) {
            if (nextBlock(positions[block]) != mergedSuccessor) {
                return false;
            }
            continue;
        }
        if (cfgBlock.firstInstruction >= cfgBlock.endInstruction
            || isExplicitControlFlowTerminator(
                function.instructions[cfgBlock.endInstruction - 1].op)) {
            continue;
        }
        if (cfgBlock.successors.size() != 1
            || nextBlock(positions[block]) != cfgBlock.successors.front()) {
            return false;
        }
    }
    return true;
}

void reorderAndRemoveIRInstruction(
    SSADeSSAIRResult& result,
    const ControlFlowGraph& cfg,
    const std::vector<CFGBlockId>& order,
    std::size_t removedInstruction,
    const char* passName)
{
    const std::size_t instructionCount = result.function.instructions.size();
    if (cfg.instructionCount != instructionCount
        || removedInstruction >= instructionCount
        || order.size() != cfg.exitBlock) {
        throw SSAError(std::string(passName) + " received inconsistent block metadata");
    }

    const std::size_t invalidOffset = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> newOffsets(instructionCount, invalidOffset);
    std::vector<bool> visitedBlocks(cfg.exitBlock, false);
    std::vector<IRInstruction> instructions;
    std::vector<bool> synthetic;
    instructions.reserve(instructionCount - 1);
    synthetic.reserve(instructionCount - 1);
    for (const CFGBlockId block : order) {
        if (block >= cfg.exitBlock || visitedBlocks[block]) {
            throw SSAError(std::string(passName) + " block order is malformed");
        }
        visitedBlocks[block] = true;
        const CFGBlock& cfgBlock = cfg.blocks[block];
        for (std::size_t oldOffset = cfgBlock.firstInstruction;
             oldOffset < cfgBlock.endInstruction;
             ++oldOffset) {
            if (oldOffset == removedInstruction) {
                continue;
            }
            newOffsets[oldOffset] = instructions.size();
            instructions.push_back(result.function.instructions[oldOffset]);
            synthetic.push_back(result.syntheticInstructions[oldOffset]);
        }
    }
    if (std::any_of(visitedBlocks.begin(), visitedBlocks.end(), [](bool visited) {
            return !visited;
        })) {
        throw SSAError(std::string(passName) + " block order does not cover the CFG");
    }

    std::vector<std::size_t> nextSurviving(instructionCount + 1, instructions.size());
    for (std::size_t oldOffset = instructionCount; oldOffset > 0; --oldOffset) {
        const std::size_t index = oldOffset - 1;
        nextSurviving[index] = newOffsets[index] == invalidOffset
            ? nextSurviving[index + 1]
            : newOffsets[index];
    }
    const auto remapOffset = [
        &nextSurviving,
        instructionCount,
        passName](std::size_t offset) {
        if (offset > instructionCount) {
            throw SSAError(std::string(passName) + " encountered an invalid offset");
        }
        return nextSurviving[offset];
    };

    for (IRInstruction& instruction : instructions) {
        if (instruction.op == IROp::Jump
            || instruction.op == IROp::JumpIfFalse
            || instruction.op == IROp::JumpIfTrue) {
            instruction.operand = remapOffset(instruction.operand);
        }
    }
    for (std::optional<std::size_t>& offset : result.originalInstructionOffsets) {
        if (!offset || *offset >= instructionCount || newOffsets[*offset] == invalidOffset) {
            offset.reset();
        } else {
            *offset = newOffsets[*offset];
        }
    }
    for (std::size_t& offset : result.originalInsertionOffsets) {
        offset = remapOffset(offset);
    }
    for (IRModuleDependency& dependency : result.moduleDependencies) {
        dependency.instructionOffset = remapOffset(dependency.instructionOffset);
    }

    result.function.instructions = std::move(instructions);
    result.syntheticInstructions = std::move(synthetic);
    result.verify();
}

bool dependencyOffsetsRemainOrdered(const SSADeSSAIRResult& result)
{
    return std::is_sorted(
        result.moduleDependencies.begin(),
        result.moduleDependencies.end(),
        [](const IRModuleDependency& left, const IRModuleDependency& right) {
            return left.instructionOffset < right.instructionOffset;
        });
}

void mergeLinearBlocks(
    SSADeSSAIRResult& result,
    SSAOptimizationStats& stats)
{
    while (true) {
        const ControlFlowGraph cfg = buildControlFlowGraph(
            result.function.instructions,
            result.moduleDependencies);
        bool merged = false;
        for (CFGBlockId predecessor = 0; predecessor < cfg.exitBlock; ++predecessor) {
            const CFGBlock& predecessorBlock = cfg.blocks[predecessor];
            if (!predecessorBlock.reachable || predecessorBlock.successors.size() != 1
                || predecessorBlock.firstInstruction >= predecessorBlock.endInstruction) {
                continue;
            }
            const CFGBlockId successor = predecessorBlock.successors.front();
            if (successor == cfg.exitBlock || !cfg.blocks[successor].reachable
                || cfg.blocks[successor].predecessors.size() != 1
                || cfg.blocks[successor].predecessors.front() != predecessor) {
                continue;
            }
            const std::size_t jump = predecessorBlock.endInstruction - 1;
            const IRInstruction& terminator = result.function.instructions[jump];
            if (terminator.op != IROp::Jump
                || terminator.operand != cfg.blocks[successor].firstInstruction) {
                continue;
            }

            const std::vector<CFGBlockId> order = blockOrderForMerge(
                cfg,
                predecessor,
                successor);
            if (!preservesFallthroughEdges(
                    cfg,
                    result.function,
                    order,
                    predecessor,
                    successor)) {
                continue;
            }

            SSADeSSAIRResult candidate = result;
            reorderAndRemoveIRInstruction(
                candidate,
                cfg,
                order,
                jump,
                "linear block merge");
            if (makeFunctionReferences(result.function.instructions)
                    != makeFunctionReferences(candidate.function.instructions)
                || !dependencyOffsetsRemainOrdered(candidate)) {
                continue;
            }
            buildControlFlowGraph(
                candidate.function.instructions,
                candidate.moduleDependencies);
            result = std::move(candidate);
            ++stats.blocksMerged;
            ++stats.jumpsRemoved;
            merged = true;
            break;
        }
        if (!merged) {
            return;
        }
    }
}

void materializeFoldedConstants(
    IRProgram& program,
    IRFunction& function,
    const std::map<SSAValueId, Value>& foldedConstants)
{
    for (IRInstruction& instruction : function.instructions) {
        if (instruction.op != IROp::Constant || !instruction.dest) {
            continue;
        }
        const auto folded = foldedConstants.find(instruction.dest->index);
        if (folded != foldedConstants.end()) {
            instruction.operand = internFoldedConstant(program, folded->second);
        }
    }
}

} // namespace

SSADeSSAIRResult optimizeIRFunction(
    const IRFunction& input,
    const std::vector<IRModuleDependency>& moduleDependencies,
    SSAOptimizationLevel level)
{
    return optimizeIRFunctionWithStats(input, moduleDependencies, level, nullptr, nullptr);
}

void SSADeSSAProgramResult::verify(const IRProgram& input) const
{
    if (functions.size() != input.functions().size()) {
        throw SSAError("program optimizer changed the function-table size");
    }
    if (functionStats.size() != functions.size()) {
        throw SSAError("program optimizer function statistics have the wrong size");
    }

    mainStream.verify();
    if (!mainStream.function.name.empty() || !mainStream.function.parameters.empty()) {
        throw SSAError("program optimizer changed the anonymous main signature");
    }
    if (!sameBindings(mainStream.function.bindings, input.bindings())) {
        throw SSAError("program optimizer changed canonical binding metadata");
    }
    if (mainStream.moduleDependencies.size() != input.moduleDependencies().size()) {
        throw SSAError("program optimizer changed main dependency metadata");
    }
    for (std::size_t index = 0; index < mainStream.moduleDependencies.size(); ++index) {
        if (!sameDependencyAnchor(
                input.moduleDependencies()[index],
                mainStream.moduleDependencies[index])) {
            throw SSAError("program optimizer changed main dependency ordering");
        }
    }
    if (makeFunctionReferences(input.instructions())
        != makeFunctionReferences(mainStream.function.instructions)) {
        throw SSAError("program optimizer changed main function references");
    }

    for (std::size_t index = 0; index < functions.size(); ++index) {
        const IRFunction& expected = input.functions()[index];
        const SSADeSSAIRResult& actual = functions[index];
        actual.verify();
        if (!sameFunctionIdentity(expected, actual.function)) {
            throw SSAError("program optimizer changed function-table identity or metadata");
        }
        if (!actual.moduleDependencies.empty()) {
            throw SSAError("nested IR function unexpectedly carries module dependencies");
        }
        if (makeFunctionReferences(expected.instructions)
            != makeFunctionReferences(actual.function.instructions)) {
            throw SSAError("program optimizer changed nested function references");
        }
    }

    // Validate the replacement streams against a copy of the source pools.
    // Folded SSA values may need one new primitive constant; materializing
    // them here keeps the immutable input program and its ordinary IR result
    // separately verifiable.
    IRProgram candidate = input;
    IRFunction main = mainStream.function;
    materializeFoldedConstants(candidate, main, mainStream.foldedConstants);
    std::vector<IRFunction> candidateFunctions;
    candidateFunctions.reserve(functions.size());
    for (const SSADeSSAIRResult& function : functions) {
        IRFunction lowered = function.function;
        materializeFoldedConstants(candidate, lowered, function.foldedConstants);
        candidateFunctions.push_back(std::move(lowered));
    }
    candidate.rebuildWithStreams(
        std::move(main.instructions),
        main.registerCount,
        std::move(candidateFunctions),
        mainStream.moduleDependencies);
}

IRProgram SSADeSSAProgramResult::rebuild(const IRProgram& input) const
{
    verify(input);
    IRProgram rebuilt = input;
    IRFunction main = mainStream.function;
    materializeFoldedConstants(rebuilt, main, mainStream.foldedConstants);
    std::vector<IRFunction> rebuiltFunctions;
    rebuiltFunctions.reserve(functions.size());
    for (const SSADeSSAIRResult& function : functions) {
        IRFunction lowered = function.function;
        materializeFoldedConstants(rebuilt, lowered, function.foldedConstants);
        rebuiltFunctions.push_back(std::move(lowered));
    }
    return rebuilt.rebuildWithStreams(
        std::move(main.instructions),
        main.registerCount,
        std::move(rebuiltFunctions),
        mainStream.moduleDependencies);
}

SSADeSSAProgramResult optimizeIRProgram(
    const IRProgram& input,
    SSAOptimizationLevel level)
{
    // Validate the source program once at the program boundary. This also
    // makes malformed MakeFunction/pool/dependency references fail before any
    // individual stream is transformed.
    input.rebuildWithStreams(
        input.instructions(),
        input.registerCount(),
        input.functions(),
        input.moduleDependencies());

    IRFunction main;
    main.instructions = input.instructions();
    main.registerCount = input.registerCount();
    main.bindings = input.bindings();

    SSADeSSAProgramResult result;
    result.mainStream = optimizeIRFunctionWithStats(
        main,
        input.moduleDependencies(),
        level,
        &result.mainStats,
        &input.constants());
    result.functions.reserve(input.functions().size());
    result.functionStats.reserve(input.functions().size());
    for (const IRFunction& function : input.functions()) {
        SSAOptimizationStats stats;
        result.functions.push_back(optimizeIRFunctionWithStats(
            function,
            {},
            level,
            &stats,
            &input.constants()));
        result.functionStats.push_back(stats);
    }
    result.verify(input);
    return result;
}
