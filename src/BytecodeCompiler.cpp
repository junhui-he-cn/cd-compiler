#include "BytecodeCompiler.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

constexpr std::size_t NO_PARENT = std::numeric_limits<std::size_t>::max();

std::uint32_t checkedU32(std::size_t value, const char* message)
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw BytecodeCompileError(message);
    }
    return static_cast<std::uint32_t>(value);
}

BytecodeRegister lowerRegister(IRRegister reg)
{
    return BytecodeRegister{checkedU32(reg.index, "register index out of range")};
}

std::optional<BytecodeRegister> lowerRegister(std::optional<IRRegister> reg)
{
    if (!reg) {
        return std::nullopt;
    }
    return lowerRegister(*reg);
}

std::vector<BytecodeRegister> lowerRegisters(const std::vector<IRRegister>& registers)
{
    std::vector<BytecodeRegister> lowered;
    lowered.reserve(registers.size());
    for (IRRegister reg : registers) {
        lowered.push_back(lowerRegister(reg));
    }
    return lowered;
}

std::vector<std::uint32_t> lowerOperands(const std::vector<std::size_t>& operands)
{
    std::vector<std::uint32_t> lowered;
    lowered.reserve(operands.size());
    for (std::size_t operand : operands) {
        lowered.push_back(checkedU32(operand, "operand out of range"));
    }
    return lowered;
}

std::optional<std::uint32_t> lowerOperand(std::optional<std::size_t> operand)
{
    if (!operand) {
        return std::nullopt;
    }
    return checkedU32(*operand, "operand out of range");
}

enum class VariableKind {
    Local,
    Upvalue,
    Global,
};

struct VariableTarget {
    VariableKind kind = VariableKind::Local;
    std::uint32_t slot = 0;
};

std::optional<std::uint32_t> slotFor(
    const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& slots,
    BindingId bindingId)
{
    const auto found = slots.find(bindingId);
    if (found == slots.end()) {
        return std::nullopt;
    }
    return found->second;
}

BytecodeOp lowerOp(IROp op)
{
    switch (op) {
    case IROp::Constant:
        return BytecodeOp::Constant;
    case IROp::MakeFunction:
        return BytecodeOp::MakeFunction;
    case IROp::Array:
        return BytecodeOp::Array;
    case IROp::Map:
        return BytecodeOp::Map;
    case IROp::Struct:
        return BytecodeOp::Struct;
    case IROp::Variant:
        return BytecodeOp::Variant;
    case IROp::VariantTag:
        return BytecodeOp::VariantTag;
    case IROp::VariantField:
        return BytecodeOp::VariantField;
    case IROp::Copy:
        return BytecodeOp::Move;
    case IROp::LoadVar:
        return BytecodeOp::LoadVar;
    case IROp::StoreVar:
        return BytecodeOp::StoreVar;
    case IROp::AssignVar:
        return BytecodeOp::AssignVar;
    case IROp::Call:
        return BytecodeOp::Call;
    case IROp::NativeCall:
        return BytecodeOp::NativeCall;
    case IROp::Index:
        return BytecodeOp::Index;
    case IROp::AssignIndex:
        return BytecodeOp::AssignIndex;
    case IROp::Field:
        return BytecodeOp::Field;
    case IROp::AssignField:
        return BytecodeOp::AssignField;
    case IROp::Len:
        return BytecodeOp::Len;
    case IROp::AssertArray:
        return BytecodeOp::AssertArray;
    case IROp::AssertNumber:
        return BytecodeOp::AssertNumber;
    case IROp::Print:
        return BytecodeOp::Print;
    case IROp::Return:
        return BytecodeOp::Return;
    case IROp::Negate:
        return BytecodeOp::Negate;
    case IROp::Not:
        return BytecodeOp::Not;
    case IROp::Add:
        return BytecodeOp::Add;
    case IROp::Subtract:
        return BytecodeOp::Subtract;
    case IROp::Multiply:
        return BytecodeOp::Multiply;
    case IROp::Divide:
        return BytecodeOp::Divide;
    case IROp::Equal:
        return BytecodeOp::Equal;
    case IROp::NotEqual:
        return BytecodeOp::NotEqual;
    case IROp::Greater:
        return BytecodeOp::Greater;
    case IROp::GreaterEqual:
        return BytecodeOp::GreaterEqual;
    case IROp::Less:
        return BytecodeOp::Less;
    case IROp::LessEqual:
        return BytecodeOp::LessEqual;
    case IROp::Jump:
        return BytecodeOp::Jump;
    case IROp::JumpIfFalse:
        return BytecodeOp::JumpIfFalse;
    case IROp::JumpIfTrue:
        return BytecodeOp::JumpIfTrue;
    }

    return BytecodeOp::Constant;
}

} // namespace

