#pragma once

#include "BindingMetadata.hpp"
#include "ModuleGraph.hpp"
#include "SourceMap.hpp"
#include "Value.hpp"

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

struct IRRegister {
    std::size_t index = 0;
};

enum class IROp {
    Constant,
    MakeFunction,
    Array,
    Map,
    Struct,
    Variant,
    VariantTag,
    VariantField,
    Copy,
    LoadVar,
    StoreVar,
    AssignVar,
    Call,
    CallDirect,
    NativeCall,
    Index,
    AssignIndex,
    ArrayGet,
    ArraySet,
    MapGet,
    MapSet,
    RangeGet,
    LenArray,
    LenMap,
    LenRange,
    LenStr,
    Field,
    AssignField,
    Len,
    IterInit,
    IterHas,
    IterNext,
    InitModule,
    AssertNumber,
    Print,
    Return,
    Negate,
    Not,
    Add,
    Subtract,
    Multiply,
    Divide,
    Equal,
    NotEqual,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
    AddNum,
    SubNum,
    MulNum,
    DivNum,
    NegNum,
    ConcatStr,
    LessNum,
    LessEqualNum,
    GreaterNum,
    GreaterEqualNum,
    LessStr,
    LessEqualStr,
    GreaterStr,
    GreaterEqualStr,
    Jump,
    JumpIfFalse,
    JumpIfTrue,
};

struct IRInstruction {
    IROp op;
    std::optional<IRRegister> dest;
    std::optional<IRRegister> left;
    std::optional<IRRegister> right;
    std::vector<IRRegister> arguments;
    std::size_t operand = 0;
    std::vector<std::size_t> operands{};
    std::optional<std::size_t> typeNameOperand = std::nullopt;
    std::optional<std::size_t> variantNameOperand = std::nullopt;
    std::optional<SourceSpan> span = std::nullopt;
    std::optional<BindingId> bindingId = std::nullopt;
};

struct IREffectSummary {
    bool readsMemory = false;
    bool writesMemory = false;
    bool mayTrap = false;
    bool allocates = false;
    bool calls = false;
    bool observable = false;
    bool controlFlow = false;

    bool isPure() const;
};

IREffectSummary irEffectSummary(IROp op);

struct IRFunction {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<IRInstruction> instructions;
    std::size_t registerCount = 0;
    std::vector<IRBinding> bindings;
    std::vector<BindingId> parameterBindingIds;
    std::size_t id = 0;
    std::size_t parentId = 0;
    bool moduleInit = false;
};

// A dependency marker emitted while lowering one module independently.  The
// marker is not an instruction: it records the local main-instruction offset
// at which a linker must expand the referenced module product.
struct IRModuleDependency {
    std::size_t importedModuleId = 0;
    ModuleGraphEdgeKind kind = ModuleGraphEdgeKind::Import;
    std::string requestedPath;
    // Retained for CFG dependency-anchor compatibility. Module init lowering
    // no longer splices dependency streams, so this offset stays zero.
    std::size_t instructionOffset = 0;
};

struct IRVariantLayout {
    std::string name;
    std::size_t payloadCount = 0;
};

struct IRStructLayout {
    std::string name;
    std::vector<std::string> fieldNames;
};

struct IREnumLayout {
    std::string name;
    std::vector<IRVariantLayout> variants;
};

class IRProgram {
public:
    void setSources(std::vector<SourceFile> sources);
    const std::vector<SourceFile>& sources() const;
    void setCurrentSpan(std::optional<SourceSpan> span);

    std::size_t addConstant(Value value);
    std::size_t addName(std::string name);
    IRRegister makeRegister();
    void beginFunction(
        std::string name,
        std::vector<std::string> parameters,
        bool moduleInit = false);
    void setFunctionParameterBindingIds(std::vector<BindingId> bindingIds);
    std::size_t endFunction();

    void addModuleDependency(IRModuleDependency dependency);
    void addStructLayout(IRStructLayout layout);
    void addEnumLayout(IREnumLayout layout);
    // Register one canonical snapshot binding.  The program table owns each
    // BindingId exactly once; function tables contain only visibility
    // references added through addFunctionBinding().
    void addBinding(IRBinding binding);
    void addFunctionBinding(IRBinding binding);

