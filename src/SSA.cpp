#include "SSA.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace {

constexpr SSAValueId invalidSSAValue = std::numeric_limits<SSAValueId>::max();

struct DefinitionSite {
    CFGBlockId block = 0;
    std::optional<std::size_t> instructionIndex = std::nullopt;
};

struct RawDefinitionSite {
    CFGBlockId block = 0;
};

void defineValue(
    std::unordered_map<SSAValueId, DefinitionSite>& definitions,
    SSAValueId value,
    CFGBlockId block,
    std::optional<std::size_t> instructionIndex = std::nullopt)
{
    if (value == invalidSSAValue) {
        throw SSAError("SSA value uses the reserved invalid value ID");
    }
    const auto inserted = definitions.emplace(value, DefinitionSite{block, instructionIndex});
    if (!inserted.second) {
        throw SSAError("SSA value " + std::to_string(value) + " is defined more than once");
    }
}

void verifyUse(
    const std::unordered_map<SSAValueId, DefinitionSite>& definitions,
    SSAValueId value)
{
    if (definitions.find(value) == definitions.end()) {
        throw SSAError("undefined SSA value " + std::to_string(value));
    }
}

void verifyDominatingUse(
    const std::unordered_map<SSAValueId, DefinitionSite>& definitions,
    SSAValueId value,
    CFGBlockId useBlock,
    std::optional<std::size_t> useInstruction,
    const ControlFlowGraph& cfg,
    const DominanceInfo& dominance,
    bool phiIncoming)
{
    verifyUse(definitions, value);
    const DefinitionSite& definition = definitions.at(value);
    if (!cfg.blocks[useBlock].reachable) {
        return;
    }
    if (!cfg.blocks[definition.block].reachable
        || !dominance.dominates(definition.block, useBlock)) {
        throw SSAError(
            "SSA value " + std::to_string(value) + " does not dominate its use");
    }
    if (!phiIncoming && definition.block == useBlock && definition.instructionIndex
        && useInstruction && *definition.instructionIndex >= *useInstruction) {
        throw SSAError(
            "SSA value " + std::to_string(value)
            + " is used before its definition in block " + std::to_string(useBlock));
    }
}

bool isVariableMemoryOperation(IROp op)
{
    return op == IROp::LoadVar || op == IROp::StoreVar || op == IROp::AssignVar;
}

bool isMemoryDefinition(IROp op)
{
    return op == IROp::StoreVar || op == IROp::AssignVar;
}

bool isBinaryOperation(IROp op)
{
    switch (op) {
    case IROp::Add:
    case IROp::Subtract:
    case IROp::Multiply:
    case IROp::Divide:
    case IROp::Equal:
    case IROp::NotEqual:
    case IROp::Greater:
    case IROp::GreaterEqual:
    case IROp::Less:
    case IROp::LessEqual:
    case IROp::AddNum:
    case IROp::SubNum:
    case IROp::MulNum:
    case IROp::DivNum:
    case IROp::ConcatStr:
    case IROp::LessNum:
    case IROp::LessEqualNum:
    case IROp::GreaterNum:
    case IROp::GreaterEqualNum:
    case IROp::LessStr:
    case IROp::LessEqualStr:
    case IROp::GreaterStr:
    case IROp::GreaterEqualStr:
        return true;
    default:
        return false;
    }
}

void requireInstructionShape(
    const SSAInstruction& instruction,
    bool condition,
    const char* detail)
{
    if (!condition) {
        throw SSAError(
            "SSA " + irOpName(instruction.op) + " instruction has invalid " + detail);
    }
}

void validateInstructionShape(const SSAInstruction& instruction)
{
    const auto noResult = [&instruction] {
        return !instruction.result;
    };
    const auto hasResult = [&instruction] {
        return instruction.result.has_value();
    };
    const auto noLeft = [&instruction] {
        return !instruction.left;
    };
    const auto hasLeft = [&instruction] {
        return instruction.left.has_value();
    };
    const auto noRight = [&instruction] {
        return !instruction.right;
    };
    const auto hasRight = [&instruction] {
        return instruction.right.has_value();
    };
    const auto noArguments = [&instruction] {
        return instruction.arguments.empty();
    };

    if (isBinaryOperation(instruction.op)) {
        requireInstructionShape(
            instruction,
            hasResult() && hasLeft() && hasRight() && noArguments(),
            "operand shape");
        return;
    }

    switch (instruction.op) {
    case IROp::Constant:
    case IROp::MakeFunction:
        requireInstructionShape(instruction, hasResult() && noLeft() && noRight() && noArguments(), "operand shape");
        return;
    case IROp::Array:
    case IROp::Variant:
        requireInstructionShape(instruction, hasResult() && noLeft() && noRight(), "operand shape");
        return;
    case IROp::Map:
        requireInstructionShape(
            instruction,
            hasResult() && noLeft() && noRight() && instruction.arguments.size() % 2 == 0,
            "operand shape");
        return;
    case IROp::Struct:
        requireInstructionShape(
            instruction,
            hasResult() && noLeft() && noRight()
                && instruction.arguments.size() == instruction.operands.size(),
            "operand shape");
        return;
    case IROp::VariantTag:
    case IROp::VariantField:
    case IROp::Copy:
    case IROp::Field:
    case IROp::Len:
    case IROp::AssertArray:
    case IROp::AssertNumber:
    case IROp::Negate:
    case IROp::NegNum:
    case IROp::Not:
        requireInstructionShape(
            instruction,
            hasResult() && hasLeft() && noRight() && noArguments(),
            "operand shape");
        return;
    case IROp::LoadVar:
        requireInstructionShape(
            instruction,
            hasResult() && noLeft() && noRight() && noArguments(),
            "operand shape");
        return;
    case IROp::StoreVar:
    case IROp::AssignVar:
        requireInstructionShape(
            instruction,
            noResult() && hasLeft() && noRight() && noArguments(),
            "operand shape");
        return;
    case IROp::Call:
        requireInstructionShape(instruction, hasResult() && hasLeft() && noRight(), "operand shape");
        return;
    case IROp::NativeCall:
        requireInstructionShape(instruction, hasResult() && noLeft() && noRight(), "operand shape");
        return;
    case IROp::Index:
        requireInstructionShape(
            instruction,
            hasResult() && hasLeft() && hasRight() && noArguments(),
            "operand shape");
        return;
    case IROp::AssignIndex:
        requireInstructionShape(
            instruction,
            hasResult() && hasLeft() && hasRight() && instruction.arguments.size() == 1,
            "operand shape");
        return;
    case IROp::AssignField:
        requireInstructionShape(
            instruction,
            hasResult() && hasLeft() && noRight() && instruction.arguments.size() == 1,
            "operand shape");
        return;
    case IROp::Print:
    case IROp::Return:
        requireInstructionShape(
            instruction,
            noResult() && hasLeft() && noRight() && noArguments(),
            "operand shape");
        return;
    case IROp::Jump:
        requireInstructionShape(
            instruction,
            noResult() && noLeft() && noRight() && noArguments(),
            "operand shape");
        return;
    case IROp::JumpIfFalse:
    case IROp::JumpIfTrue:
        requireInstructionShape(
            instruction,
            noResult() && hasLeft() && noRight() && noArguments(),
            "operand shape");
        return;
    default:
        throw SSAError("SSA instruction has an unknown opcode");
    }
}

void observeSSAValue(
    std::set<SSAValueId>& values,
    SSAValueId value,
    SSAValueId& maximum,
    bool& hasMaximum)
{
    if (value == invalidSSAValue) {
        throw SSAError("SSA value uses the reserved invalid value ID");
    }
    values.insert(value);
    if (!hasMaximum || value > maximum) {
        maximum = value;
        hasMaximum = true;
    }
}

std::vector<SSAMove> lowerParallelCopies(
    std::vector<SSAMove> pending,
    std::set<SSAValueId>& usedValues,
    SSAValueId& nextTemporary,
    std::vector<SSAValueId>& temporaryValues)
{
    std::set<SSAValueId> destinations;
    for (const SSAMove& move : pending) {
        if (move.destination == invalidSSAValue || move.source == invalidSSAValue) {
            throw SSAError("SSA edge copy uses the reserved invalid value ID");
        }
        if (move.destination == move.source) {
            continue;
        }
        if (!destinations.insert(move.destination).second) {
            throw SSAError("SSA edge copy has duplicate destinations");
        }
    }
    pending.erase(
        std::remove_if(
            pending.begin(),
            pending.end(),
            [](const SSAMove& move) {
                return move.destination == move.source;
            }),
        pending.end());

    std::vector<SSAMove> ordered;
    ordered.reserve(pending.size() + 1);
    while (!pending.empty()) {
        std::set<SSAValueId> sources;
        for (const SSAMove& move : pending) {
            sources.insert(move.source);
        }

        const auto safe = std::find_if(
            pending.begin(),
            pending.end(),
            [&sources](const SSAMove& move) {
                return sources.find(move.destination) == sources.end();
            });
        if (safe != pending.end()) {
            ordered.push_back(*safe);
            pending.erase(safe);
            continue;
        }

        const SSAValueId cycleDestination = pending.front().destination;
        if (nextTemporary == invalidSSAValue) {
            throw SSAError("exhausted SSA temporary value IDs");
        }
        const SSAValueId temporary = nextTemporary++;
        while (usedValues.find(temporary) != usedValues.end()) {
            if (nextTemporary == invalidSSAValue) {
                throw SSAError("exhausted SSA temporary value IDs");
            }
            ++nextTemporary;
        }
        usedValues.insert(temporary);
        temporaryValues.push_back(temporary);
        ordered.push_back(SSAMove{temporary, cycleDestination});
        for (SSAMove& move : pending) {
            if (move.source == cycleDestination) {
                move.source = temporary;
            }
        }
    }
    return ordered;
}

void validateMemorySlot(
    const std::vector<SSAMemorySlot>& memorySlots,
    SSAMemorySlotId slot,
    const char* context)
{
    if (slot >= memorySlots.size()) {
        throw SSAError(
            std::string("SSA ") + context + " references an invalid memory slot");
    }
}

