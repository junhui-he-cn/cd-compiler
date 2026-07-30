#pragma once

#include "IR.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using CFGBlockId = std::size_t;

struct CFGBlock {
    CFGBlockId id = 0;
    std::size_t firstInstruction = 0;
    std::size_t endInstruction = 0;
    bool syntheticExit = false;
    bool reachable = false;
    std::vector<CFGBlockId> predecessors;
    std::vector<CFGBlockId> successors;
};

struct CFGDependencyAnchor {
    IRModuleDependency dependency;
    CFGBlockId block = 0;
    std::size_t offsetInBlock = 0;
};

class ControlFlowGraphError final : public std::runtime_error {
public:
    explicit ControlFlowGraphError(std::string message);
};

struct ControlFlowGraph {
    std::size_t instructionCount = 0;
    CFGBlockId entryBlock = 0;
    CFGBlockId exitBlock = 0;
    std::vector<CFGBlock> blocks;
    std::vector<CFGBlockId> instructionBlocks;
    std::vector<CFGDependencyAnchor> dependencyAnchors;

    std::optional<CFGBlockId> blockForInstruction(std::size_t instruction) const;

    // Validate block ranges, edge symmetry, reachability, and dependency
    // anchors after construction or a later CFG transformation.
    void verify() const;
};

ControlFlowGraph buildControlFlowGraph(
    const std::vector<IRInstruction>& instructions,
    const std::vector<IRModuleDependency>& dependencies = {});
ControlFlowGraph buildControlFlowGraph(const IRProgram& program);
ControlFlowGraph buildControlFlowGraph(const IRFunction& function);