BytecodeCompileError::BytecodeCompileError(std::string message)
    : DiagnosticError(DiagnosticKind::Compile, std::move(message))
{
}

BytecodeProgram BytecodeCompiler::compile(const IRProgram& ir)
{
    const auto& functions = ir.functions();

    std::unordered_map<BindingId, std::string, SnapshotIdHash<BindingIdTag>> bindingNames;
    for (const IRBinding& binding : ir.bindings()) {
        bindingNames.emplace(binding.bindingId, binding.resolvedName);
    }

    // Imported bindings copy the exporter's resolvedName, so global slots are
    // deduplicated by resolvedName to preserve cross-module identity in a
    // single linked program.
    std::unordered_map<std::string, std::uint32_t> globalSlotsByName;
    std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>> globalSlots;
    const auto assignGlobalSlot = [&](BindingId bindingId) {
        if (globalSlots.find(bindingId) != globalSlots.end()) {
            return;
        }
        const auto name = bindingNames.find(bindingId);
        if (name == bindingNames.end()) {
            throw BytecodeCompileError("variable binding has no canonical name");
        }
        const auto existing = globalSlotsByName.find(name->second);
        const std::uint32_t slot = existing == globalSlotsByName.end()
            ? checkedU32(globalSlotsByName.size(), "global slot count out of range")
            : existing->second;
        if (existing == globalSlotsByName.end()) {
            globalSlotsByName.emplace(name->second, slot);
        }
        globalSlots.emplace(bindingId, slot);
    };
    for (const IRInstruction& instruction : ir.instructions()) {
        if (instruction.op == IROp::StoreVar && instruction.bindingId) {
            assignGlobalSlot(*instruction.bindingId);
        }
    }
    // Imported and re-exported module bindings never emit StoreVar in this
    // module. Any canonical binding not declared by main or by a function is
    // such an imported module-level binding; assign its global slot after the
    // declared bindings.
    std::unordered_set<BindingId, SnapshotIdHash<BindingIdTag>> functionDeclared;
    for (const IRFunction& function : functions) {
        for (BindingId parameterId : function.parameterBindingIds) {
            functionDeclared.insert(parameterId);
        }
        for (const IRInstruction& instruction : function.instructions) {
            if (instruction.op == IROp::StoreVar && instruction.bindingId) {
                functionDeclared.insert(*instruction.bindingId);
            }
        }
    }
    for (const IRBinding& binding : ir.bindings()) {
        if (functionDeclared.find(binding.bindingId) == functionDeclared.end()
            && globalSlots.find(binding.bindingId) == globalSlots.end()) {
            assignGlobalSlot(binding.bindingId);
        }
    }
    // Namespace-qualified module references lower without binding metadata.
    // Give their resolved names global slots too, so module products can
    // resolve them through the linker's name-deduplicated global table.
    const auto assignAnonymousGlobal = [&](const IRInstruction& instruction) {
        if ((instruction.op != IROp::LoadVar && instruction.op != IROp::AssignVar)
            || instruction.bindingId) {
            return;
        }
        if (instruction.operand >= ir.names().size()) {
            throw BytecodeCompileError("variable instruction has an out-of-range name operand");
        }
        const std::string& name = ir.names()[instruction.operand];
        if (globalSlotsByName.find(name) == globalSlotsByName.end()) {
            globalSlotsByName.emplace(
                name,
                checkedU32(globalSlotsByName.size(), "global slot count out of range"));
        }
    };
    for (const IRInstruction& instruction : ir.instructions()) {
        assignAnonymousGlobal(instruction);
    }
    for (const IRFunction& function : functions) {
        for (const IRInstruction& instruction : function.instructions) {
            assignAnonymousGlobal(instruction);
        }
    }

    std::vector<std::uint32_t> globals(globalSlotsByName.size(), 0);
    for (const auto& [name, slot] : globalSlotsByName) {
        const auto nameIndex = std::find(ir.names().begin(), ir.names().end(), name);
        if (nameIndex == ir.names().end()) {
            throw BytecodeCompileError("global binding name is missing from the name table");
        }
        globals[slot] = checkedU32(
            static_cast<std::size_t>(std::distance(ir.names().begin(), nameIndex)),
            "name index out of range");
    }

    std::vector<FunctionPlan> plans(functions.size());
    for (std::size_t index = 0; index < functions.size(); ++index) {
        FunctionPlan& plan = plans[index];
        for (BindingId parameterId : functions[index].parameterBindingIds) {
            plan.locals.emplace(
                parameterId,
                checkedU32(plan.locals.size(), "local slot count out of range"));
        }
        for (const IRInstruction& instruction : functions[index].instructions) {
            if (instruction.op != IROp::StoreVar || !instruction.bindingId) {
                continue;
            }
            const auto found = plan.locals.find(*instruction.bindingId);
            if (found == plan.locals.end()) {
                plan.locals.emplace(
                    *instruction.bindingId,
                    checkedU32(plan.locals.size(), "local slot count out of range"));
            }
        }
        plan.localCount = checkedU32(plan.locals.size(), "local slot count out of range");
    }

    std::unordered_map<std::size_t, std::size_t> functionIdToIndex;
    for (std::size_t index = 0; index < functions.size(); ++index) {
        functionIdToIndex.emplace(functions[index].id, index);
    }

    // Parents finish after their children, so their final indexes are larger.
    // Walking in reverse index order resolves parent upvalue slots first.
    for (std::size_t reverse = 0; reverse < functions.size(); ++reverse) {
        const std::size_t index = functions.size() - 1 - reverse;
        FunctionPlan& plan = plans[index];
        for (const IRInstruction& instruction : functions[index].instructions) {
            if ((instruction.op != IROp::LoadVar
                    && instruction.op != IROp::AssignVar)
                || !instruction.bindingId) {
                continue;
            }
            BindingId bindingId = *instruction.bindingId;
            if (slotFor(plan.locals, bindingId)) {
                continue;
            }
            if (slotFor(globalSlots, bindingId)) {
                continue;
            }
            if (plan.upvalues.find(bindingId) != plan.upvalues.end()) {
                continue;
            }

            const std::size_t parentId = functions[index].parentId;
            const auto parentFound = functionIdToIndex.find(parentId);
            if (parentId == NO_PARENT || parentFound == functionIdToIndex.end()) {
                throw BytecodeCompileError(
                    "captured binding `" + ir.names()[instruction.operand]
                    + "` has no enclosing function slot");
            }
            const FunctionPlan& parentPlan = plans[parentFound->second];
            BytecodeUpvalue source{};
            if (const auto local = slotFor(parentPlan.locals, bindingId)) {
                source.sourceIsLocal = true;
                source.source = *local;
            } else if (const auto upvalue = slotFor(parentPlan.upvalues, bindingId)) {
                source.sourceIsLocal = false;
                source.source = *upvalue;
            } else {
                throw BytecodeCompileError(
                    "captured binding `" + ir.names()[instruction.operand]
                    + "` is not visible in its parent function");
            }
            const std::uint32_t slot
                = checkedU32(plan.upvalueSources.size(), "upvalue slot count out of range");
            plan.upvalues.emplace(bindingId, slot);
            plan.upvalueSources.push_back(source);
        }
    }

    BytecodeProgram program;
    program.setSources(ir.sources());
    program.setConstants(ir.constants());
    program.setNames(ir.names());
    program.setRegisterCount(checkedU32(ir.registerCount(), "register index out of range"));
    program.setInstructions(lowerInstructions(
        ir.instructions(), globalSlots, globalSlotsByName, ir.names(), nullptr));
    program.setGlobals(std::move(globals));

    std::vector<BytecodeFunction> loweredFunctions;
    loweredFunctions.reserve(functions.size());
    for (std::size_t index = 0; index < functions.size(); ++index) {
        loweredFunctions.push_back(lowerFunction(
            functions[index], globalSlots, globalSlotsByName, ir.names(), plans[index]));
    }
    program.setFunctions(std::move(loweredFunctions));

    return program;
}

