#pragma once

#include "ModuleGraph.hpp"
#include "SourceMap.hpp"
#include "Value.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

struct BytecodeRegister {
    std::uint32_t index = 0;
};

enum class BytecodeOp {
    Constant,
    MakeFunction,
    Array,
    Map,
    Struct,
    Variant,
    VariantTag,
    VariantField,
    Move,
    LoadVar,
    StoreVar,
    AssignVar,
    LoadLocal,
    BindLocal,
    SetLocal,
    LoadUpvalue,
    SetUpvalue,
    LoadGlobal,
    InitGlobal,
    SetGlobal,
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

struct BytecodeInstruction {
    BytecodeOp op;
    std::optional<BytecodeRegister> dest;
    std::optional<BytecodeRegister> left;
    std::optional<BytecodeRegister> right;
    std::vector<BytecodeRegister> arguments;
    std::uint32_t operand = 0;
    std::vector<std::uint32_t> operands{};
    std::optional<std::uint32_t> typeNameOperand = std::nullopt;
    std::optional<std::uint32_t> variantNameOperand = std::nullopt;
    std::optional<SourceSpan> span = std::nullopt;
};

struct BytecodeUpvalue {
    bool sourceIsLocal = true;
    std::uint32_t source = 0;
};

struct BytecodeFunction {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<BytecodeInstruction> instructions;
    std::uint32_t registerCount = 0;
    std::uint32_t localCount = 0;
    std::vector<BytecodeUpvalue> upvalues;
};

struct BytecodeModuleDependency {
    std::string moduleIdentity;
    ModuleGraphEdgeKind kind = ModuleGraphEdgeKind::Import;
    std::string requestedPath;
    std::uint32_t instructionOffset = 0;
};

class BytecodeProgram {
public:
    void setSources(std::vector<SourceFile> sources);
    const std::vector<SourceFile>& sources() const;

    void setConstants(std::vector<Value> constants);
    void setNames(std::vector<std::string> names);
    void setInstructions(std::vector<BytecodeInstruction> instructions);
    void setRegisterCount(std::uint32_t registerCount);
    void setFunctions(std::vector<BytecodeFunction> functions);
    void setGlobals(std::vector<std::uint32_t> globals);

    const std::vector<Value>& constants() const;
    const std::vector<std::string>& names() const;
    const std::vector<BytecodeInstruction>& instructions() const;
    std::uint32_t registerCount() const;
    const std::vector<BytecodeFunction>& functions() const;
    const std::vector<std::uint32_t>& globals() const;

    void print(std::ostream& out) const;

private:
    std::vector<Value> constants_;
    std::vector<std::string> names_;
    std::vector<BytecodeInstruction> instructions_;
    std::uint32_t registerCount_ = 0;
    std::vector<BytecodeFunction> functions_;
    std::vector<std::uint32_t> globals_;
    std::vector<SourceFile> sources_;
};

// A module artifact carries one module's linked-program-compatible bytecode
// plus the metadata a future linker needs to expand its dependencies.  The
// existing BytecodeProgram remains the representation of a final linked
// program artifact.
struct BytecodeModuleArtifact {
    std::string identity;
    std::string path;
    std::string canonicalPath;
    bool isEntry = false;
    std::optional<std::uint32_t> entryOrder = std::nullopt;
    std::vector<BytecodeModuleDependency> dependencies;
    BytecodeProgram program;
};

std::string bytecodeOpName(BytecodeOp op);
std::ostream& operator<<(std::ostream& out, BytecodeRegister reg);
