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
    NativeCall,
    Index,
    AssignIndex,
    Field,
    AssignField,
    Len,
    AssertArray,
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
};

// A dependency marker emitted while lowering one module independently.  The
// marker is not an instruction: it records the local main-instruction offset
// at which a linker must expand the referenced module product.
struct IRModuleDependency {
    std::size_t importedModuleId = 0;
    ModuleGraphEdgeKind kind = ModuleGraphEdgeKind::Import;
    std::string requestedPath;
    std::size_t instructionOffset = 0;
};

class IRProgram {
public:
    void setSources(std::vector<SourceFile> sources);
    const std::vector<SourceFile>& sources() const;
    void setCurrentSpan(std::optional<SourceSpan> span);

    std::size_t addConstant(Value value);
    std::size_t addName(std::string name);
    IRRegister makeRegister();
    void beginFunction(std::string name, std::vector<std::string> parameters);
    std::size_t endFunction();

    void addModuleDependency(IRModuleDependency dependency);
    void addBinding(IRBinding binding);

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
    IRRegister emitVariantField(IRRegister value, std::size_t index);
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
    IRRegister emitNativeCall(std::string name, std::vector<IRRegister> arguments);
    IRRegister emitIndex(IRRegister collection, IRRegister index);
    IRRegister emitAssignIndex(IRRegister collection, IRRegister index, IRRegister value);
    IRRegister emitField(IRRegister object, std::string fieldName);
    IRRegister emitAssignField(IRRegister object, std::string fieldName, IRRegister value);
    IRRegister emitLen(IRRegister value);
    IRRegister emitAssertArray(IRRegister value);
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

    const std::vector<Value>& constants() const;
    const std::vector<std::string>& names() const;
    const std::vector<IRInstruction>& instructions() const;
    const std::vector<IRFunction>& functions() const;
    const std::vector<IRModuleDependency>& moduleDependencies() const;
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
    std::vector<IRModuleDependency> moduleDependencies_;
    std::vector<IRBinding> bindings_;
    std::vector<SourceFile> sources_;
    std::optional<SourceSpan> currentSpan_;
};

std::string irOpName(IROp op);
std::ostream& operator<<(std::ostream& out, IRRegister reg);
