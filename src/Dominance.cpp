#include "Dominance.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace {

bool contains(const std::vector<CFGBlockId>& values, CFGBlockId value)
{
    return std::binary_search(values.begin(), values.end(), value);
}

void addUnique(std::vector<CFGBlockId>& values, CFGBlockId value)
{
    if (!contains(values, value)) {
        values.push_back(value);
        std::sort(values.begin(), values.end());
    }
}

std::vector<CFGBlockId> intersect(
    const std::vector<CFGBlockId>& left,
    const std::vector<CFGBlockId>& right)
{
    std::vector<CFGBlockId> result;
    std::set_intersection(
        left.begin(),
        left.end(),
        right.begin(),
        right.end(),
        std::back_inserter(result));
    return result;
}

bool equalOptional(CFGBlockId value, const std::optional<CFGBlockId>& other)
{
    return other.has_value() && value == *other;
}

void validateSortedUnique(
    const std::vector<CFGBlockId>& values,
    CFGBlockId blockCount,
    const std::string& label)
{
    if (!std::is_sorted(values.begin(), values.end())
        || std::adjacent_find(values.begin(), values.end()) != values.end()) {
        throw DominanceError(label + " is not sorted and unique");
    }
    for (const CFGBlockId value : values) {
        if (value >= blockCount) {
            throw DominanceError(label + " references an invalid block");
        }
    }
}

void addFrontierAlongIdomChain(
    std::vector<std::vector<CFGBlockId>>& frontiers,
    const std::vector<std::optional<CFGBlockId>>& immediateDominators,
    CFGBlockId frontierBlock,
    CFGBlockId predecessor,
    std::size_t blockCount)
{
    const std::optional<CFGBlockId>& stop = immediateDominators[frontierBlock];
    CFGBlockId runner = predecessor;
    for (std::size_t steps = 0; steps <= blockCount; ++steps) {
        if (equalOptional(runner, stop)) {
            return;
        }
        addUnique(frontiers[runner], frontierBlock);
        if (!immediateDominators[runner]) {
            if (stop) {
                throw DominanceError("immediate-dominator chain does not reach its stop");
            }
            return;
        }
        runner = *immediateDominators[runner];
    }
    throw DominanceError("immediate-dominator chain contains a cycle");
}

std::vector<std::vector<CFGBlockId>> computeExpectedFrontiers(
    const ControlFlowGraph& cfg,
    const std::vector<std::optional<CFGBlockId>>& immediateDominators)
{
    std::vector<std::vector<CFGBlockId>> result(cfg.blocks.size());
    for (const CFGBlock& block : cfg.blocks) {
        if (!block.reachable) {
            continue;
        }
        for (const CFGBlockId predecessor : block.predecessors) {
            if (!cfg.blocks[predecessor].reachable) {
                continue;
            }
            addFrontierAlongIdomChain(
                result,
                immediateDominators,
                block.id,
                predecessor,
                cfg.blocks.size());
        }
    }
    return result;
}

} // namespace

DominanceError::DominanceError(std::string message)
    : std::runtime_error(std::move(message))
{
}

bool DominanceInfo::dominates(CFGBlockId dominator, CFGBlockId block) const
{
    if (block >= dominators.size() || dominator >= dominators.size()) {
        return false;
    }
    return contains(dominators[block], dominator);
}

