#include "Optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

using ValueAliases = std::unordered_map<SSAValueId, SSAValueId>;
using ValueDefinitions = std::unordered_map<SSAValueId, CFGBlockId>;

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
}

std::string ssaOptimizationPipelineFingerprint(SSAOptimizationLevel level)
{
    switch (level) {
    case SSAOptimizationLevel::O0:
        return "m7-ssa-o0-v1";
    case SSAOptimizationLevel::O1:
        return "m7-ssa-o1-copy-phi-dce-v1";
    }
    throw SSAError("unknown SSA optimization level");
}

SSAOptimizationResult optimizeSSA(
    const ControlFlowGraph& cfg,
    const SSAFunction& input,
    SSAOptimizationLevel level)
{
    cfg.verify();
    input.verify(cfg);

    SSAOptimizationResult result{input, {}};
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

    eliminateDeadPureInstructions(result.function, result.stats);
    ++result.stats.passesRun;
    result.verify(cfg);
    return result;
}

namespace {

SSADeSSAIRResult optimizeIRFunctionWithStats(
    const IRFunction& input,
    const std::vector<IRModuleDependency>& moduleDependencies,
    SSAOptimizationLevel level,
    SSAOptimizationStats* stats)
{
    const ControlFlowGraph cfg = buildControlFlowGraph(
        input.instructions,
        moduleDependencies);
    const SSAFunction lifted = liftIRToSSA(cfg, input);
    const SSAOptimizationResult optimized = optimizeSSA(cfg, lifted, level);
    if (stats) {
        *stats = optimized.stats;
    }
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
    result.verify();
    return result;
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

} // namespace

SSADeSSAIRResult optimizeIRFunction(
    const IRFunction& input,
    const std::vector<IRModuleDependency>& moduleDependencies,
    SSAOptimizationLevel level)
{
    return optimizeIRFunctionWithStats(input, moduleDependencies, level, nullptr);
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

    // This validates the replacement streams against the original immutable
    // pools and also rejects stale register, name, constant, function, jump,
    // binding, or dependency references before any caller can rebuild a
    // program for a later compiler boundary.
    input.rebuildWithStreams(
        mainStream.function.instructions,
        mainStream.function.registerCount,
        [&]() {
            std::vector<IRFunction> result;
            result.reserve(functions.size());
            for (const SSADeSSAIRResult& function : functions) {
                result.push_back(function.function);
            }
            return result;
        }(),
        mainStream.moduleDependencies);
}

IRProgram SSADeSSAProgramResult::rebuild(const IRProgram& input) const
{
    verify(input);
    std::vector<IRFunction> rebuiltFunctions;
    rebuiltFunctions.reserve(functions.size());
    for (const SSADeSSAIRResult& function : functions) {
        rebuiltFunctions.push_back(function.function);
    }
    return input.rebuildWithStreams(
        mainStream.function.instructions,
        mainStream.function.registerCount,
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
        &result.mainStats);
    result.functions.reserve(input.functions().size());
    result.functionStats.reserve(input.functions().size());
    for (const IRFunction& function : input.functions()) {
        SSAOptimizationStats stats;
        result.functions.push_back(optimizeIRFunctionWithStats(
            function,
            {},
            level,
            &stats));
        result.functionStats.push_back(stats);
    }
    result.verify(input);
    return result;
}