void validateRenameInput(
    const ControlFlowGraph& cfg,
    const SSAFunction& input,
    std::unordered_map<SSAValueId, RawDefinitionSite>& rawDefinitions)
{
    if (input.blocks.size() != cfg.blocks.size()) {
        throw SSAError("SSA block count does not match CFG block count");
    }

    for (SSAMemorySlotId id = 0; id < input.memorySlots.size(); ++id) {
        const SSAMemorySlot& slot = input.memorySlots[id];
        if (slot.id != id) {
            throw SSAError("SSA memory slot IDs are not deterministic");
        }
        if (slot.name.empty()) {
            throw SSAError("SSA memory slot name cannot be empty");
        }
        if (slot.bindingId && !slot.bindingId->valid()) {
            throw SSAError("SSA memory slot has an invalid binding ID");
        }
    }

    const auto defineRawValue = [&rawDefinitions](SSAValueId value, CFGBlockId block) {
        if (value == invalidSSAValue) {
            throw SSAError("SSA input uses the reserved invalid value ID");
        }
        const auto inserted = rawDefinitions.emplace(value, RawDefinitionSite{block});
        if (!inserted.second) {
            throw SSAError(
                "raw virtual register " + std::to_string(value)
                + " is defined more than once");
        }
    };

    for (const SSAParameter& parameter : input.parameters) {
        if (parameter.block >= cfg.exitBlock || parameter.block != cfg.entryBlock) {
            throw SSAError("SSA parameter is not defined in the entry block");
        }
        if (parameter.memorySlot) {
            validateMemorySlot(input.memorySlots, *parameter.memorySlot, "parameter");
        }
        defineRawValue(parameter.value, parameter.block);
    }

    for (CFGBlockId id = 0; id < cfg.blocks.size(); ++id) {
        const SSABlock& block = input.blocks[id];
        const CFGBlock& cfgBlock = cfg.blocks[id];
        if (block.id != id || block.firstInstruction != cfgBlock.firstInstruction
            || block.endInstruction != cfgBlock.endInstruction
            || block.syntheticExit != cfgBlock.syntheticExit) {
            throw SSAError("SSA block shape does not match CFG block " + std::to_string(id));
        }
        if (!block.phis.empty()) {
            throw SSAError("SSA rename input must not contain phi nodes");
        }
        if (block.syntheticExit && !block.instructions.empty()) {
            throw SSAError("SSA synthetic exit block cannot contain values");
        }

        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.originalInstruction < block.firstInstruction
                || instruction.originalInstruction >= block.endInstruction) {
                throw SSAError("SSA instruction does not belong to its CFG block");
            }
            if (instruction.memorySlot) {
                validateMemorySlot(input.memorySlots, *instruction.memorySlot, "instruction");
                if (!isVariableMemoryOperation(instruction.op)) {
                    throw SSAError("SSA memory slot is attached to a non-variable instruction");
                }
            }
            if (instruction.bindingId && !instruction.bindingId->valid()) {
                throw SSAError("SSA instruction has an invalid binding ID");
            }

            if (instruction.op == IROp::LoadVar) {
                if (!instruction.result) {
                    throw SSAError("SSA LoadVar instruction must define a result");
                }
            } else if (isMemoryDefinition(instruction.op)) {
                if (instruction.result) {
                    throw SSAError("SSA variable definition instruction cannot have a result");
                }
                if (!instruction.left) {
                    throw SSAError("SSA variable definition instruction must have a value operand");
                }
            }

            if (instruction.result) {
                defineRawValue(*instruction.result, block.id);
            }
        }
    }
}

bool isSSAControlFlowTerminator(IROp op)
{
    return op == IROp::Jump || op == IROp::JumpIfFalse || op == IROp::JumpIfTrue
        || op == IROp::Return;
}

bool isSSAConditionalJump(IROp op)
{
    return op == IROp::JumpIfFalse || op == IROp::JumpIfTrue;
}

CFGBlockId blockForSSAOffset(const ControlFlowGraph& cfg, std::size_t offset)
{
    if (offset > cfg.instructionCount) {
        throw SSAError("SSA branch target is out of range");
    }
    if (offset == cfg.instructionCount) {
        return cfg.exitBlock;
    }
    return cfg.instructionBlocks.at(offset);
}

CFGBlockId fallthroughSuccessor(
    const ControlFlowGraph& cfg,
    CFGBlockId block)
{
    return blockForSSAOffset(cfg, cfg.blocks[block].endInstruction);
}

void checkedLinearAdvance(std::size_t& value, std::size_t amount)
{
    if (amount > std::numeric_limits<std::size_t>::max() - value) {
        throw SSAError("exhausted linear de-SSA instruction offsets");
    }
    value += amount;
}

SSAInstruction liftIRInstruction(
    const IRInstruction& source,
    std::size_t originalInstruction)
{
    SSAInstruction lifted;
    lifted.op = source.op;
    if (source.dest) {
        lifted.result = source.dest->index;
    }
    if (source.left) {
        lifted.left = source.left->index;
    }
    if (source.right) {
        lifted.right = source.right->index;
    }
    lifted.arguments.reserve(source.arguments.size());
    for (const IRRegister argument : source.arguments) {
        lifted.arguments.push_back(argument.index);
    }
    lifted.originalInstruction = originalInstruction;
    lifted.operand = source.operand;
    lifted.operands = source.operands;
    lifted.typeNameOperand = source.typeNameOperand;
    lifted.variantNameOperand = source.variantNameOperand;
    lifted.span = source.span;
    lifted.bindingId = source.bindingId;
    validateInstructionShape(lifted);
    return lifted;
}