void DominanceInfo::verify(const ControlFlowGraph& cfg) const
{
    cfg.verify();
    const std::size_t blockCount = cfg.blocks.size();
    if (dominators.size() != blockCount
        || immediateDominators.size() != blockCount
        || dominanceChildren.size() != blockCount
        || dominanceFrontiers.size() != blockCount) {
        throw DominanceError("dominance vectors do not match CFG block count");
    }

    for (const CFGBlock& block : cfg.blocks) {
        validateSortedUnique(dominators[block.id], blockCount, "dominators");
        validateSortedUnique(dominanceChildren[block.id], blockCount, "dominance children");
        validateSortedUnique(dominanceFrontiers[block.id], blockCount, "dominance frontiers");

        if (immediateDominators[block.id]
            && *immediateDominators[block.id] >= blockCount) {
            throw DominanceError("immediate dominator references an invalid block");
        }

        if (!block.reachable) {
            if (!dominators[block.id].empty() || immediateDominators[block.id]
                || !dominanceChildren[block.id].empty()
                || !dominanceFrontiers[block.id].empty()) {
                throw DominanceError("unreachable block has dominance metadata");
            }
            continue;
        }

        if (!contains(dominators[block.id], block.id)) {
            throw DominanceError("reachable block does not dominate itself");
        }
        for (const CFGBlockId dominator : dominators[block.id]) {
            if (!cfg.blocks[dominator].reachable) {
                throw DominanceError("dominance set contains an unreachable block");
            }
        }

        if (block.id == cfg.entryBlock) {
            if (immediateDominators[block.id]) {
                throw DominanceError("entry block has an immediate dominator");
            }
        } else {
            const std::optional<CFGBlockId>& idom = immediateDominators[block.id];
            if (!idom || !contains(dominators[block.id], *idom) || *idom == block.id) {
                throw DominanceError("non-entry block has an invalid immediate dominator");
            }
            for (const CFGBlockId candidate : dominators[block.id]) {
                if (candidate == block.id || candidate == *idom) {
                    continue;
                }
                if (!contains(dominators[*idom], candidate)) {
                    throw DominanceError("immediate dominator is not the closest dominator");
                }
            }
        }
    }

    std::vector<std::vector<CFGBlockId>> expectedChildren(blockCount);
    for (CFGBlockId block = 0; block < blockCount; ++block) {
        if (immediateDominators[block]) {
            addUnique(expectedChildren[*immediateDominators[block]], block);
        }
    }
    if (expectedChildren != dominanceChildren) {
        throw DominanceError("dominance children do not match immediate dominators");
    }

    const auto expectedFrontiers = computeExpectedFrontiers(cfg, immediateDominators);
    if (expectedFrontiers != dominanceFrontiers) {
        throw DominanceError("dominance frontiers do not match CFG");
    }
}

DominanceInfo buildDominanceInfo(const ControlFlowGraph& cfg)
{
    cfg.verify();
    const std::size_t blockCount = cfg.blocks.size();

    DominanceInfo result;
    result.dominators.resize(blockCount);
    result.immediateDominators.resize(blockCount);
    result.dominanceChildren.resize(blockCount);
    result.dominanceFrontiers.resize(blockCount);

    std::vector<CFGBlockId> reachable;
    for (const CFGBlock& block : cfg.blocks) {
        if (block.reachable) {
            reachable.push_back(block.id);
        }
    }

    for (const CFGBlockId block : reachable) {
        if (block == cfg.entryBlock) {
            result.dominators[block] = {cfg.entryBlock};
        } else {
            result.dominators[block] = reachable;
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const CFGBlockId block : reachable) {
            if (block == cfg.entryBlock) {
                continue;
            }
            bool hasPredecessor = false;
            std::vector<CFGBlockId> next;
            for (const CFGBlockId predecessor : cfg.blocks[block].predecessors) {
                if (!cfg.blocks[predecessor].reachable) {
                    continue;
                }
                if (!hasPredecessor) {
                    next = result.dominators[predecessor];
                    hasPredecessor = true;
                } else {
                    next = intersect(next, result.dominators[predecessor]);
                }
            }
            if (!hasPredecessor) {
                throw DominanceError("reachable non-entry block has no reachable predecessor");
            }
            next.push_back(block);
            std::sort(next.begin(), next.end());
            next.erase(std::unique(next.begin(), next.end()), next.end());
            if (next != result.dominators[block]) {
                result.dominators[block] = std::move(next);
                changed = true;
            }
        }
    }

    for (const CFGBlockId block : reachable) {
        if (block == cfg.entryBlock) {
            continue;
        }
        std::vector<CFGBlockId> strictDominators = result.dominators[block];
        strictDominators.erase(
            std::remove(strictDominators.begin(), strictDominators.end(), block),
            strictDominators.end());
        for (const CFGBlockId candidate : strictDominators) {
            bool isImmediate = true;
            for (const CFGBlockId other : strictDominators) {
                if (other == candidate) {
                    continue;
                }
                if (!contains(result.dominators[candidate], other)) {
                    isImmediate = false;
                    break;
                }
            }
            if (isImmediate) {
                if (result.immediateDominators[block]) {
                    throw DominanceError("block has multiple immediate dominators");
                }
                result.immediateDominators[block] = candidate;
            }
        }
        if (!result.immediateDominators[block]) {
            throw DominanceError("non-entry reachable block has no immediate dominator");
        }
    }

    for (const CFGBlockId block : reachable) {
        if (result.immediateDominators[block]) {
            addUnique(result.dominanceChildren[*result.immediateDominators[block]], block);
        }
    }
    result.dominanceFrontiers = computeExpectedFrontiers(cfg, result.immediateDominators);
    result.verify(cfg);
    return result;
}
