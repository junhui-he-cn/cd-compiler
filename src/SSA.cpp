#include "SSA.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace {

struct DefinitionSite {
    CFGBlockId block = 0;
};

void defineValue(
    std::unordered_map<SSAValueId, DefinitionSite>& definitions,
    SSAValueId value,
    CFGBlockId block)
{
    const auto inserted = definitions.emplace(value, DefinitionSite{block});
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
            defineValue(definitions, phi.result, block.id);
        }
        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.originalInstruction < block.firstInstruction
                || instruction.originalInstruction >= block.endInstruction) {
                throw SSAError("SSA instruction does not belong to its CFG block");
            }
            if (instruction.result) {
                defineValue(definitions, *instruction.result, block.id);
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
                verifyUse(definitions, incoming.value);
            }
        }

        for (const SSAInstruction& instruction : block.instructions) {
            if (instruction.left) {
                verifyUse(definitions, *instruction.left);
            }
            if (instruction.right) {
                verifyUse(definitions, *instruction.right);
            }
            for (const SSAValueId argument : instruction.arguments) {
                verifyUse(definitions, argument);
            }
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