    IRRegister emitConstant(Value value);
    IRRegister emitMakeFunction(std::size_t functionIndex);
    IRRegister emitArray(std::vector<IRRegister> elements);
    IRRegister emitMap(std::vector<IRRegister> keyValueRegisters);
    IRRegister emitStruct(
        std::vector<std::size_t> fieldNames,
        std::vector<IRRegister> fieldValues,
        std::optional<std::size_t> typeNameOperand = std::nullopt);
    IRRegister emitVariant(
        std::string enumName,
        std::string variantName,
        std::vector<IRRegister> payload);
    IRRegister emitVariantTag(IRRegister value, std::string enumName, std::string variantName);
    IRRegister emitVariantField(
        IRRegister value,
        std::size_t index,
        std::string enumName,
        std::string variantName);
    IRRegister emitCopy(IRRegister value);
    void emitCopyTo(IRRegister dest, IRRegister value);
    IRRegister emitLoadVar(
        std::string name,
        std::optional<BindingId> bindingId = std::nullopt);
    void emitStoreVar(
        std::string name,
        IRRegister value,
        std::optional<BindingId> bindingId = std::nullopt);
    void emitAssignVar(
        std::string name,
        IRRegister value,
        std::optional<BindingId> bindingId = std::nullopt);
    IRRegister emitCall(IRRegister callee, std::vector<IRRegister> arguments);
    IRRegister emitCallDirect(std::size_t functionIndex, std::vector<IRRegister> arguments);
    IRRegister emitNativeCall(std::string name, std::vector<IRRegister> arguments);
    IRRegister emitIndex(IRRegister collection, IRRegister index);
    IRRegister emitAssignIndex(IRRegister collection, IRRegister index, IRRegister value);
    IRRegister emitArrayGet(IRRegister collection, IRRegister index);
    IRRegister emitArraySet(IRRegister collection, IRRegister index, IRRegister value);
    IRRegister emitMapGet(IRRegister collection, IRRegister index);
    IRRegister emitMapSet(IRRegister collection, IRRegister index, IRRegister value);
    IRRegister emitRangeGet(IRRegister collection, IRRegister index);
    IRRegister emitField(
        IRRegister object,
        std::string fieldName,
        std::optional<std::string> structTypeName);
    IRRegister emitAssignField(
        IRRegister object,
        std::string fieldName,
        std::optional<std::string> structTypeName,
        IRRegister value);
    IRRegister emitLen(IRRegister value);
    IRRegister emitLenArray(IRRegister value);
    IRRegister emitLenMap(IRRegister value);
    IRRegister emitLenRange(IRRegister value);
    IRRegister emitLenStr(IRRegister value);
    IRRegister emitIterInit(IRRegister collection);
    IRRegister emitIterHas(IRRegister iterator);
    IRRegister emitIterNext(IRRegister iterator);
    void emitInitModule(std::size_t dependencyIndex);
    IRRegister emitAssertNumber(IRRegister value, std::string message);
    void emitPrint(IRRegister value);
    void emitReturn(IRRegister value);
    IRRegister emitUnary(IROp op, IRRegister value);
    IRRegister emitBinary(IROp op, IRRegister left, IRRegister right);
    std::size_t emitJump();
    void emitJumpTo(std::size_t target);
    std::size_t emitJumpIfFalse(IRRegister condition);
    std::size_t emitJumpIfTrue(IRRegister condition);
    void patchJump(std::size_t jumpInstruction);
    std::size_t instructionCount() const;
    std::size_t functionCount() const;
    std::size_t activeFunctionId() const;
    void patchMainCallDirect(std::size_t instructionIndex, std::size_t functionIndex);
    void patchFunctionCallDirectById(
        std::size_t functionId,
        std::size_t instructionIndex,
        std::size_t targetFunctionIndex);

    // Rebuild a completed program with replacement main/function streams.
    // Program-level compiler tables (constants, names, sources, and the
    // canonical binding table) are copied unchanged.  The replacement is
    // validated before it is returned so an internal lowering result cannot
    // be connected to a later pipeline boundary in a partially valid state.
    IRProgram rebuildWithStreams(
        std::vector<IRInstruction> instructions,
        std::size_t registerCount,
        std::vector<IRFunction> functions,
        std::vector<IRModuleDependency> moduleDependencies) const;

    const std::vector<Value>& constants() const;
    const std::vector<std::string>& names() const;
    const std::vector<IRInstruction>& instructions() const;
    const std::vector<IRFunction>& functions() const;
    const std::vector<IRModuleDependency>& moduleDependencies() const;
    const std::vector<IRStructLayout>& structLayouts() const;
    const std::vector<IREnumLayout>& enumLayouts() const;
    const std::vector<IRBinding>& bindings() const;
    std::size_t registerCount() const;

    // Print a compact, assembly-like view of the generated register IR.
    void print(std::ostream& out) const;

private:
    void emit(IRInstruction instruction);

    std::vector<Value> constants_;
    std::vector<std::string> names_;
    std::vector<IRInstruction> instructions_;
    std::size_t registerCount_ = 0;
    std::vector<IRFunction> functionStack_;
    std::vector<IRFunction> functions_;
    std::size_t nextFunctionId_ = 0;
    std::vector<IRModuleDependency> moduleDependencies_;
    std::vector<IRStructLayout> structLayouts_;
    std::vector<IREnumLayout> enumLayouts_;
    std::vector<IRBinding> bindings_;
    std::vector<SourceFile> sources_;
    std::optional<SourceSpan> currentSpan_;
};

std::string irOpName(IROp op);
std::ostream& operator<<(std::ostream& out, IRRegister reg);