std::vector<BytecodeInstruction> BytecodeCompiler::lowerInstructions(
    const std::vector<IRInstruction>& instructions,
    const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
    const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
    const std::vector<std::string>& names,
    const FunctionPlan* plan)
{
    std::vector<BytecodeInstruction> lowered;
    lowered.reserve(instructions.size());
    for (const IRInstruction& instruction : instructions) {
        lowered.push_back(lowerInstruction(
            instruction, globalSlots, globalSlotsByName, names, plan));
    }
    return lowered;
}

BytecodeInstruction BytecodeCompiler::lowerInstruction(
    const IRInstruction& instruction,
    const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
    const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
    const std::vector<std::string>& names,
    const FunctionPlan* plan)
{
    BytecodeInstruction lowered{
        lowerOp(instruction.op),
        lowerRegister(instruction.dest),
        lowerRegister(instruction.left),
        lowerRegister(instruction.right),
        lowerRegisters(instruction.arguments),
        checkedU32(instruction.operand, "operand out of range"),
        lowerOperands(instruction.operands),
        lowerOperand(instruction.typeNameOperand),
        lowerOperand(instruction.variantNameOperand),
        instruction.span};

    const auto requireTarget = [&](VariableTarget& target) {
        if (!instruction.bindingId) {
            // Namespace-qualified module references lower without binding
            // metadata; resolve them through the global name table.
            if (instruction.operand >= names.size()) {
                throw BytecodeCompileError(
                    "variable instruction has an out-of-range name operand");
            }
            const auto global = globalSlotsByName.find(names[instruction.operand]);
            if (global == globalSlotsByName.end()) {
                throw BytecodeCompileError(
                    "variable instruction references an unknown global `"
                    + names[instruction.operand] + "`");
            }
            target = VariableTarget{VariableKind::Global, global->second};
            return;
        }
        if (plan) {
            if (const auto local = slotFor(plan->locals, *instruction.bindingId)) {
                target = VariableTarget{VariableKind::Local, *local};
                return;
            }
            if (const auto upvalue = slotFor(plan->upvalues, *instruction.bindingId)) {
                target = VariableTarget{VariableKind::Upvalue, *upvalue};
                return;
            }
        }
        const auto global = slotFor(globalSlots, *instruction.bindingId);
        if (!global) {
            throw BytecodeCompileError(
                "variable instruction references an unknown binding");
        }
        target = VariableTarget{VariableKind::Global, *global};
    };

    if (instruction.op == IROp::LoadVar) {
        VariableTarget target{};
        requireTarget(target);
        switch (target.kind) {
        case VariableKind::Local:
            lowered.op = BytecodeOp::LoadLocal;
            break;
        case VariableKind::Upvalue:
            lowered.op = BytecodeOp::LoadUpvalue;
            break;
        case VariableKind::Global:
            lowered.op = BytecodeOp::LoadGlobal;
            break;
        }
        lowered.operand = target.slot;
    } else if (instruction.op == IROp::StoreVar) {
        VariableTarget target{};
        requireTarget(target);
        lowered.op = plan ? BytecodeOp::BindLocal : BytecodeOp::InitGlobal;
        lowered.operand = target.slot;
    } else if (instruction.op == IROp::AssignVar) {
        VariableTarget target{};
        requireTarget(target);
        switch (target.kind) {
        case VariableKind::Local:
            lowered.op = BytecodeOp::SetLocal;
            break;
        case VariableKind::Upvalue:
            lowered.op = BytecodeOp::SetUpvalue;
            break;
        case VariableKind::Global:
            lowered.op = BytecodeOp::SetGlobal;
            break;
        }
        lowered.operand = target.slot;
    }

    return lowered;
}

BytecodeFunction BytecodeCompiler::lowerFunction(
    const IRFunction& function,
    const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
    const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
    const std::vector<std::string>& names,
    const FunctionPlan& plan)
{
    BytecodeFunction lowered{
        function.name,
        function.parameters,
        lowerInstructions(
            function.instructions, globalSlots, globalSlotsByName, names, &plan),
        checkedU32(function.registerCount, "register index out of range"),
        plan.localCount,
        plan.upvalueSources,
    };
    return lowered;
}
