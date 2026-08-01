#include "ControlFlowGraph.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace {

bool isConditionalJump(IROp op)
{
    return op == IROp::JumpIfFalse || op == IROp::JumpIfTrue;
}

bool isTerminator(IROp op)
{
    return op == IROp::Jump || isConditionalJump(op) || op == IROp::Return;
}

std::string instructionLabel(std::size_t instruction)
{
    return " at instruction " + std::to_string(instruction);
}

void validateJumpTarget(
    const std::vector<IRInstruction>& instructions,
    std::size_t instruction)
{
    const IRInstruction& current = instructions[instruction];
    if (!isTerminator(current.op) || current.op == IROp::Return) {
        return;
    }
    if (current.operand > instructions.size()) {
        throw ControlFlowGraphError(
            "jump target out of range" + instructionLabel(instruction));
    }
}

void addUnique(std::vector<CFGBlockId>& values, CFGBlockId value)
{
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

CFGBlockId blockAtInstruction(
    const std::vector<CFGBlockId>& instructionBlocks,
    std::size_t instruction,
    CFGBlockId exitBlock)
{
    return instruction == instructionBlocks.size() ? exitBlock : instructionBlocks.at(instruction);
}

void markReachable(ControlFlowGraph& graph)
{
    std::vector<CFGBlockId> pending{graph.entryBlock};
    std::vector<bool> visited(graph.blocks.size(), false);
    while (!pending.empty()) {
        const CFGBlockId block = pending.back();
        pending.pop_back();
        if (visited[block]) {
            continue;
        }
        visited[block] = true;
        for (const CFGBlockId successor : graph.blocks[block].successors) {
            pending.push_back(successor);
        }
    }

    for (CFGBlock& block : graph.blocks) {
        block.reachable = visited[block.id];
    }
}

} // namespace

ControlFlowGraphError::ControlFlowGraphError(std::string message)
    : std::runtime_error(std::move(message))
{
}

std::optional<CFGBlockId> ControlFlowGraph::blockForInstruction(std::size_t instruction) const
{
    if (instruction >= instructionBlocks.size()) {
        return std::nullopt;
    }
    return instructionBlocks[instruction];
}

void ControlFlowGraph::verify() const
{
    if (blocks.empty()) {
        throw ControlFlowGraphError("CFG must contain an exit block");
    }
    if (entryBlock >= blocks.size() || exitBlock >= blocks.size()) {
        throw ControlFlowGraphError("CFG entry or exit block is out of range");
    }
    if (exitBlock != blocks.size() - 1) {
        throw ControlFlowGraphError("CFG exit block must be last");
    }
    if (blocks[exitBlock].id != exitBlock || !blocks[exitBlock].syntheticExit) {
        throw ControlFlowGraphError("CFG exit block is malformed");
    }
    if (blocks[exitBlock].firstInstruction != instructionCount
        || blocks[exitBlock].endInstruction != instructionCount) {
        throw ControlFlowGraphError("CFG exit block range is malformed");
    }
    if (!blocks[exitBlock].successors.empty()) {
        throw ControlFlowGraphError("CFG exit block cannot have successors");
    }

    std::size_t nextInstruction = 0;
    for (CFGBlockId id = 0; id < exitBlock; ++id) {
        const CFGBlock& block = blocks[id];
        if (block.id != id) {
            throw ControlFlowGraphError("CFG block IDs are not deterministic");
        }
        if (block.syntheticExit || block.firstInstruction != nextInstruction
            || block.firstInstruction >= block.endInstruction
            || block.endInstruction > instructionCount) {
            throw ControlFlowGraphError("CFG block ranges are not contiguous");
        }
        nextInstruction = block.endInstruction;
    }
    if (nextInstruction != instructionCount) {
        throw ControlFlowGraphError("CFG blocks do not cover all instructions");
    }
    if (instructionBlocks.size() != instructionCount) {
        throw ControlFlowGraphError("CFG instruction map has the wrong size");
    }
    for (std::size_t instruction = 0; instruction < instructionCount; ++instruction) {
        const CFGBlockId blockId = instructionBlocks[instruction];
        if (blockId >= exitBlock
            || instruction < blocks[blockId].firstInstruction
            || instruction >= blocks[blockId].endInstruction) {
            throw ControlFlowGraphError("CFG instruction map points outside its block");
        }
    }

    for (const CFGBlock& block : blocks) {
        if (block.id >= blocks.size()) {
            throw ControlFlowGraphError("CFG edge references an invalid block");
        }
        std::unordered_set<CFGBlockId> successors;
        for (const CFGBlockId successor : block.successors) {
            if (successor >= blocks.size() || !successors.insert(successor).second) {
                throw ControlFlowGraphError("CFG successor list is malformed");
            }
            const auto& predecessors = blocks[successor].predecessors;
            if (std::find(predecessors.begin(), predecessors.end(), block.id)
                == predecessors.end()) {
                throw ControlFlowGraphError("CFG predecessor list is not symmetric");
            }
        }

        std::unordered_set<CFGBlockId> predecessors;
        for (const CFGBlockId predecessor : block.predecessors) {
            if (predecessor >= blocks.size() || !predecessors.insert(predecessor).second) {
                throw ControlFlowGraphError("CFG predecessor list is malformed");
            }
            const auto& successorsOfPredecessor = blocks[predecessor].successors;
            if (std::find(successorsOfPredecessor.begin(), successorsOfPredecessor.end(), block.id)
                == successorsOfPredecessor.end()) {
                throw ControlFlowGraphError(
                    "CFG predecessor/successor lists are not symmetric");
            }
        }
    }

    std::vector<bool> reachable(blocks.size(), false);
    std::vector<CFGBlockId> pending{entryBlock};
    while (!pending.empty()) {
        const CFGBlockId block = pending.back();
        pending.pop_back();
        if (reachable[block]) {
            continue;
        }
        reachable[block] = true;
        for (const CFGBlockId successor : blocks[block].successors) {
            pending.push_back(successor);
        }
    }
    for (const CFGBlock& block : blocks) {
        if (block.reachable != reachable[block.id]) {
            throw ControlFlowGraphError("CFG reachability metadata is stale");
        }
    }

    for (const CFGDependencyAnchor& anchor : dependencyAnchors) {
        const std::size_t offset = anchor.dependency.instructionOffset;
        if (offset > instructionCount) {
            throw ControlFlowGraphError("CFG dependency offset is out of range");
        }
        const CFGBlockId expectedBlock = blockAtInstruction(instructionBlocks, offset, exitBlock);
        if (anchor.block != expectedBlock || anchor.block >= blocks.size()) {
            throw ControlFlowGraphError("CFG dependency anchor block is stale");
        }
        const std::size_t expectedOffset = anchor.block == exitBlock
            ? 0
            : offset - blocks[anchor.block].firstInstruction;
        if (anchor.offsetInBlock != expectedOffset) {
            throw ControlFlowGraphError("CFG dependency anchor offset is stale");
        }
    }
}

