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

std::string unqualifiedTypeName(const std::string& name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

template <typename Table>
const typename Table::mapped_type* findTypeIn(
    const Table& table,
    const std::string& name)
{
    const auto exact = table.find(name);
    if (exact != table.end()) {
        return &exact->second;
    }
    const auto unqualified = table.find(unqualifiedTypeName(name));
    if (unqualified != table.end()) {
        return &unqualified->second;
    }
    return nullptr;
}

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

void splitControlFlow(
    std::vector<BytecodeInstruction>& instructions,
    std::unordered_map<std::uint32_t, std::uint32_t>* remap,
    const std::vector<std::size_t>& forcedStarts)
{
    const std::size_t end = instructions.size();
    std::vector<bool> starts(end + 1, false);
    starts[0] = true;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        const BytecodeOp op = instructions[index].op;
        if (op == BytecodeOp::Jump || op == BytecodeOp::JumpIfFalse
            || op == BytecodeOp::JumpIfTrue) {
            const std::size_t target = instructions[index].operand;
            if (target <= end) {
                starts[target] = true;
            }
        }
        if (op == BytecodeOp::Jump || op == BytecodeOp::JumpIfFalse
            || op == BytecodeOp::JumpIfTrue || op == BytecodeOp::Return) {
            starts[index + 1] = true;
        }
    }
    for (std::size_t offset : forcedStarts) {
        if (offset <= end) {
            starts[offset] = true;
        }
    }

    std::unordered_map<std::size_t, std::uint32_t> blockOf;
    std::uint32_t nextBlock = 0;
    for (std::size_t index = 0; index <= end; ++index) {
        if (starts[index]) {
            blockOf.emplace(index, nextBlock++);
        }
    }

    std::vector<BytecodeInstruction> rewritten;
    std::vector<std::optional<std::uint32_t>> rewrittenIr;
    rewritten.reserve(instructions.size() + nextBlock);
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        if (starts[index]) {
            BytecodeInstruction marker{};
            marker.op = BytecodeOp::BlockStart;
            marker.operand = blockOf[index];
            marker.span = instructions[index].span;
            rewritten.push_back(std::move(marker));
            rewrittenIr.push_back(std::nullopt);
        }
        BytecodeInstruction instruction = std::move(instructions[index]);
        if (instruction.op == BytecodeOp::Jump) {
            const std::size_t target = instruction.operand;
            instruction.op = target == end ? BytecodeOp::ReturnNil : BytecodeOp::Br;
            if (target != end) {
                instruction.operand = blockOf[target];
            }
        } else if (instruction.op == BytecodeOp::JumpIfFalse) {
            const std::uint32_t branchTarget = instruction.operand == end
                ? blockOf[end]
                : blockOf[instruction.operand];
            instruction.op = BytecodeOp::BrIf;
            instruction.operand = blockOf[index + 1];
            instruction.operands = {branchTarget};
        } else if (instruction.op == BytecodeOp::JumpIfTrue) {
            const std::uint32_t branchTarget = instruction.operand == end
                ? blockOf[end]
                : blockOf[instruction.operand];
            instruction.op = BytecodeOp::BrIf;
            instruction.operand = branchTarget;
            instruction.operands = {blockOf[index + 1]};
        }
        rewritten.push_back(std::move(instruction));
        rewrittenIr.push_back(checkedU32(index, "instruction offset out of range"));
    }
    if (starts[end]) {
        BytecodeInstruction marker{};
        marker.op = BytecodeOp::BlockStart;
        marker.operand = blockOf[end];
        if (!instructions.empty()) {
            marker.span = instructions.back().span;
        }
        rewritten.push_back(std::move(marker));
        rewrittenIr.push_back(std::nullopt);
        BytecodeInstruction nilReturn{};
        nilReturn.op = BytecodeOp::ReturnNil;
        nilReturn.span = marker.span;
        rewritten.push_back(std::move(nilReturn));
        rewrittenIr.push_back(std::nullopt);
    }
    // 0.2 forbids implicit fallthrough: a block that ends without a
    // terminator gets an explicit branch to the next block (or a nil return
    // when it is the function's last block).
    std::vector<BytecodeInstruction> normalized;
    std::vector<std::optional<std::uint32_t>> normalizedIr;
    normalized.reserve(rewritten.size() + nextBlock);
    for (std::size_t index = 0; index < rewritten.size(); ++index) {
        normalized.push_back(std::move(rewritten[index]));
        normalizedIr.push_back(rewrittenIr[index]);
        const BytecodeOp op = normalized.back().op;
        const bool isTerminator = op == BytecodeOp::Br || op == BytecodeOp::BrIf
            || op == BytecodeOp::Return || op == BytecodeOp::ReturnNil
            || op == BytecodeOp::BlockStart;
        if (isTerminator) {
            continue;
        }
        const bool isLast = index + 1 == rewritten.size();
        const bool nextIsBlock = !isLast && rewritten[index + 1].op == BytecodeOp::BlockStart;
        if (nextIsBlock) {
            BytecodeInstruction branch{};
            branch.op = BytecodeOp::Br;
            branch.operand = rewritten[index + 1].operand;
            branch.span = normalized.back().span;
            normalized.push_back(std::move(branch));
            normalizedIr.push_back(std::nullopt);
        } else if (isLast) {
            BytecodeInstruction nilReturn{};
            nilReturn.op = BytecodeOp::ReturnNil;
            nilReturn.span = normalized.back().span;
            normalized.push_back(std::move(nilReturn));
            normalizedIr.push_back(std::nullopt);
        }
    }
    rewritten = std::move(normalized);
    if (remap) {
        remap->clear();
        for (std::size_t index = 0; index < normalizedIr.size(); ++index) {
            if (normalizedIr[index]) {
                std::size_t position = index;
                const bool forcedStart = std::find(
                    forcedStarts.begin(), forcedStarts.end(), *normalizedIr[index])
                    != forcedStarts.end();
                if (forcedStart && index > 0) {
                    position = index - 1;
                }
                remap->emplace(
                    *normalizedIr[index],
                    checkedU32(position, "instruction offset out of range"));
            }
        }
    }
    instructions = std::move(rewritten);
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
        return BytecodeOp::MakeStruct;
    case IROp::Variant:
        return BytecodeOp::MakeVariant;
    case IROp::VariantTag:
        return BytecodeOp::IsVariant;
    case IROp::VariantField:
        return BytecodeOp::VariantGet;
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
    case IROp::CallDirect:
        return BytecodeOp::CallDirect;
    case IROp::NativeCall:
        return BytecodeOp::CallNative;
    case IROp::Index:
        return BytecodeOp::Index;
    case IROp::AssignIndex:
        return BytecodeOp::AssignIndex;
    case IROp::ArrayGet:
        return BytecodeOp::ArrayGet;
    case IROp::ArraySet:
        return BytecodeOp::ArraySet;
    case IROp::MapGet:
        return BytecodeOp::MapGet;
    case IROp::MapSet:
        return BytecodeOp::MapSet;
    case IROp::RangeGet:
        return BytecodeOp::RangeGet;
    case IROp::Field:
        return BytecodeOp::Field;
    case IROp::AssignField:
        return BytecodeOp::AssignField;
    case IROp::Len:
        return BytecodeOp::Len;
    case IROp::LenArray:
        return BytecodeOp::LenArray;
    case IROp::LenMap:
        return BytecodeOp::LenMap;
    case IROp::LenRange:
        return BytecodeOp::LenRange;
    case IROp::LenStr:
        return BytecodeOp::LenStr;
    case IROp::IterInit:
        return BytecodeOp::IterInit;
    case IROp::IterHas:
        return BytecodeOp::IterHas;
    case IROp::IterNext:
        return BytecodeOp::IterNext;
    case IROp::InitModule:
        return BytecodeOp::InitModule;
    case IROp::AssertNumber:
        return BytecodeOp::AssertNumber;
    case IROp::Return:
        return BytecodeOp::Return;
    case IROp::Negate:
        return BytecodeOp::Negate;
    case IROp::NegNum:
        return BytecodeOp::NegNum;
    case IROp::Not:
        return BytecodeOp::Not;
    case IROp::Add:
        return BytecodeOp::Add;
    case IROp::AddNum:
        return BytecodeOp::AddNum;
    case IROp::Subtract:
        return BytecodeOp::Subtract;
    case IROp::SubNum:
        return BytecodeOp::SubNum;
    case IROp::Multiply:
        return BytecodeOp::Multiply;
    case IROp::MulNum:
        return BytecodeOp::MulNum;
    case IROp::Divide:
        return BytecodeOp::Divide;
    case IROp::DivNum:
        return BytecodeOp::DivNum;
    case IROp::ConcatStr:
        return BytecodeOp::ConcatStr;
    case IROp::Equal:
        return BytecodeOp::Equal;
    case IROp::NotEqual:
        return BytecodeOp::NotEqual;
    case IROp::Greater:
        return BytecodeOp::Greater;
    case IROp::GreaterNum:
        return BytecodeOp::GreaterNum;
    case IROp::GreaterStr:
        return BytecodeOp::GreaterStr;
    case IROp::GreaterEqual:
        return BytecodeOp::GreaterEqual;
    case IROp::GreaterEqualNum:
        return BytecodeOp::GreaterEqualNum;
    case IROp::GreaterEqualStr:
        return BytecodeOp::GreaterEqualStr;
    case IROp::Less:
        return BytecodeOp::Less;
    case IROp::LessNum:
        return BytecodeOp::LessNum;
    case IROp::LessStr:
        return BytecodeOp::LessStr;
    case IROp::LessEqual:
        return BytecodeOp::LessEqual;
    case IROp::LessEqualNum:
        return BytecodeOp::LessEqualNum;
    case IROp::LessEqualStr:
        return BytecodeOp::LessEqualStr;
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
    std::unordered_set<BindingId, SnapshotIdHash<BindingIdTag>> moduleBindingIds;
    for (const IRBinding& binding : ir.bindings()) {
        bindingNames.emplace(binding.bindingId, binding.resolvedName);
        if (binding.storage == BindingStorageClass::Module
            || binding.storage == BindingStorageClass::Exported) {
            moduleBindingIds.insert(binding.bindingId);
        }
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
                if (moduleBindingIds.find(*instruction.bindingId)
                    != moduleBindingIds.end()) {
                    continue;
                }
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

    TypeTables types;
    for (const IRStructLayout& layout : ir.structLayouts()) {
        BytecodeType type{};
        type.isEnum = false;
        type.name = layout.name;
        type.fieldCount = checkedU32(layout.fieldNames.size(), "struct field count out of range");
        type.fieldNames = layout.fieldNames;
        const std::uint32_t typeId = checkedU32(types.types.size(), "type count out of range");
        types.structTypeIds.emplace(layout.name, typeId);
        types.structFields.emplace(layout.name, layout.fieldNames);
        types.types.push_back(std::move(type));
    }
    for (const IREnumLayout& layout : ir.enumLayouts()) {
        BytecodeType type{};
        type.isEnum = true;
        type.name = layout.name;
        std::vector<std::pair<std::string, std::uint32_t>> variants;
        for (const IRVariantLayout& variant : layout.variants) {
            type.variants.push_back(BytecodeVariantLayout{
                variant.name,
                checkedU32(variant.payloadCount, "variant payload count out of range"),
            });
            variants.emplace_back(variant.name, checkedU32(variant.payloadCount, "variant payload count out of range"));
        }
        const std::uint32_t typeId = checkedU32(types.types.size(), "type count out of range");
        types.enumTypeIds.emplace(layout.name, typeId);
        types.enumVariants.emplace(layout.name, std::move(variants));
        types.types.push_back(std::move(type));
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
            if (moduleBindingIds.find(*instruction.bindingId)
                    != moduleBindingIds.end()
                && functions[index].moduleInit) {
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
        if (functions[index].moduleInit) {
            continue;
        }
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
                const std::uint32_t slot
                    = checkedU32(plan.upvalueSources.size(), "upvalue slot count out of range");
                plan.upvalues.emplace(bindingId, slot);
                plan.upvalueSources.push_back(BytecodeUpvalue{
                    BytecodeUpvalueSource::Global,
                    *slotFor(globalSlots, bindingId),
                });
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
                source.source = BytecodeUpvalueSource::Local;
                source.index = *local;
            } else if (const auto upvalue = slotFor(parentPlan.upvalues, bindingId)) {
                source.source = BytecodeUpvalueSource::Upvalue;
                source.index = *upvalue;
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

    // Phase 7: native calls address a serialized import table instead of the
    // name table. Print statements lower to a call of the native `print`
    // import. Imports follow first-use order across main and then functions.
    std::vector<BytecodeNativeImport> nativeImports;
    std::unordered_map<std::string, std::uint32_t> nativeImportIds;
    const auto internNative = [&](const std::string& name) {
        const auto existing = nativeImportIds.find(name);
        if (existing != nativeImportIds.end()) {
            return existing->second;
        }
        const std::uint32_t id
            = checkedU32(nativeImports.size(), "native import count out of range");
        nativeImportIds.emplace(name, id);
        nativeImports.push_back(BytecodeNativeImport{name, 1});
        return id;
    };
    const auto scanNativeUses = [&](const std::vector<IRInstruction>& instructions) {
        for (const IRInstruction& instruction : instructions) {
            if (instruction.op == IROp::Print) {
                internNative("print");
            } else if (instruction.op == IROp::NativeCall) {
                if (instruction.operand >= ir.names().size()) {
                    throw BytecodeCompileError(
                        "native call has an out-of-range name operand");
                }
                internNative(ir.names()[instruction.operand]);
            }
        }
    };
    scanNativeUses(ir.instructions());
    for (const IRFunction& function : functions) {
        scanNativeUses(function.instructions);
    }
    const bool mainHasPrint = std::any_of(
        ir.instructions().begin(), ir.instructions().end(),
        [](const IRInstruction& instruction) { return instruction.op == IROp::Print; });
    const std::uint32_t mainPrintScratch
        = checkedU32(ir.registerCount(), "print scratch register out of range");

    BytecodeProgram program;
    program.setSources(ir.sources());
    program.setConstants(ir.constants());
    program.setNames(ir.names());
    program.setRegisterCount(mainHasPrint
            ? checkedU32(ir.registerCount() + 1, "register index out of range")
            : checkedU32(ir.registerCount(), "register index out of range"));
    program.setNativeImports(std::move(nativeImports));
    auto mainInstructions = lowerInstructions(
        ir.instructions(), globalSlots, globalSlotsByName, ir.names(), types,
        nativeImportIds, mainPrintScratch, moduleBindingIds, false, nullptr);
    splitControlFlow(mainInstructions, nullptr, {});
    program.setInstructions(std::move(mainInstructions));
    program.setDependencyRemap({});
    program.setGlobals(std::move(globals));

    std::vector<BytecodeFunction> loweredFunctions;
    loweredFunctions.reserve(functions.size());
    for (std::size_t index = 0; index < functions.size(); ++index) {
        loweredFunctions.push_back(lowerFunction(
            functions[index], globalSlots, globalSlotsByName, ir.names(), types,
            nativeImportIds, moduleBindingIds, functions[index].moduleInit, plans[index]));
    }
    program.setFunctions(std::move(loweredFunctions));
    program.setTypes(std::move(types.types));

    return program;
}

std::vector<BytecodeInstruction> BytecodeCompiler::lowerInstructions(
    const std::vector<IRInstruction>& instructions,
    const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
    const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
    const std::vector<std::string>& names,
    TypeTables& types,
    const std::unordered_map<std::string, std::uint32_t>& nativeImports,
    std::uint32_t printScratch,
    const std::unordered_set<BindingId, SnapshotIdHash<BindingIdTag>>& moduleBindings,
    bool moduleInit,
    const FunctionPlan* plan)
{
    std::vector<BytecodeInstruction> lowered;
    lowered.reserve(instructions.size());
    for (const IRInstruction& instruction : instructions) {
        lowered.push_back(lowerInstruction(
            instruction, globalSlots, globalSlotsByName, names, types,
            nativeImports, printScratch, moduleBindings, moduleInit, plan));
    }
    return lowered;
}

BytecodeInstruction BytecodeCompiler::lowerInstruction(
    const IRInstruction& instruction,
    const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
    const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
    const std::vector<std::string>& names,
    TypeTables& types,
    const std::unordered_map<std::string, std::uint32_t>& nativeImports,
    std::uint32_t printScratch,
    const std::unordered_set<BindingId, SnapshotIdHash<BindingIdTag>>& moduleBindings,
    bool moduleInit,
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

    if (instruction.op == IROp::Print) {
        const auto printImport = nativeImports.find("print");
        if (printImport == nativeImports.end()) {
            throw BytecodeCompileError("missing native import for print");
        }
        if (!instruction.left) {
            throw BytecodeCompileError("print statement is missing a value operand");
        }
        lowered.op = BytecodeOp::CallNative;
        lowered.dest = BytecodeRegister{printScratch};
        lowered.arguments = {*lowered.left};
        lowered.left = std::nullopt;
        lowered.operand = printImport->second;
        return lowered;
    }
    if (instruction.op == IROp::NativeCall) {
        if (instruction.operand >= names.size()) {
            throw BytecodeCompileError("native call has an out-of-range name operand");
        }
        const auto nativeImport = nativeImports.find(names[instruction.operand]);
        if (nativeImport == nativeImports.end()) {
            throw BytecodeCompileError(
                "missing native import for `" + names[instruction.operand] + "`");
        }
        lowered.operand = nativeImport->second;
    }

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
        if (moduleInit) {
            const auto global = slotFor(globalSlots, *instruction.bindingId);
            if (!global) {
                throw BytecodeCompileError(
                    "module binding references an unknown global slot");
            }
            target = VariableTarget{VariableKind::Global, *global};
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
            throw BytecodeCompileError(
                "function variable reference `" + names[instruction.operand]
                    + "` has no local or upvalue slot");
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
        lowered.op = target.kind == VariableKind::Global
            ? BytecodeOp::InitGlobal
            : BytecodeOp::BindLocal;
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

    const auto structTypeId = [&](const IRInstruction& source, const char* role) {
        if (!source.typeNameOperand || *source.typeNameOperand >= names.size()) {
            throw BytecodeCompileError(std::string("struct ") + role + " is missing a type name");
        }
        const std::string& name = names[*source.typeNameOperand];
        const auto* found = findTypeIn(types.structTypeIds, name);
        if (!found) {
            throw BytecodeCompileError("unknown struct type `" + name + "`");
        }
        if (name.find('.') != std::string::npos) {
            types.types[*found].name = name;
        }
        return *found;
    };
    const auto enumTypeId = [&](const IRInstruction& source) {
        if (!source.typeNameOperand || *source.typeNameOperand >= names.size()) {
            throw BytecodeCompileError("enum instruction is missing an enum name");
        }
        const std::string& name = names[*source.typeNameOperand];
        const auto* found = findTypeIn(types.enumTypeIds, name);
        if (!found) {
            throw BytecodeCompileError("unknown enum type `" + name + "`");
        }
        if (name.find('.') != std::string::npos) {
            types.types[*found].name = name;
        }
        return *found;
    };

    if (instruction.op == IROp::Struct) {
        lowered.op = BytecodeOp::MakeStruct;
        lowered.operand = structTypeId(instruction, "constructor");
        lowered.typeNameOperand = std::nullopt;
        const std::string& typeName = names[*instruction.typeNameOperand];
        const auto* layout = findTypeIn(types.structFields, typeName);
        if (!layout) {
            throw BytecodeCompileError("missing field layout for struct `" + typeName + "`");
        }
        // instruction.operands = literal field name indexes; instruction.arguments = values
        std::vector<BytecodeRegister> ordered;
        for (const std::string& canonical : *layout) {
            std::size_t literal = 0;
            for (; literal < instruction.operands.size(); ++literal) {
                if (names[instruction.operands[literal]] == canonical) {
                    break;
                }
            }
            if (literal >= instruction.operands.size() || literal >= instruction.arguments.size()) {
                throw BytecodeCompileError("struct constructor is missing field `" + canonical + "`");
            }
            ordered.push_back(lowered.arguments[literal]);
        }
        lowered.arguments = std::move(ordered);
        lowered.operands.clear();
    } else if (instruction.op == IROp::Field || instruction.op == IROp::AssignField) {
        const bool assign = instruction.op == IROp::AssignField;
        if (!instruction.typeNameOperand) {
            // Dynamic receivers keep the legacy name-driven field op; the VM
            // performs the struct check at runtime.
            lowered.op = assign ? BytecodeOp::AssignField : BytecodeOp::Field;
            lowered.operand = instruction.operand;
            return lowered;
        }
        lowered.op = assign ? BytecodeOp::StructSet : BytecodeOp::StructGet;
        const std::uint32_t typeId = structTypeId(instruction, assign ? "assignment" : "access");
        lowered.typeNameOperand = std::nullopt;
        const std::string& typeName = names[*instruction.typeNameOperand];
        const std::string& fieldName = names[instruction.operand];
        const auto* layout = findTypeIn(types.structFields, typeName);
        if (!layout) {
            throw BytecodeCompileError("missing field layout for struct `" + typeName + "`");
        }
        const auto slot = std::find(layout->begin(), layout->end(), fieldName);
        if (slot == layout->end()) {
            throw BytecodeCompileError("unknown field `" + fieldName + "` for struct `" + typeName + "`");
        }
        lowered.operand = typeId;
        lowered.operands = {checkedU32(
            static_cast<std::size_t>(std::distance(layout->begin(), slot)),
            "field slot out of range")};
    } else if (instruction.op == IROp::Variant) {
        lowered.op = BytecodeOp::MakeVariant;
        lowered.operand = enumTypeId(instruction);
        lowered.typeNameOperand = std::nullopt;
        const std::string& enumName = names[*instruction.typeNameOperand];
        const std::string& variantName = names[*instruction.variantNameOperand];
        const auto* layout = findTypeIn(types.enumVariants, enumName);
        if (!layout) {
            throw BytecodeCompileError("missing variant layout for enum `" + enumName + "`");
        }
        const auto slot = std::find_if(layout->begin(), layout->end(),
            [&](const std::pair<std::string, std::uint32_t>& variant) {
                return variant.first == variantName;
            });
        if (slot == layout->end()) {
            throw BytecodeCompileError("unknown variant `" + variantName + "` for enum `" + enumName + "`");
        }
        lowered.variantNameOperand = checkedU32(
            static_cast<std::size_t>(std::distance(layout->begin(), slot)),
            "variant id out of range");
    } else if (instruction.op == IROp::VariantTag) {
        lowered.op = BytecodeOp::IsVariant;
        lowered.operand = enumTypeId(instruction);
        lowered.typeNameOperand = std::nullopt;
        const std::string& enumName = names[*instruction.typeNameOperand];
        const std::string& variantName = names[*instruction.variantNameOperand];
        const auto* layout = findTypeIn(types.enumVariants, enumName);
        if (!layout) {
            throw BytecodeCompileError("missing variant layout for enum `" + enumName + "`");
        }
        const auto slot = std::find_if(layout->begin(), layout->end(),
            [&](const std::pair<std::string, std::uint32_t>& variant) {
                return variant.first == variantName;
            });
        if (slot == layout->end()) {
            throw BytecodeCompileError("unknown variant `" + variantName + "` for enum `" + enumName + "`");
        }
        lowered.variantNameOperand = checkedU32(
            static_cast<std::size_t>(std::distance(layout->begin(), slot)),
            "variant id out of range");
    } else if (instruction.op == IROp::VariantField) {
        lowered.op = BytecodeOp::VariantGet;
        lowered.operand = instruction.operand; // payload index
        lowered.typeNameOperand = lowerOperand(instruction.typeNameOperand);
        lowered.variantNameOperand = lowerOperand(instruction.variantNameOperand);
        // Rewrite the enum name to its TypeId and variant name to its VariantId.
        if (!instruction.typeNameOperand || !instruction.variantNameOperand) {
            throw BytecodeCompileError("variant_field is missing enum or variant metadata");
        }
        const std::string& enumName = names[*instruction.typeNameOperand];
        const std::string& variantName = names[*instruction.variantNameOperand];
        const std::uint32_t* typeFound = findTypeIn(types.enumTypeIds, enumName);
        if (!typeFound) {
            throw BytecodeCompileError("unknown enum type `" + enumName + "`");
        }
        const auto* layout = findTypeIn(types.enumVariants, enumName);
        if (!layout) {
            throw BytecodeCompileError("missing variant layout for enum `" + enumName + "`");
        }
        const auto slot = std::find_if(layout->begin(), layout->end(),
            [&](const std::pair<std::string, std::uint32_t>& variant) {
                return variant.first == variantName;
            });
        if (slot == layout->end()) {
            throw BytecodeCompileError("unknown variant `" + variantName + "` for enum `" + enumName + "`");
        }
        lowered.typeNameOperand = *typeFound;
        lowered.variantNameOperand = checkedU32(
            static_cast<std::size_t>(std::distance(layout->begin(), slot)),
            "variant id out of range");
    }

    return lowered;
}

BytecodeFunction BytecodeCompiler::lowerFunction(
    const IRFunction& function,
    const std::unordered_map<BindingId, std::uint32_t, SnapshotIdHash<BindingIdTag>>& globalSlots,
    const std::unordered_map<std::string, std::uint32_t>& globalSlotsByName,
    const std::vector<std::string>& names,
    TypeTables& types,
    const std::unordered_map<std::string, std::uint32_t>& nativeImports,
    const std::unordered_set<BindingId, SnapshotIdHash<BindingIdTag>>& moduleBindings,
    bool moduleInit,
    const FunctionPlan& plan)
{
    const bool hasPrint = std::any_of(
        function.instructions.begin(), function.instructions.end(),
        [](const IRInstruction& instruction) { return instruction.op == IROp::Print; });
    const std::uint32_t printScratch
        = checkedU32(function.registerCount, "print scratch register out of range");
    auto instructions = lowerInstructions(
        function.instructions, globalSlots, globalSlotsByName, names, types,
        nativeImports, printScratch, moduleBindings, moduleInit, &plan);
    splitControlFlow(instructions, nullptr, {});
    BytecodeFunction lowered{
        function.name,
        function.parameters,
        std::move(instructions),
        hasPrint
            ? checkedU32(function.registerCount + 1, "register index out of range")
            : checkedU32(function.registerCount, "register index out of range"),
        plan.localCount,
        plan.upvalueSources,
    };
    return lowered;
}