SSAFunction liftOrdinaryIRToSSA(
    const ControlFlowGraph& cfg,
    const std::vector<IRInstruction>& instructions,
    std::optional<std::size_t> registerCountHint)
{
    cfg.verify();
    if (cfg.instructionCount != instructions.size()) {
        throw SSAError("SSA input instruction count does not match CFG");
    }

    std::size_t registerSlotCount = registerCountHint.value_or(0);
    const auto observeRegister = [&registerSlotCount, &registerCountHint](SSAValueId value) {
        if (value == invalidSSAValue) {
            throw SSAError("SSA input uses the reserved invalid register ID");
        }
        if (registerCountHint && value >= *registerCountHint) {
            throw SSAError(
                "ordinary IR register " + std::to_string(value)
                + " is outside registerCount");
        }
        if (value >= registerSlotCount) {
            if (value == invalidSSAValue - 1) {
                throw SSAError("exhausted ordinary IR register slots");
            }
            registerSlotCount = value + 1;
        }
    };

    SSAFunction raw;
    raw.blocks.reserve(cfg.blocks.size());
    std::vector<std::set<CFGBlockId>> definitionBlocks;
    for (const CFGBlock& cfgBlock : cfg.blocks) {
        raw.blocks.push_back(SSABlock{
            cfgBlock.id,
            cfgBlock.firstInstruction,
            cfgBlock.endInstruction,
            cfgBlock.syntheticExit,
            {},
            {}});
    }

    for (const CFGBlock& cfgBlock : cfg.blocks) {
        SSABlock& block = raw.blocks[cfgBlock.id];
        block.instructions.reserve(cfgBlock.endInstruction - cfgBlock.firstInstruction);
        for (std::size_t index = cfgBlock.firstInstruction;
             index < cfgBlock.endInstruction;
             ++index) {
            SSAInstruction lifted = liftIRInstruction(instructions[index], index);
            if (lifted.result) {
                observeRegister(*lifted.result);
            }
            if (lifted.left) {
                observeRegister(*lifted.left);
            }
            if (lifted.right) {
                observeRegister(*lifted.right);
            }
            for (const SSAValueId argument : lifted.arguments) {
                observeRegister(argument);
            }
            block.instructions.push_back(std::move(lifted));
        }
    }
    definitionBlocks.resize(registerSlotCount);
    for (const CFGBlock& cfgBlock : cfg.blocks) {
        if (!cfgBlock.reachable || cfgBlock.syntheticExit) {
            continue;
        }
        for (const SSAInstruction& instruction : raw.blocks[cfgBlock.id].instructions) {
            if (instruction.result) {
                definitionBlocks[*instruction.result].insert(cfgBlock.id);
            }
        }
    }

    const DominanceInfo dominance = buildDominanceInfo(cfg);
    std::vector<std::vector<std::optional<std::size_t>>> phiIndexes(
        cfg.blocks.size(),
        std::vector<std::optional<std::size_t>>(registerSlotCount));
    SSAFunction result;
    result.blocks.reserve(cfg.blocks.size());
    for (const CFGBlock& cfgBlock : cfg.blocks) {
        result.blocks.push_back(SSABlock{
            cfgBlock.id,
            cfgBlock.firstInstruction,
            cfgBlock.endInstruction,
            cfgBlock.syntheticExit,
            {},
            {}});
    }

    for (SSARegisterSlotId slot = 0; slot < registerSlotCount; ++slot) {
        // A single defining block either dominates every use or the
        // ordinary register stream is malformed.  It does not need a phi;
        // inserting one would create an undefined incoming value on paths
        // where the slot was never initialized.
        if (definitionBlocks[slot].size() < 2) {
            continue;
        }
        std::set<CFGBlockId> worklist = definitionBlocks[slot];
        std::set<CFGBlockId> processed;
        std::set<CFGBlockId> phiBlocks;
        while (!worklist.empty()) {
            const CFGBlockId definitionBlock = *worklist.begin();
            worklist.erase(worklist.begin());
            if (!processed.insert(definitionBlock).second) {
                continue;
            }
            for (const CFGBlockId frontierBlock :
                 dominance.dominanceFrontiers[definitionBlock]) {
                const CFGBlock& block = cfg.blocks[frontierBlock];
                if (!block.reachable || block.syntheticExit) {
                    continue;
                }
                if (phiBlocks.insert(frontierBlock).second
                    && definitionBlocks[slot].find(frontierBlock)
                        == definitionBlocks[slot].end()) {
                    worklist.insert(frontierBlock);
                }
            }
        }

        for (const CFGBlockId block : phiBlocks) {
            SSAPhi phi;
            phi.result = 0;
            phi.registerSlot = slot;
            phi.incoming.reserve(cfg.blocks[block].predecessors.size());
            for (const CFGBlockId predecessor : cfg.blocks[block].predecessors) {
                phi.incoming.push_back(SSAIncoming{predecessor, invalidSSAValue});
            }
            phiIndexes[block][slot] = result.blocks[block].phis.size();
            result.blocks[block].phis.push_back(std::move(phi));
        }
    }

    SSAValueId nextValue = registerSlotCount;
    if (nextValue == invalidSSAValue) {
        throw SSAError("exhausted SSA value IDs while normalizing registers");
    }
    const auto allocateValue = [&nextValue]() {
        if (nextValue == invalidSSAValue) {
            throw SSAError("exhausted SSA value IDs while normalizing registers");
        }
        return nextValue++;
    };

    const auto allocatePhiValues = [&result, &allocateValue]() {
        for (SSABlock& block : result.blocks) {
            for (SSAPhi& phi : block.phis) {
                phi.result = allocateValue();
            }
        }
    };
    allocatePhiValues();

    std::vector<std::vector<SSAValueId>> registerStacks(registerSlotCount);
    std::vector<std::optional<SSAValueId>> firstValues(registerSlotCount);
    const auto allocateDefinition = [&firstValues, &allocateValue](SSARegisterSlotId slot) {
        if (!firstValues[slot]) {
            firstValues[slot] = slot;
            return slot;
        }
        return allocateValue();
    };
    const auto resolveCurrent = [&registerStacks](SSARegisterSlotId slot) {
        if (slot >= registerStacks.size() || registerStacks[slot].empty()) {
            throw SSAError(
                "ordinary IR register " + std::to_string(slot)
                + " is used before its definition");
        }
        return registerStacks[slot].back();
    };

    const auto fillSuccessorPhis = [
        &cfg,
        &result,
        &phiIndexes,
        &registerStacks,
        registerSlotCount](CFGBlockId block) {
        for (const CFGBlockId successor : cfg.blocks[block].successors) {
            if (!cfg.blocks[successor].reachable) {
                continue;
            }
            const auto predecessor = std::find(
                cfg.blocks[successor].predecessors.begin(),
                cfg.blocks[successor].predecessors.end(),
                block);
            if (predecessor == cfg.blocks[successor].predecessors.end()) {
                throw SSAError("CFG successor is missing its predecessor edge");
            }
            const std::size_t predecessorIndex = static_cast<std::size_t>(
                std::distance(cfg.blocks[successor].predecessors.begin(), predecessor));
            for (SSARegisterSlotId slot = 0; slot < registerSlotCount; ++slot) {
                const auto phiIndex = phiIndexes[successor][slot];
                if (!phiIndex) {
                    continue;
                }
                if (registerStacks[slot].empty()) {
                    throw SSAError(
                        "ordinary IR register " + std::to_string(slot)
                        + " has no value on predecessor " + std::to_string(block));
                }
                SSAPhi& phi = result.blocks[successor].phis[*phiIndex];
                if (predecessorIndex >= phi.incoming.size()) {
                    throw SSAError("SSA register phi incoming metadata is inconsistent");
                }
                phi.incoming[predecessorIndex].value = registerStacks[slot].back();
            }
        }
    };

    std::function<void(CFGBlockId)> renameReachable;
    renameReachable = [&](CFGBlockId blockId) {
        const CFGBlock& cfgBlock = cfg.blocks[blockId];
        if (!cfgBlock.reachable) {
            return;
        }

        std::vector<SSARegisterSlotId> pushedSlots;
        for (SSAPhi& phi : result.blocks[blockId].phis) {
            if (!phi.registerSlot) {
                throw SSAError("ordinary register phi is missing its source slot");
            }
            registerStacks[*phi.registerSlot].push_back(phi.result);
            pushedSlots.push_back(*phi.registerSlot);
        }

        for (const SSAInstruction& source : raw.blocks[blockId].instructions) {
            SSAInstruction lowered = source;
            if (source.left) {
                lowered.left = resolveCurrent(*source.left);
            }
            if (source.right) {
                lowered.right = resolveCurrent(*source.right);
            }
            for (std::size_t index = 0; index < source.arguments.size(); ++index) {
                lowered.arguments[index] = resolveCurrent(source.arguments[index]);
            }
            if (source.result) {
                const SSARegisterSlotId slot = *source.result;
                const SSAValueId value = allocateDefinition(slot);
                lowered.result = value;
                registerStacks[slot].push_back(value);
                pushedSlots.push_back(slot);
            }
            result.blocks[blockId].instructions.push_back(std::move(lowered));
        }

        fillSuccessorPhis(blockId);
        for (const CFGBlockId child : dominance.dominanceChildren[blockId]) {
            renameReachable(child);
        }

        for (auto it = pushedSlots.rbegin(); it != pushedSlots.rend(); ++it) {
            if (registerStacks[*it].empty()) {
                throw SSAError("ordinary register SSA stack underflow");
            }
            registerStacks[*it].pop_back();
        }
    };
    renameReachable(cfg.entryBlock);

    // Keep unreachable blocks in the internal shape. They do not contribute
    // phi inputs or dominance facts, but their uses still need a unique
    // definition so the SSA verifier can reject malformed streams uniformly.
    std::vector<std::optional<SSAValueId>> unreachableValues = firstValues;
    for (const CFGBlock& cfgBlock : cfg.blocks) {
        if (cfgBlock.reachable || cfgBlock.syntheticExit) {
            continue;
        }
        for (const SSAInstruction& source : raw.blocks[cfgBlock.id].instructions) {
            SSAInstruction lowered = source;
            const auto resolveUnreachable = [&unreachableValues](SSARegisterSlotId slot) {
                if (slot >= unreachableValues.size() || !unreachableValues[slot]) {
                    throw SSAError(
                        "ordinary IR register " + std::to_string(slot)
                        + " is used before its definition");
                }
                return *unreachableValues[slot];
            };
            if (source.left) {
                lowered.left = resolveUnreachable(*source.left);
            }
            if (source.right) {
                lowered.right = resolveUnreachable(*source.right);
            }
            for (std::size_t index = 0; index < source.arguments.size(); ++index) {
                lowered.arguments[index] = resolveUnreachable(source.arguments[index]);
            }
            if (source.result) {
                const SSARegisterSlotId slot = *source.result;
                const SSAValueId value = allocateDefinition(slot);
                lowered.result = value;
                unreachableValues[slot] = value;
            }
            result.blocks[cfgBlock.id].instructions.push_back(std::move(lowered));
        }
    }

    result.verify(cfg);
    return result;
}

} // namespace

bool SSAMemorySlot::canPromote() const
{
    return storage == SSAMemoryStorage::Local;
}

SSAError::SSAError(std::string message)
    : std::runtime_error(std::move(message))
{
}

void SSAFunction::verify(const ControlFlowGraph& cfg) const
{
    cfg.verify();
    const DominanceInfo dominance = buildDominanceInfo(cfg);
    if (blocks.size() != cfg.blocks.size()) {
        throw SSAError("SSA block count does not match CFG block count");
    }

    for (CFGBlockId id = 0; id < blocks.size(); ++id) {
        const SSABlock& block = blocks[id];
        const CFGBlock& cfgBlock = cfg.blocks[id];
        if (block.id != id || block.firstInstruction != cfgBlock.firstInstruction
            || block.endInstruction != cfgBlock.endInstruction
            || block.syntheticExit != cfgBlock.syntheticExit) {
            throw SSAError("SSA block shape does not match CFG block " + std::to_string(id));
        }
        if (block.syntheticExit && (!block.phis.empty() || !block.instructions.empty())) {
            throw SSAError("SSA synthetic exit block cannot contain values");
        }
    }

    for (SSAMemorySlotId id = 0; id < memorySlots.size(); ++id) {
        const SSAMemorySlot& slot = memorySlots[id];
        if (slot.id != id) {
            throw SSAError("SSA memory slot IDs are not deterministic");
        }
        if (slot.name.empty()) {
            throw SSAError("SSA memory slot name cannot be empty");
        }
        if (slot.bindingId && !slot.bindingId->valid()) {
            throw SSAError("SSA memory slot has an invalid binding ID");
        }
    }

    std::unordered_map<SSAValueId, DefinitionSite> definitions;
    for (const SSAParameter& parameter : parameters) {
        if (parameter.block >= cfg.exitBlock || parameter.block != cfg.entryBlock) {
            throw SSAError("SSA parameter is not defined in the entry block");
        }
        defineValue(definitions, parameter.value, parameter.block);
    }

    for (const SSABlock& block : blocks) {
        for (const SSAPhi& phi : block.phis) {
            if (phi.memorySlot && *phi.memorySlot >= memorySlots.size()) {
                throw SSAError("SSA phi references an invalid memory slot");
            }
            defineValue(definitions, phi.result, block.id);
        }
        for (std::size_t instructionIndex = 0;
             instructionIndex < block.instructions.size();
             ++instructionIndex) {
            const SSAInstruction& instruction = block.instructions[instructionIndex];
            if (instruction.originalInstruction < block.firstInstruction
                || instruction.originalInstruction >= block.endInstruction) {
                throw SSAError("SSA instruction does not belong to its CFG block");
            }
            validateInstructionShape(instruction);
            if (instruction.result) {
                defineValue(definitions, *instruction.result, block.id, instructionIndex);
            }
            if (instruction.memorySlot && *instruction.memorySlot >= memorySlots.size()) {
                throw SSAError("SSA instruction references an invalid memory slot");
            }
            if (instruction.memorySlot && !isVariableMemoryOperation(instruction.op)) {
                throw SSAError("SSA memory slot is attached to a non-variable instruction");
            }
        }
    }

    for (const SSABlock& block : blocks) {
        const CFGBlock& cfgBlock = cfg.blocks[block.id];
        for (const SSAPhi& phi : block.phis) {
            if (phi.memorySlot && phi.registerSlot) {
                throw SSAError("SSA phi cannot carry both memory and register slot metadata");
            }
            if (phi.registerSlot && *phi.registerSlot == invalidSSAValue) {
                throw SSAError("SSA register phi uses the reserved invalid slot ID");
            }
            if (phi.incoming.size() != cfgBlock.predecessors.size()) {
                throw SSAError("phi incoming count does not match CFG predecessors");
            }
            for (std::size_t index = 0; index < phi.incoming.size(); ++index) {
                const SSAIncoming& incoming = phi.incoming[index];
                if (incoming.predecessor != cfgBlock.predecessors[index]) {
                    throw SSAError("phi incoming predecessors are not in CFG order");
                }
                verifyDominatingUse(
                    definitions,
                    incoming.value,
                    incoming.predecessor,
                    std::nullopt,
                    cfg,
                    dominance,
                    true);
            }
        }

        for (std::size_t instructionIndex = 0;
             instructionIndex < block.instructions.size();
             ++instructionIndex) {
            const SSAInstruction& instruction = block.instructions[instructionIndex];
            if (instruction.left) {
                verifyDominatingUse(
                    definitions,
                    *instruction.left,
                    block.id,
                    instructionIndex,
                    cfg,
                    dominance,
                    false);
            }
            if (instruction.right) {
                verifyDominatingUse(
                    definitions,
                    *instruction.right,
                    block.id,
                    instructionIndex,
                    cfg,
                    dominance,
                    false);
            }
            for (const SSAValueId argument : instruction.arguments) {
                verifyDominatingUse(
                    definitions,
                    argument,
                    block.id,
                    instructionIndex,
                    cfg,
                    dominance,
                    false);
            }
        }
    }

    for (const SSAParameter& parameter : parameters) {
        if (parameter.memorySlot && *parameter.memorySlot >= memorySlots.size()) {
            throw SSAError("SSA parameter references an invalid memory slot");
        }
    }
}