ControlFlowGraph buildControlFlowGraph(
    const std::vector<IRInstruction>& instructions,
    const std::vector<IRModuleDependency>& dependencies)
{
    ControlFlowGraph graph;
    graph.instructionCount = instructions.size();

    if (instructions.empty()) {
        graph.blocks.push_back(CFGBlock{0, 0, 0, true, false, {}, {}});
        graph.entryBlock = 0;
        graph.exitBlock = 0;
    } else {
        std::vector<std::size_t> starts{0};
        for (std::size_t instruction = 0; instruction < instructions.size(); ++instruction) {
            validateJumpTarget(instructions, instruction);
            const IROp op = instructions[instruction].op;
            if (isTerminator(op) && instruction + 1 < instructions.size()) {
                starts.push_back(instruction + 1);
            }
            if ((op == IROp::Jump || isConditionalJump(op))
                && instructions[instruction].operand < instructions.size()) {
                starts.push_back(instructions[instruction].operand);
            }
        }
        std::sort(starts.begin(), starts.end());
        starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

        graph.instructionBlocks.assign(instructions.size(), 0);
        for (std::size_t index = 0; index < starts.size(); ++index) {
            const std::size_t first = starts[index];
            const std::size_t end = index + 1 < starts.size()
                ? starts[index + 1]
                : instructions.size();
            const CFGBlockId blockId = graph.blocks.size();
            graph.blocks.push_back(CFGBlock{blockId, first, end, false, false, {}, {}});
            for (std::size_t instruction = first; instruction < end; ++instruction) {
                graph.instructionBlocks[instruction] = blockId;
            }
        }

        graph.entryBlock = 0;
        graph.exitBlock = graph.blocks.size();
        graph.blocks.push_back(CFGBlock{
            graph.exitBlock,
            instructions.size(),
            instructions.size(),
            true,
            false,
            {},
            {}});

        for (CFGBlockId blockId = 0; blockId < graph.exitBlock; ++blockId) {
            CFGBlock& block = graph.blocks[blockId];
            const IRInstruction& terminator = instructions[block.endInstruction - 1];
            const auto addSuccessor = [&block](CFGBlockId successor) {
                addUnique(block.successors, successor);
            };
            if (terminator.op == IROp::Jump) {
                addSuccessor(blockAtInstruction(
                    graph.instructionBlocks,
                    terminator.operand,
                    graph.exitBlock));
            } else if (isConditionalJump(terminator.op)) {
                addSuccessor(blockAtInstruction(
                    graph.instructionBlocks,
                    block.endInstruction,
                    graph.exitBlock));
                addSuccessor(blockAtInstruction(
                    graph.instructionBlocks,
                    terminator.operand,
                    graph.exitBlock));
            } else if (terminator.op == IROp::Return) {
                addSuccessor(graph.exitBlock);
            } else {
                addSuccessor(blockAtInstruction(
                    graph.instructionBlocks,
                    block.endInstruction,
                    graph.exitBlock));
            }
        }

        for (const CFGBlock& block : graph.blocks) {
            for (const CFGBlockId successor : block.successors) {
                addUnique(graph.blocks[successor].predecessors, block.id);
            }
        }
    }

    for (const IRModuleDependency& dependency : dependencies) {
        if (dependency.instructionOffset > instructions.size()) {
            throw ControlFlowGraphError("CFG dependency offset is out of range");
        }
        const CFGBlockId block = blockAtInstruction(
            graph.instructionBlocks,
            dependency.instructionOffset,
            graph.exitBlock);
        const std::size_t offsetInBlock = block == graph.exitBlock
            ? 0
            : dependency.instructionOffset - graph.blocks[block].firstInstruction;
        graph.dependencyAnchors.push_back(CFGDependencyAnchor{
            dependency,
            block,
            offsetInBlock});
    }

    markReachable(graph);
    graph.verify();
    return graph;
}

ControlFlowGraph buildControlFlowGraph(const IRProgram& program)
{
    return buildControlFlowGraph(program.instructions(), program.moduleDependencies());
}

ControlFlowGraph buildControlFlowGraph(const IRFunction& function)
{
    return buildControlFlowGraph(function.instructions);
}
