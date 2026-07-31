#include "SSA.hpp"

#include <algorithm>
#include <functional>
#include <limits>
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
