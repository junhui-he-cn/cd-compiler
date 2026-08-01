#pragma once

#include "ControlFlowGraph.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

class DominanceError final : public std::runtime_error {
public:
    explicit DominanceError(std::string message);
};

struct DominanceInfo {
    // Each reachable block's set is sorted by block ID. Unreachable blocks
    // have no dominance set because dominance is defined from the entry.
    std::vector<std::vector<CFGBlockId>> dominators;
    std::vector<std::optional<CFGBlockId>> immediateDominators;
    std::vector<std::vector<CFGBlockId>> dominanceChildren;
    std::vector<std::vector<CFGBlockId>> dominanceFrontiers;

    bool dominates(CFGBlockId dominator, CFGBlockId block) const;

    // Validate the deterministic dominance, immediate-dominator tree, and
    // frontier contract against the source CFG.
    void verify(const ControlFlowGraph& cfg) const;
};

DominanceInfo buildDominanceInfo(const ControlFlowGraph& cfg);