SSAFunction makeSSAFunction(const ControlFlowGraph& cfg)
{
    SSAFunction result;
    result.blocks.reserve(cfg.blocks.size());
    for (const CFGBlock& block : cfg.blocks) {
        result.blocks.push_back(SSABlock{
            block.id,
            block.firstInstruction,
            block.endInstruction,
            block.syntheticExit,
            {},
            {}});
    }
    result.verify(cfg);
    return result;
}

SSAFunction liftIRToSSA(
    const ControlFlowGraph& cfg,
    const std::vector<IRInstruction>& instructions)
{
    return liftOrdinaryIRToSSA(cfg, instructions, std::nullopt);
}

SSAFunction liftIRToSSA(const ControlFlowGraph& cfg, const IRFunction& function)
{
    return liftOrdinaryIRToSSA(cfg, function.instructions, function.registerCount);
}

std::vector<SSAPhiPlacement> placePromotableMemoryPhis(
    const ControlFlowGraph& cfg,
    const DominanceInfo& dominance,
    const std::vector<SSAMemorySlot>& memorySlots,
    const std::vector<SSAMemoryDefinition>& definitions)
{
    cfg.verify();
    dominance.verify(cfg);

    for (SSAMemorySlotId id = 0; id < memorySlots.size(); ++id) {
        const SSAMemorySlot& slot = memorySlots[id];
        if (slot.id != id) {
            throw SSAError("SSA memory slot IDs are not deterministic");
        }
        if (slot.name.empty()) {
            throw SSAError("SSA memory slot name cannot be empty");
        }
    }

    std::vector<std::set<CFGBlockId>> definitionBlocks(memorySlots.size());
    for (const SSAMemoryDefinition& definition : definitions) {
        if (definition.slot >= memorySlots.size()) {
            throw SSAError("SSA memory definition references an invalid slot");
        }
        if (definition.block >= cfg.blocks.size()) {
            throw SSAError("SSA memory definition references an invalid block");
        }
        const SSAMemorySlot& slot = memorySlots[definition.slot];
        const CFGBlock& block = cfg.blocks[definition.block];
        if (!slot.canPromote() || !block.reachable || block.syntheticExit) {
            continue;
        }
        definitionBlocks[definition.slot].insert(definition.block);
    }

    std::vector<SSAPhiPlacement> result;
    for (SSAMemorySlotId slot = 0; slot < memorySlots.size(); ++slot) {
        if (!memorySlots[slot].canPromote()) {
            continue;
        }

        std::set<CFGBlockId> worklist = definitionBlocks[slot];
        std::set<CFGBlockId> processed;
        std::set<CFGBlockId> phiBlocks;
        while (!worklist.empty()) {
            const CFGBlockId definitionBlock = *worklist.begin();
            worklist.erase(worklist.begin());
            if (!processed.insert(definitionBlock).second) {
                continue;
            }

            for (const CFGBlockId frontierBlock :
                 dominance.dominanceFrontiers[definitionBlock]) {
                const CFGBlock& block = cfg.blocks[frontierBlock];
                if (!block.reachable || block.syntheticExit) {
                    continue;
                }
                if (phiBlocks.insert(frontierBlock).second
                    && definitionBlocks[slot].find(frontierBlock)
                        == definitionBlocks[slot].end()) {
                    worklist.insert(frontierBlock);
                }
            }
        }

        for (const CFGBlockId block : phiBlocks) {
            result.push_back(SSAPhiPlacement{slot, block});
        }
    }
    return result;
}

