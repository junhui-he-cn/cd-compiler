#include "Optimizer.hpp"

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