SSAFunction renamePromotableMemorySlots(
    const ControlFlowGraph& cfg,
    const DominanceInfo& dominance,
    const SSAFunction& input)
{
    cfg.verify();
    dominance.verify(cfg);

    std::unordered_map<SSAValueId, RawDefinitionSite> rawDefinitions;
    validateRenameInput(cfg, input, rawDefinitions);

    std::vector<SSAMemoryDefinition> definitions;
    for (const SSABlock& block : input.blocks) {
        if (!cfg.blocks[block.id].reachable || block.syntheticExit) {
            continue;
        }
        for (const SSAInstruction& instruction : block.instructions) {
            if (!isMemoryDefinition(instruction.op) || !instruction.memorySlot) {
                continue;
            }
            definitions.push_back(SSAMemoryDefinition{*instruction.memorySlot, block.id});
        }
    }

    const std::vector<SSAPhiPlacement> placements = placePromotableMemoryPhis(
        cfg,
        dominance,
        input.memorySlots,
        definitions);

    SSAFunction result;
    result.memorySlots = input.memorySlots;
    result.blocks.reserve(cfg.blocks.size());
    for (const CFGBlock& block : cfg.blocks) {
        result.blocks.push_back(SSABlock{
            block.id,
            block.firstInstruction,
            block.endInstruction,
            block.syntheticExit,
            {},
            {}});
    }

    std::vector<std::vector<std::optional<std::size_t>>> phiIndexes(
        cfg.blocks.size(),
        std::vector<std::optional<std::size_t>>(input.memorySlots.size()));

    SSAValueId nextValue = 0;
    const auto allocateValue = [&nextValue]() {
        if (nextValue == invalidSSAValue) {
            throw SSAError("exhausted SSA value IDs");
        }
        return nextValue++;
    };

    std::unordered_map<SSAValueId, SSAValueId> renamedValues;
    result.parameters.reserve(input.parameters.size());
    std::vector<std::vector<SSAValueId>> memoryStacks(input.memorySlots.size());
    std::vector<bool> parameterSlots(input.memorySlots.size(), false);
    for (const SSAParameter& parameter : input.parameters) {
        const SSAValueId value = allocateValue();
        renamedValues.emplace(parameter.value, value);
        result.parameters.push_back(SSAParameter{value, parameter.block, parameter.memorySlot});
        if (parameter.memorySlot && input.memorySlots[*parameter.memorySlot].canPromote()) {
            const SSAMemorySlotId slot = *parameter.memorySlot;
            if (parameterSlots[slot]) {
                throw SSAError(
                    "multiple SSA parameters initialize memory slot "
                    + std::to_string(slot));
            }
            parameterSlots[slot] = true;
            memoryStacks[slot].push_back(value);
        }
    }

    for (const SSAPhiPlacement& placement : placements) {
        if (placement.block >= result.blocks.size()) {
            throw SSAError("SSA phi placement references an invalid block");
        }
        if (phiIndexes[placement.block][placement.slot]) {
            throw SSAError("duplicate SSA memory phi placement");
        }

        SSAPhi phi;
        phi.result = allocateValue();
        phi.memorySlot = placement.slot;
        phi.incoming.reserve(cfg.blocks[placement.block].predecessors.size());
        for (const CFGBlockId predecessor : cfg.blocks[placement.block].predecessors) {
            phi.incoming.push_back(SSAIncoming{predecessor, invalidSSAValue});
        }
        phiIndexes[placement.block][placement.slot] = result.blocks[placement.block].phis.size();
        result.blocks[placement.block].phis.push_back(std::move(phi));
    }

    const auto currentMemoryValue = [&memoryStacks, &input](SSAMemorySlotId slot) {
        if (memoryStacks[slot].empty()) {
            throw SSAError(
                "local memory slot " + std::to_string(slot) + " (`"
                + input.memorySlots[slot].name + "`) is used before definition");
        }
        return memoryStacks[slot].back();
    };

    const auto resolveValue = [
        &cfg,
        &dominance,
        &rawDefinitions,
        &renamedValues](SSAValueId rawValue, CFGBlockId useBlock) {
        if (rawValue == invalidSSAValue) {
            throw SSAError("SSA input uses the reserved invalid value ID");
        }
        const auto definition = rawDefinitions.find(rawValue);
        if (definition == rawDefinitions.end()) {
            throw SSAError("undefined raw virtual register " + std::to_string(rawValue));
        }
        const CFGBlockId definitionBlock = definition->second.block;
        if (cfg.blocks[useBlock].reachable) {
            if (!cfg.blocks[definitionBlock].reachable
                || !dominance.dominates(definitionBlock, useBlock)) {
                throw SSAError(
                    "raw virtual register " + std::to_string(rawValue)
                    + " does not dominate its use");
            }
        } else if (!cfg.blocks[definitionBlock].reachable && definitionBlock != useBlock) {
            throw SSAError(
                "raw virtual register " + std::to_string(rawValue)
                + " does not dominate its unreachable use");
        }

        const auto renamed = renamedValues.find(rawValue);
        if (renamed == renamedValues.end()) {
            throw SSAError(
                "raw virtual register " + std::to_string(rawValue)
                + " is not available while renaming");
        }
        return renamed->second;
    };

    const auto fillSuccessorPhis = [
        &cfg,
        &result,
        &phiIndexes,
        &memoryStacks](CFGBlockId block) {
        for (const CFGBlockId successor : cfg.blocks[block].successors) {
            if (!cfg.blocks[successor].reachable) {
                continue;
            }
            const auto predecessor = std::find(
                cfg.blocks[successor].predecessors.begin(),
                cfg.blocks[successor].predecessors.end(),
                block);
            if (predecessor == cfg.blocks[successor].predecessors.end()) {
                throw SSAError("CFG successor is missing its predecessor edge");
            }
            const std::size_t predecessorIndex = static_cast<std::size_t>(
                std::distance(cfg.blocks[successor].predecessors.begin(), predecessor));
            for (const SSAPhi& currentPhi : result.blocks[successor].phis) {
                if (!currentPhi.memorySlot) {
                    continue;
                }
                const SSAMemorySlotId slot = *currentPhi.memorySlot;
                if (memoryStacks[slot].empty()) {
                    throw SSAError(
                        "local memory slot " + std::to_string(slot)
                        + " has no value on predecessor " + std::to_string(block));
                }
                const auto index = phiIndexes[successor][slot];
                if (!index || predecessorIndex >= result.blocks[successor].phis[*index].incoming.size()) {
                    throw SSAError("SSA phi incoming metadata is inconsistent");
                }
                result.blocks[successor].phis[*index].incoming[predecessorIndex].value
                    = memoryStacks[slot].back();
            }
        }
    };

    std::function<void(CFGBlockId)> renameBlock;
    renameBlock = [&](CFGBlockId blockId) {
        const CFGBlock& cfgBlock = cfg.blocks[blockId];
        if (!cfgBlock.reachable) {
            return;
        }

        std::vector<SSAMemorySlotId> pushedSlots;
        for (SSAPhi& phi : result.blocks[blockId].phis) {
            if (!phi.memorySlot) {
                throw SSAError("SSA memory phi is missing its memory slot");
            }
            memoryStacks[*phi.memorySlot].push_back(phi.result);
            pushedSlots.push_back(*phi.memorySlot);
        }

        for (const SSAInstruction& source : input.blocks[blockId].instructions) {
            SSAInstruction lowered = source;
            const bool promotableLoad = source.op == IROp::LoadVar && source.memorySlot
                && input.memorySlots[*source.memorySlot].canPromote();
            const bool promotableStore = isMemoryDefinition(source.op) && source.memorySlot
                && input.memorySlots[*source.memorySlot].canPromote();

            if (source.left) {
                lowered.left = resolveValue(*source.left, blockId);
            }
            if (source.right) {
                lowered.right = resolveValue(*source.right, blockId);
            }
            for (std::size_t index = 0; index < source.arguments.size(); ++index) {
                lowered.arguments[index] = resolveValue(source.arguments[index], blockId);
            }

            if (promotableLoad) {
                const SSAValueId value = currentMemoryValue(*source.memorySlot);
                if (!source.result) {
                    throw SSAError("promotable LoadVar instruction must define a result");
                }
                renamedValues.emplace(*source.result, value);
                continue;
            }

            if (promotableStore) {
                if (!source.left) {
                    throw SSAError("promotable variable definition has no value operand");
                }
                memoryStacks[*source.memorySlot].push_back(*lowered.left);
                pushedSlots.push_back(*source.memorySlot);
                continue;
            }

            if (source.result) {
                const SSAValueId value = allocateValue();
                lowered.result = value;
                const auto inserted = renamedValues.emplace(*source.result, value);
                if (!inserted.second) {
                    throw SSAError(
                        "raw virtual register " + std::to_string(*source.result)
                        + " is renamed more than once");
                }
            } else {
                lowered.result = std::nullopt;
            }
            result.blocks[blockId].instructions.push_back(std::move(lowered));
        }

        fillSuccessorPhis(blockId);

        for (const CFGBlockId child : dominance.dominanceChildren[blockId]) {
            renameBlock(child);
        }

        for (auto it = pushedSlots.rbegin(); it != pushedSlots.rend(); ++it) {
            if (memoryStacks[*it].empty()) {
                throw SSAError("SSA memory stack underflow");
            }
            memoryStacks[*it].pop_back();
        }
    };

    renameBlock(cfg.entryBlock);

    // Dominance metadata intentionally excludes unreachable blocks. Preserve
    // their instruction shape with a deterministic linear rename so the
    // resulting function remains a complete internal representation; no
    // unreachable block can contribute a phi input or a reachable value.
    for (const CFGBlock& cfgBlock : cfg.blocks) {
        if (cfgBlock.reachable || cfgBlock.syntheticExit) {
            continue;
        }
        std::vector<SSAMemorySlotId> pushedSlots;
        for (const SSAInstruction& source : input.blocks[cfgBlock.id].instructions) {
            SSAInstruction lowered = source;
            const bool promotableLoad = source.op == IROp::LoadVar && source.memorySlot
                && input.memorySlots[*source.memorySlot].canPromote();
            const bool promotableStore = isMemoryDefinition(source.op) && source.memorySlot
                && input.memorySlots[*source.memorySlot].canPromote();

            if (source.left) {
                lowered.left = resolveValue(*source.left, cfgBlock.id);
            }
            if (source.right) {
                lowered.right = resolveValue(*source.right, cfgBlock.id);
            }
            for (std::size_t index = 0; index < source.arguments.size(); ++index) {
                lowered.arguments[index] = resolveValue(source.arguments[index], cfgBlock.id);
            }

            if (promotableLoad) {
                if (memoryStacks[*source.memorySlot].empty()) {
                    throw SSAError(
                        "local memory slot " + std::to_string(*source.memorySlot)
                        + " is used before definition");
                }
                renamedValues.emplace(*source.result, memoryStacks[*source.memorySlot].back());
                continue;
            }
            if (promotableStore) {
                memoryStacks[*source.memorySlot].push_back(*lowered.left);
                pushedSlots.push_back(*source.memorySlot);
                continue;
            }

            if (source.result) {
                const SSAValueId value = allocateValue();
                lowered.result = value;
                const auto inserted = renamedValues.emplace(*source.result, value);
                if (!inserted.second) {
                    throw SSAError(
                        "raw virtual register " + std::to_string(*source.result)
                        + " is renamed more than once");
                }
            } else {
                lowered.result = std::nullopt;
            }
            result.blocks[cfgBlock.id].instructions.push_back(std::move(lowered));
        }
        for (auto it = pushedSlots.rbegin(); it != pushedSlots.rend(); ++it) {
            memoryStacks[*it].pop_back();
        }
    }

    for (const auto& blockPhis : result.blocks) {
        for (const SSAPhi& phi : blockPhis.phis) {
            for (const SSAIncoming& incoming : phi.incoming) {
                if (incoming.value == invalidSSAValue) {
                    throw SSAError(
                        "SSA phi for memory slot "
                        + std::to_string(phi.memorySlot.value_or(0))
                        + " has an undefined incoming value");
                }
            }
        }
    }

    result.verify(cfg);
    return result;
}

SSADeSSACopyPlan planSSADeSSACopies(
    const ControlFlowGraph& cfg,
    const SSAFunction& input)
{
    cfg.verify();
    input.verify(cfg);

    SSADeSSACopyPlan result;
    std::set<SSAValueId> usedValues;
    SSAValueId maximum = 0;
    bool hasMaximum = false;
    const auto observe = [&usedValues, &maximum, &hasMaximum](SSAValueId value) {
        observeSSAValue(usedValues, value, maximum, hasMaximum);
    };

    for (const SSAParameter& parameter : input.parameters) {
        observe(parameter.value);
    }
    for (const SSABlock& block : input.blocks) {
        for (const SSAPhi& phi : block.phis) {
            observe(phi.result);
            for (const SSAIncoming& incoming : phi.incoming) {
                observe(incoming.value);
            }
        }
        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.result) {
                observe(*instruction.result);
            }
            if (instruction.left) {
                observe(*instruction.left);
            }
            if (instruction.right) {
                observe(*instruction.right);
            }
            for (const SSAValueId argument : instruction.arguments) {
                observe(argument);
            }
        }
    }

    SSAValueId nextTemporary = hasMaximum ? maximum + 1 : 0;
    if (hasMaximum && maximum == invalidSSAValue - 1) {
        nextTemporary = invalidSSAValue;
    }

    for (const SSABlock& successorBlock : input.blocks) {
        if (successorBlock.phis.empty()) {
            continue;
        }
        const CFGBlock& cfgSuccessor = cfg.blocks[successorBlock.id];
        for (std::size_t predecessorIndex = 0;
             predecessorIndex < cfgSuccessor.predecessors.size();
             ++predecessorIndex) {
            const CFGBlockId predecessor = cfgSuccessor.predecessors[predecessorIndex];
            std::vector<SSAMove> pending;
            pending.reserve(successorBlock.phis.size());
            for (const SSAPhi& phi : successorBlock.phis) {
                if (phi.incoming.size() != cfgSuccessor.predecessors.size()) {
                    throw SSAError("SSA phi incoming count changed during de-SSA planning");
                }
                const SSAValueId source = phi.incoming[predecessorIndex].value;
                if (source != phi.result) {
                    pending.push_back(SSAMove{phi.result, source});
                }
            }
            if (pending.empty()) {
                continue;
            }

            SSAEdgeCopyBundle bundle;
            bundle.predecessor = predecessor;
            bundle.successor = successorBlock.id;
            bundle.requiresCriticalEdgeSplit = cfg.blocks[predecessor].successors.size() > 1
                && cfgSuccessor.predecessors.size() > 1;
            bundle.moves = lowerParallelCopies(
                std::move(pending),
                usedValues,
                nextTemporary,
                result.temporaryValues);
            result.edgeCopies.push_back(std::move(bundle));
        }
    }
    return result;
}

void SSADeSSALinearFunction::verify(const ControlFlowGraph& cfg) const
{
    cfg.verify();
    if (blocks.empty()) {
        throw SSAError("linear de-SSA result must contain a synthetic exit block");
    }
    if (blockEntryOffsets.size() != cfg.blocks.size()) {
        throw SSAError("linear de-SSA block-entry map has the wrong size");
    }
    if (originalInstructionOffsets.size() != cfg.instructionCount) {
        throw SSAError("linear de-SSA original-instruction map has the wrong size");
    }
    if (originalInsertionOffsets.size() != cfg.instructionCount + 1) {
        throw SSAError("linear de-SSA insertion map has the wrong size");
    }

    for (const SSAParameter& parameter : parameters) {
        if (parameter.value == invalidSSAValue
            || parameter.block != cfg.entryBlock
            || parameter.block >= cfg.exitBlock) {
            throw SSAError("linear de-SSA parameter is not an entry definition");
        }
    }
    for (SSAMemorySlotId id = 0; id < memorySlots.size(); ++id) {
        const SSAMemorySlot& slot = memorySlots[id];
        if (slot.id != id || slot.name.empty()) {
            throw SSAError("linear de-SSA memory slot metadata is malformed");
        }
        if (slot.bindingId && !slot.bindingId->valid()) {
            throw SSAError("linear de-SSA memory slot has an invalid binding ID");
        }
    }

    const std::size_t invalidOffset = std::numeric_limits<std::size_t>::max();
    std::vector<bool> sourceBlocks(cfg.blocks.size(), false);
    std::set<std::pair<CFGBlockId, CFGBlockId>> splitEdges;
    std::set<std::size_t> blockStarts;
    for (const SSADeSSABlock& block : blocks) {
        blockStarts.insert(block.firstInstruction);
    }
    std::size_t nextInstruction = 0;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const SSADeSSABlock& block = blocks[index];
        if (block.id != index || block.firstInstruction != nextInstruction
            || block.endInstruction < block.firstInstruction
            || block.endInstruction > instructions.size()) {
            throw SSAError("linear de-SSA block ranges are not contiguous");
        }
        if (block.sourceBlock && block.splitEdge) {
            throw SSAError("linear de-SSA block cannot be both source and split");
        }
        if (!block.sourceBlock && !block.splitEdge) {
            throw SSAError("linear de-SSA block has no source or split identity");
        }

        if (block.sourceBlock) {
            const CFGBlockId source = *block.sourceBlock;
            if (source >= cfg.blocks.size() || sourceBlocks[source]) {
                throw SSAError("linear de-SSA source block identity is malformed");
            }
            sourceBlocks[source] = true;
            if (blockEntryOffsets[source] != block.firstInstruction) {
                throw SSAError("linear de-SSA source block entry map is stale");
            }
            if (block.syntheticExit != cfg.blocks[source].syntheticExit) {
                throw SSAError("linear de-SSA synthetic-exit metadata is stale");
            }
            if (source == cfg.exitBlock
                && (index + 1 != blocks.size()
                    || block.firstInstruction != instructions.size()
                    || block.endInstruction != instructions.size())) {
                throw SSAError("linear de-SSA exit block is not last or empty");
            }
            if (source != cfg.exitBlock && block.syntheticExit) {
                throw SSAError("linear de-SSA ordinary block is marked synthetic exit");
            }
        } else {
            if (block.syntheticExit || !block.splitEdge) {
                throw SSAError("linear de-SSA split block metadata is malformed");
            }
            const CFGBlockId predecessor = block.splitEdge->first;
            const CFGBlockId successor = block.splitEdge->second;
            if (predecessor >= cfg.blocks.size() || successor >= cfg.blocks.size()
                || predecessor == cfg.exitBlock || successor == cfg.exitBlock
                || cfg.blocks[predecessor].successors.size() <= 1
                || cfg.blocks[successor].predecessors.size() <= 1
                || !splitEdges.insert(*block.splitEdge).second) {
                throw SSAError("linear de-SSA split edge is malformed");
            }
            if (block.endInstruction - block.firstInstruction < 1) {
                throw SSAError("linear de-SSA split block is empty");
            }
        }

        std::optional<std::size_t> previousOriginal;
        for (std::size_t instructionIndex = block.firstInstruction;
             instructionIndex < block.endInstruction;
             ++instructionIndex) {
            const SSADeSSAInstruction& current = instructions[instructionIndex];
            validateInstructionShape(current.instruction);
            const bool isLast = instructionIndex + 1 == block.endInstruction;
            if (current.instruction.op == IROp::Jump
                || isSSAConditionalJump(current.instruction.op)) {
                if (current.instruction.operand > instructions.size()
                    || blockStarts.find(current.instruction.operand) == blockStarts.end()) {
                    throw SSAError("linear de-SSA jump target is not a block entry");
                }
            }

            if (!block.sourceBlock) {
                if (!current.synthetic
                    || (!isLast && current.instruction.op != IROp::Copy)
                    || (isLast && current.instruction.op != IROp::Jump)) {
                    throw SSAError("linear de-SSA split block has invalid instruction shape");
                }
                continue;
            }

            if (current.synthetic) {
                const bool isExitFallthroughBarrier =
                    current.instruction.op == IROp::Jump
                    && isLast
                    && *block.sourceBlock + 1 == cfg.exitBlock
                    && current.instruction.operand == blockEntryOffsets[cfg.exitBlock];
                if (current.instruction.op != IROp::Copy
                    && !isExitFallthroughBarrier) {
                    throw SSAError("linear de-SSA source block has invalid synthetic instruction");
                }
                continue;
            }
            if (*block.sourceBlock == cfg.exitBlock
                || current.instruction.originalInstruction >= cfg.instructionCount
                || current.instruction.originalInstruction
                    < cfg.blocks[*block.sourceBlock].firstInstruction
                || current.instruction.originalInstruction
                    >= cfg.blocks[*block.sourceBlock].endInstruction) {
                throw SSAError("linear de-SSA instruction has an invalid source location");
            }
            if (previousOriginal
                && current.instruction.originalInstruction <= *previousOriginal) {
                throw SSAError("linear de-SSA source instructions are not ordered");
            }
            previousOriginal = current.instruction.originalInstruction;
            const auto& mapped = originalInstructionOffsets[current.instruction.originalInstruction];
            if (!mapped || *mapped != instructionIndex) {
                throw SSAError("linear de-SSA original-instruction map is stale");
            }
        }

        if (!block.sourceBlock && instructions[block.endInstruction - 1].instruction.operand
                != blockEntryOffsets[block.splitEdge->second]) {
            throw SSAError("linear de-SSA split jump target is stale");
        }
        nextInstruction = block.endInstruction;
    }

    if (blocks.back().sourceBlock != std::optional<CFGBlockId>(cfg.exitBlock)
        || !blocks.back().syntheticExit || nextInstruction != instructions.size()) {
        throw SSAError("linear de-SSA result has no final synthetic exit block");
    }
    for (const bool seen : sourceBlocks) {
        if (!seen) {
            throw SSAError("linear de-SSA result does not cover every CFG block");
        }
    }
    for (const std::size_t entry : blockEntryOffsets) {
        if (entry == invalidOffset || entry > instructions.size()) {
            throw SSAError("linear de-SSA block-entry map contains an invalid offset");
        }
    }
    for (std::size_t instruction = 0; instruction < cfg.instructionCount; ++instruction) {
        if (originalInstructionOffsets[instruction]) {
            const std::size_t offset = *originalInstructionOffsets[instruction];
            if (offset >= instructions.size() || instructions[offset].synthetic
                || instructions[offset].instruction.originalInstruction != instruction) {
                throw SSAError("linear de-SSA original-instruction map points to the wrong instruction");
            }
        }
        if (originalInsertionOffsets[instruction] > instructions.size()) {
            throw SSAError("linear de-SSA insertion map contains an invalid offset");
        }
    }
    if (originalInsertionOffsets.back() != instructions.size()) {
        throw SSAError("linear de-SSA end insertion offset is stale");
    }

    for (std::size_t index = 0; index < moduleDependencies.size(); ++index) {
        if (index >= cfg.dependencyAnchors.size()) {
            throw SSAError("linear de-SSA has extra module dependency metadata");
        }
        const IRModuleDependency& actual = moduleDependencies[index];
        const IRModuleDependency& expected = cfg.dependencyAnchors[index].dependency;
        if (actual.importedModuleId != expected.importedModuleId
            || actual.kind != expected.kind
            || actual.requestedPath != expected.requestedPath
            || expected.instructionOffset > cfg.instructionCount
            || actual.instructionOffset != originalInsertionOffsets[expected.instructionOffset]) {
            throw SSAError("linear de-SSA module dependency offset is stale");
        }
    }
    if (moduleDependencies.size() != cfg.dependencyAnchors.size()) {
        throw SSAError("linear de-SSA module dependency metadata has the wrong size");
    }

    std::set<SSAValueId> temporaryValuesSet;
    std::set<SSAValueId> syntheticCopyResults;
    for (const SSAValueId value : temporaryValues) {
        if (value == invalidSSAValue || !temporaryValuesSet.insert(value).second) {
            throw SSAError("linear de-SSA temporary value metadata is malformed");
        }
    }
    for (const SSADeSSAInstruction& current : instructions) {
        if (current.synthetic && current.instruction.op == IROp::Copy
            && current.instruction.result) {
            syntheticCopyResults.insert(*current.instruction.result);
        }
    }
    for (const SSAValueId value : temporaryValues) {
        if (syntheticCopyResults.find(value) == syntheticCopyResults.end()) {
            throw SSAError("linear de-SSA temporary value is not materialized");
        }
    }
}

SSADeSSALinearFunction lowerSSADeSSACopies(
    const ControlFlowGraph& cfg,
    const SSAFunction& input,
    const std::vector<std::optional<SourceSpan>>* sourceSpans)
{
    cfg.verify();
    input.verify(cfg);
    const SSADeSSACopyPlan plan = planSSADeSSACopies(cfg, input);

    // Optimizer-generated copies and control-flow barriers still execute on
    // behalf of a source instruction.  Preserve that instruction's source
    // span so trace/debug consumers never lose the source location merely
    // because de-SSA inserted a legal implementation detail.
    std::vector<std::optional<SourceSpan>> anchorSpans(cfg.instructionCount);
    for (const SSABlock& block : input.blocks) {
        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.originalInstruction >= anchorSpans.size()) {
                throw SSAError("SSA de-SSA source span anchor is out of range");
            }
            anchorSpans[instruction.originalInstruction] = instruction.span;
        }
    }
    if (sourceSpans) {
        if (sourceSpans->size() != anchorSpans.size()) {
            throw SSAError("SSA de-SSA source span table has the wrong size");
        }
        for (std::size_t index = 0; index < anchorSpans.size(); ++index) {
            if ((*sourceSpans)[index]) {
                anchorSpans[index] = (*sourceSpans)[index];
            }
        }
    }
    const auto sourceSpanForAnchor = [&anchorSpans](std::size_t anchor) {
        return anchor < anchorSpans.size()
            ? anchorSpans[anchor]
            : std::optional<SourceSpan>{};
    };

    using EdgeKey = std::pair<CFGBlockId, CFGBlockId>;
    std::map<EdgeKey, SSAEdgeCopyBundle> bundles;
    for (const SSAEdgeCopyBundle& bundle : plan.edgeCopies) {
        if (bundle.predecessor >= cfg.blocks.size()
            || bundle.successor >= cfg.blocks.size()
            || bundle.predecessor == cfg.exitBlock
            || bundle.successor == cfg.exitBlock) {
            throw SSAError("SSA de-SSA copy edge references the synthetic exit");
        }
        const bool critical = cfg.blocks[bundle.predecessor].successors.size() > 1
            && cfg.blocks[bundle.successor].predecessors.size() > 1;
        if (bundle.requiresCriticalEdgeSplit != critical
            || !bundles.emplace(
                    EdgeKey{bundle.predecessor, bundle.successor},
                    bundle)
                    .second) {
            throw SSAError("SSA de-SSA copy plan contains a duplicate or stale edge");
        }
    }

    std::vector<std::vector<SSAMove>> entryCopies(cfg.blocks.size());
    std::vector<std::vector<SSAMove>> exitCopies(cfg.blocks.size());
    std::vector<std::optional<SSAEdgeCopyBundle>> fallthroughSplits(cfg.blocks.size());
    std::vector<SSAEdgeCopyBundle> branchSplits;
    for (const auto& item : bundles) {
        const SSAEdgeCopyBundle& bundle = item.second;
        if (bundle.requiresCriticalEdgeSplit) {
            if (bundle.successor == fallthroughSuccessor(cfg, bundle.predecessor)) {
                if (fallthroughSplits[bundle.predecessor]) {
                    throw SSAError("SSA de-SSA predecessor has duplicate fallthrough splits");
                }
                fallthroughSplits[bundle.predecessor] = bundle;
            } else {
                branchSplits.push_back(bundle);
            }
            continue;
        }

        if (cfg.blocks[bundle.successor].predecessors.size() == 1) {
            entryCopies[bundle.successor].insert(
                entryCopies[bundle.successor].end(),
                bundle.moves.begin(),
                bundle.moves.end());
        } else if (cfg.blocks[bundle.predecessor].successors.size() == 1) {
            exitCopies[bundle.predecessor].insert(
                exitCopies[bundle.predecessor].end(),
                bundle.moves.begin(),
                bundle.moves.end());
        } else {
            throw SSAError("SSA de-SSA copy edge is neither splittable nor safely placeable");
        }
    }
    std::sort(
        branchSplits.begin(),
        branchSplits.end(),
        [](const SSAEdgeCopyBundle& left, const SSAEdgeCopyBundle& right) {
            return std::tie(left.predecessor, left.successor)
                < std::tie(right.predecessor, right.successor);
        });

    struct LayoutBlockSpec {
        std::optional<CFGBlockId> sourceBlock;
        std::optional<SSAEdgeCopyBundle> split;
        bool exitFallthroughBarrier = false;
    };
    std::vector<LayoutBlockSpec> layout;
    layout.reserve(cfg.blocks.size() + branchSplits.size());
    for (CFGBlockId block = 0; block < cfg.exitBlock; ++block) {
        const bool exitFallthroughBarrier = block + 1 == cfg.exitBlock
            && (input.blocks[block].instructions.empty()
                || (input.blocks[block].instructions.back().op != IROp::Jump
                    && input.blocks[block].instructions.back().op != IROp::Return));
        layout.push_back(LayoutBlockSpec{block, std::nullopt, exitFallthroughBarrier});
        if (fallthroughSplits[block]) {
            layout.push_back(LayoutBlockSpec{std::nullopt, fallthroughSplits[block], false});
        }
    }
    for (const SSAEdgeCopyBundle& split : branchSplits) {
        layout.push_back(LayoutBlockSpec{std::nullopt, split, false});
    }
    layout.push_back(LayoutBlockSpec{cfg.exitBlock, std::nullopt, false});

    SSADeSSALinearFunction result;
    result.parameters = input.parameters;
    result.memorySlots = input.memorySlots;
    result.temporaryValues = plan.temporaryValues;
    result.blockEntryOffsets.assign(
        cfg.blocks.size(),
        std::numeric_limits<std::size_t>::max());

    std::map<EdgeKey, std::size_t> splitOutputBlocks;
    std::size_t instructionCount = 0;
    result.blocks.reserve(layout.size());
    for (std::size_t index = 0; index < layout.size(); ++index) {
        const LayoutBlockSpec& spec = layout[index];
        std::size_t count = 0;
        SSADeSSABlock block;
        block.id = index;
        block.sourceBlock = spec.sourceBlock;
        if (spec.split) {
            block.splitEdge = EdgeKey{spec.split->predecessor, spec.split->successor};
            checkedLinearAdvance(count, spec.split->moves.size());
            checkedLinearAdvance(count, 1);
            splitOutputBlocks.emplace(*block.splitEdge, index);
        } else {
            const CFGBlockId source = *spec.sourceBlock;
            block.syntheticExit = source == cfg.exitBlock;
            if (source != cfg.exitBlock) {
                checkedLinearAdvance(count, entryCopies[source].size());
                checkedLinearAdvance(count, input.blocks[source].instructions.size());
                checkedLinearAdvance(count, exitCopies[source].size());
                if (spec.exitFallthroughBarrier) {
                    checkedLinearAdvance(count, 1);
                }
            }
        }
        block.firstInstruction = instructionCount;
        checkedLinearAdvance(instructionCount, count);
        block.endInstruction = instructionCount;
        result.blocks.push_back(block);
        if (spec.sourceBlock) {
            result.blockEntryOffsets[*spec.sourceBlock] = block.firstInstruction;
        }
    }

    result.instructions.reserve(instructionCount);
    result.originalInstructionOffsets.assign(cfg.instructionCount, std::nullopt);
    const std::size_t invalidOffset = std::numeric_limits<std::size_t>::max();
    result.originalInsertionOffsets.assign(cfg.instructionCount + 1, invalidOffset);
    std::set<EdgeKey> requiredBranchSplits;
    for (const SSAEdgeCopyBundle& split : branchSplits) {
        requiredBranchSplits.insert(EdgeKey{split.predecessor, split.successor});
    }
    std::set<EdgeKey> rewrittenBranchSplits;

    const auto emitSyntheticCopy = [
        &result,
        &cfg,
        &sourceSpanForAnchor](const SSAMove& move, std::size_t anchor) {
        if (anchor >= cfg.instructionCount) {
            throw SSAError("SSA de-SSA copy has no source anchor");
        }
        SSAInstruction instruction;
        instruction.op = IROp::Copy;
        instruction.result = move.destination;
        instruction.left = move.source;
        instruction.originalInstruction = anchor;
        instruction.span = sourceSpanForAnchor(anchor);
        result.instructions.push_back(SSADeSSAInstruction{std::move(instruction), true});
    };

    const auto emitOriginal = [
        &result,
        &cfg,
        &splitOutputBlocks,
        &rewrittenBranchSplits](CFGBlockId sourceBlock, const SSAInstruction& source) {
        SSAInstruction lowered = source;
        if (lowered.op == IROp::Jump || isSSAConditionalJump(lowered.op)) {
            const CFGBlockId target = blockForSSAOffset(cfg, lowered.operand);
            const EdgeKey edge{sourceBlock, target};
            const auto split = splitOutputBlocks.find(edge);
            if (split != splitOutputBlocks.end()) {
                lowered.operand = result.blocks[split->second].firstInstruction;
                rewrittenBranchSplits.insert(edge);
            } else {
                lowered.operand = result.blockEntryOffsets[target];
            }
        }
        result.instructions.push_back(SSADeSSAInstruction{std::move(lowered), false});
    };

    const auto recordBoundaries = [
        &result,
        invalidOffset](std::size_t begin, std::size_t end, std::size_t offset) {
        if (begin > end) {
            return;
        }
        for (std::size_t boundary = begin; boundary <= end; ++boundary) {
            if (result.originalInsertionOffsets[boundary] != invalidOffset
                && result.originalInsertionOffsets[boundary] != offset) {
                throw SSAError("SSA de-SSA insertion boundary is assigned twice");
            }
            result.originalInsertionOffsets[boundary] = offset;
        }
    };

    for (std::size_t outputBlockIndex = 0;
         outputBlockIndex < layout.size();
         ++outputBlockIndex) {
        const LayoutBlockSpec& spec = layout[outputBlockIndex];
        const SSADeSSABlock& outputBlock = result.blocks[outputBlockIndex];
        if (spec.split) {
            if (result.instructions.size() != outputBlock.firstInstruction) {
                throw SSAError("SSA de-SSA split block layout offset is stale");
            }
            const std::size_t anchor = cfg.blocks[spec.split->predecessor].endInstruction - 1;
            for (const SSAMove& move : spec.split->moves) {
                emitSyntheticCopy(move, anchor);
            }
            SSAInstruction jump;
            jump.op = IROp::Jump;
            jump.operand = result.blockEntryOffsets[spec.split->successor];
            jump.originalInstruction = anchor;
            jump.span = sourceSpanForAnchor(anchor);
            result.instructions.push_back(SSADeSSAInstruction{std::move(jump), true});
            continue;
        }

        const CFGBlockId sourceBlock = *spec.sourceBlock;
        if (sourceBlock == cfg.exitBlock) {
            continue;
        }
        if (result.instructions.size() != outputBlock.firstInstruction) {
            throw SSAError("SSA de-SSA source block layout offset is stale");
        }
        const CFGBlock& cfgBlock = cfg.blocks[sourceBlock];
        const std::vector<SSAInstruction>& sourceInstructions
            = input.blocks[sourceBlock].instructions;
        if (result.originalInsertionOffsets[cfgBlock.firstInstruction] != invalidOffset) {
            throw SSAError("SSA de-SSA source block insertion boundary is duplicated");
        }
        result.originalInsertionOffsets[cfgBlock.firstInstruction]
            = outputBlock.firstInstruction;

        const std::size_t entryAnchor = cfgBlock.firstInstruction;
        for (const SSAMove& move : entryCopies[sourceBlock]) {
            emitSyntheticCopy(move, entryAnchor);
        }

        std::optional<std::size_t> previousOriginal;
        bool insertedExitCopies = false;
        for (std::size_t index = 0; index < sourceInstructions.size(); ++index) {
            const SSAInstruction& source = sourceInstructions[index];
            if (source.originalInstruction < cfgBlock.firstInstruction
                || source.originalInstruction >= cfgBlock.endInstruction) {
                throw SSAError("SSA de-SSA source instruction is outside its CFG block");
            }
            if (previousOriginal && source.originalInstruction <= *previousOriginal) {
                throw SSAError("SSA de-SSA source instructions are not ordered");
            }
            const std::size_t gapStart = previousOriginal
                ? *previousOriginal + 1
                : cfgBlock.firstInstruction + 1;
            recordBoundaries(gapStart, source.originalInstruction, result.instructions.size());

            const bool insertBeforeTerminator = !exitCopies[sourceBlock].empty()
                && index + 1 == sourceInstructions.size()
                && isSSAControlFlowTerminator(source.op);
            if (insertBeforeTerminator) {
                const std::size_t exitAnchor = cfgBlock.endInstruction - 1;
                for (const SSAMove& move : exitCopies[sourceBlock]) {
                    emitSyntheticCopy(move, exitAnchor);
                }
                insertedExitCopies = true;
            }

            if (result.originalInstructionOffsets[source.originalInstruction]) {
                throw SSAError("SSA de-SSA source instruction is duplicated");
            }
            result.originalInstructionOffsets[source.originalInstruction]
                = result.instructions.size();
            emitOriginal(sourceBlock, source);
            previousOriginal = source.originalInstruction;
        }

        const std::size_t trailingStart = previousOriginal
            ? *previousOriginal + 1
            : cfgBlock.firstInstruction + 1;
        recordBoundaries(
            trailingStart,
            cfgBlock.endInstruction == 0 ? 0 : cfgBlock.endInstruction - 1,
            result.instructions.size());
        if (!insertedExitCopies) {
            const std::size_t exitAnchor = cfgBlock.endInstruction - 1;
            for (const SSAMove& move : exitCopies[sourceBlock]) {
                emitSyntheticCopy(move, exitAnchor);
            }
        }
        if (spec.exitFallthroughBarrier) {
            SSAInstruction jump;
            jump.op = IROp::Jump;
            jump.operand = result.blockEntryOffsets[cfg.exitBlock];
            jump.originalInstruction = cfgBlock.endInstruction - 1;
            jump.span = sourceSpanForAnchor(jump.originalInstruction);
            result.instructions.push_back(SSADeSSAInstruction{std::move(jump), true});
        }
        if (result.instructions.size() != outputBlock.endInstruction) {
            throw SSAError("SSA de-SSA source block instruction count is stale");
        }
    }

    if (result.originalInsertionOffsets.back() != invalidOffset) {
        throw SSAError("SSA de-SSA end insertion boundary is duplicated");
    }
    result.originalInsertionOffsets.back() = result.instructions.size();
    for (const std::size_t offset : result.originalInsertionOffsets) {
        if (offset == invalidOffset) {
            throw SSAError("SSA de-SSA did not map every insertion boundary");
        }
    }
    if (rewrittenBranchSplits != requiredBranchSplits) {
        throw SSAError("SSA de-SSA did not rewrite every critical branch edge");
    }

    result.moduleDependencies.reserve(cfg.dependencyAnchors.size());
    for (const CFGDependencyAnchor& anchor : cfg.dependencyAnchors) {
        if (anchor.dependency.instructionOffset > cfg.instructionCount) {
            throw SSAError("SSA de-SSA dependency offset is out of range");
        }
        IRModuleDependency dependency = anchor.dependency;
        dependency.instructionOffset
            = result.originalInsertionOffsets[dependency.instructionOffset];
        result.moduleDependencies.push_back(std::move(dependency));
    }

    result.verify(cfg);
    return result;
}

void SSADeSSAIRResult::verify() const
{
    if (syntheticInstructions.size() != function.instructions.size()) {
        throw SSAError("SSA de-SSA IR synthetic-instruction map has the wrong size");
    }
    if (originalInstructionOffsets.empty()) {
        if (!originalInsertionOffsets.empty()) {
            throw SSAError("SSA de-SSA IR insertion map has no instruction map");
        }
    } else if (originalInsertionOffsets.size() != originalInstructionOffsets.size() + 1) {
        throw SSAError("SSA de-SSA IR offset maps have inconsistent sizes");
    }
    for (const auto& offset : originalInstructionOffsets) {
        if (offset && *offset >= function.instructions.size()) {
            throw SSAError("SSA de-SSA IR original-instruction offset is out of range");
        }
    }
    for (const std::size_t offset : originalInsertionOffsets) {
        if (offset > function.instructions.size()) {
            throw SSAError("SSA de-SSA IR insertion offset is out of range");
        }
    }
    if (!originalInsertionOffsets.empty()
        && originalInsertionOffsets.back() != function.instructions.size()) {
        throw SSAError("SSA de-SSA IR end insertion offset is stale");
    }
    for (const IRModuleDependency& dependency : moduleDependencies) {
        if (dependency.instructionOffset > function.instructions.size()) {
            throw SSAError("SSA de-SSA IR dependency offset is out of range");
        }
    }

    const auto verifyRegister = [this](const std::optional<IRRegister>& value) {
        if (value && value->index >= function.registerCount) {
            throw SSAError("SSA de-SSA IR register is outside registerCount");
        }
    };
    for (const IRInstruction& instruction : function.instructions) {
        verifyRegister(instruction.dest);
        verifyRegister(instruction.left);
        verifyRegister(instruction.right);
        for (const IRRegister argument : instruction.arguments) {
            if (argument.index >= function.registerCount) {
                throw SSAError("SSA de-SSA IR argument register is outside registerCount");
            }
        }
    }

    std::set<SSAValueId> constantResults;
    for (const IRInstruction& instruction : function.instructions) {
        if (instruction.op == IROp::Constant && instruction.dest) {
            constantResults.insert(instruction.dest->index);
        }
    }
    for (const auto& [value, constant] : foldedConstants) {
        if (constantResults.find(value) == constantResults.end()) {
            throw SSAError(
                "SSA de-SSA folded-constant metadata has no Constant result");
        }
        if (constant.type() == Value::Type::Number
            && !std::isfinite(constant.asNumber())) {
            throw SSAError(
                "SSA de-SSA folded-constant metadata contains a non-finite number");
        }
        if (constant.type() == Value::Type::Function
            || constant.type() == Value::Type::Array
            || constant.type() == Value::Type::Map
            || constant.type() == Value::Type::Range
            || constant.type() == Value::Type::Struct
            || constant.type() == Value::Type::Variant) {
            throw SSAError(
                "SSA de-SSA folded-constant metadata is not primitive");
        }
    }
}

SSADeSSAIRResult lowerSSADeSSAToIR(
    const ControlFlowGraph& cfg,
    const SSADeSSALinearFunction& input,
    std::string name,
    std::vector<std::string> parameters,
    std::vector<IRBinding> bindings)
{
    input.verify(cfg);
    if (parameters.size() != input.parameters.size()) {
        throw SSAError("SSA de-SSA IR parameter names do not match SSA parameters");
    }

    SSADeSSAIRResult result;
    result.function.name = std::move(name);
    result.function.parameters = std::move(parameters);
    result.function.bindings = std::move(bindings);
    result.function.instructions.reserve(input.instructions.size());
    result.syntheticInstructions.reserve(input.instructions.size());

    SSAValueId maximumValue = 0;
    bool hasValue = false;
    const auto observeValue = [&maximumValue, &hasValue](SSAValueId value) {
        if (value == invalidSSAValue) {
            throw SSAError("SSA de-SSA IR uses the reserved invalid value ID");
        }
        if (!hasValue || value > maximumValue) {
            maximumValue = value;
            hasValue = true;
        }
    };
    for (const SSAParameter& parameter : input.parameters) {
        observeValue(parameter.value);
    }

    const auto lowerOptionalRegister = [&observeValue](
        const std::optional<SSAValueId>& value) -> std::optional<IRRegister> {
        if (!value) {
            return std::nullopt;
        }
        observeValue(*value);
        return IRRegister{*value};
    };
    const auto lowerArguments = [&observeValue](const std::vector<SSAValueId>& values) {
        std::vector<IRRegister> result;
        result.reserve(values.size());
        for (const SSAValueId value : values) {
            observeValue(value);
            result.push_back(IRRegister{value});
        }
        return result;
    };

    for (const SSADeSSAInstruction& source : input.instructions) {
        result.function.instructions.push_back(IRInstruction{
            source.instruction.op,
            lowerOptionalRegister(source.instruction.result),
            lowerOptionalRegister(source.instruction.left),
            lowerOptionalRegister(source.instruction.right),
            lowerArguments(source.instruction.arguments),
            source.instruction.operand,
            source.instruction.operands,
            source.instruction.typeNameOperand,
            source.instruction.variantNameOperand,
            source.instruction.span,
            source.instruction.bindingId});
        result.syntheticInstructions.push_back(source.synthetic);
    }

    if (hasValue) {
        if (maximumValue == std::numeric_limits<SSAValueId>::max()) {
            throw SSAError("exhausted SSA de-SSA IR register IDs");
        }
        result.function.registerCount = maximumValue + 1;
    }
    result.moduleDependencies = input.moduleDependencies;
    result.originalInstructionOffsets = input.originalInstructionOffsets;
    result.originalInsertionOffsets = input.originalInsertionOffsets;
    result.verify();
    return result;
}
