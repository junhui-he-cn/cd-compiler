#include "Bytecode.hpp"

#include <iomanip>
#include <utility>

namespace {

bool isUnary(BytecodeOp op)
{
    return op == BytecodeOp::Negate || op == BytecodeOp::Not;
}

bool isBinary(BytecodeOp op)
{
    switch (op) {
    case BytecodeOp::Add:
    case BytecodeOp::Subtract:
    case BytecodeOp::Multiply:
    case BytecodeOp::Divide:
    case BytecodeOp::Equal:
    case BytecodeOp::NotEqual:
    case BytecodeOp::Greater:
    case BytecodeOp::GreaterEqual:
    case BytecodeOp::Less:
    case BytecodeOp::LessEqual:
        return true;
    case BytecodeOp::Constant:
    case BytecodeOp::MakeFunction:
    case BytecodeOp::Array:
    case BytecodeOp::Map:
    case BytecodeOp::Struct:
    case BytecodeOp::Variant:
    case BytecodeOp::VariantTag:
    case BytecodeOp::VariantField:
    case BytecodeOp::Move:
    case BytecodeOp::LoadVar:
    case BytecodeOp::StoreVar:
    case BytecodeOp::AssignVar:
    case BytecodeOp::LoadLocal:
    case BytecodeOp::BindLocal:
    case BytecodeOp::SetLocal:
    case BytecodeOp::LoadUpvalue:
    case BytecodeOp::SetUpvalue:
    case BytecodeOp::LoadGlobal:
    case BytecodeOp::InitGlobal:
    case BytecodeOp::SetGlobal:
    case BytecodeOp::Call:
    case BytecodeOp::NativeCall:
    case BytecodeOp::Index:
    case BytecodeOp::AssignIndex:
    case BytecodeOp::Field:
    case BytecodeOp::AssignField:
    case BytecodeOp::Len:
    case BytecodeOp::AssertArray:
    case BytecodeOp::AssertNumber:
    case BytecodeOp::Print:
    case BytecodeOp::Return:
    case BytecodeOp::Negate:
    case BytecodeOp::Not:
    case BytecodeOp::Jump:
    case BytecodeOp::JumpIfFalse:
    case BytecodeOp::JumpIfTrue:
    case BytecodeOp::BlockStart:
    case BytecodeOp::Br:
    case BytecodeOp::BrIf:
    case BytecodeOp::ReturnNil:
        return false;
    }

    return false;
}

void printEscapedStringLiteral(std::ostream& out, const std::string& value)
{
    out << '"';
    for (char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            out << ch;
            break;
        }
    }
    out << '"';
}

void printBytecodeConstantValue(std::ostream& out, const Value& value)
{
    if (value.type() == Value::Type::String) {
        printEscapedStringLiteral(out, value.asString());
        return;
    }

    out << value;
}

void printConstantOperand(std::ostream& out, const BytecodeProgram& program, std::uint32_t operand)
{
    out << " #" << operand;
    if (operand < program.constants().size()) {
        out << " ";
        printBytecodeConstantValue(out, program.constants()[operand]);
    }
}

void printNameOperand(std::ostream& out, const BytecodeProgram& program, std::uint32_t operand)
{
    out << " @" << operand;
    if (operand < program.names().size()) {
        out << " " << program.names()[operand];
    }
}

void printInstruction(
    std::ostream& out,
    const BytecodeProgram& program,
    const BytecodeInstruction& instruction,
    std::size_t index)
{
    out << std::setw(4) << std::setfill('0') << index << std::setfill(' ') << "  ";

    if (instruction.dest) {
        out << *instruction.dest << " = ";
    }

    out << bytecodeOpName(instruction.op);

    if (instruction.op == BytecodeOp::Constant) {
        printConstantOperand(out, program, instruction.operand);
    } else if (instruction.op == BytecodeOp::MakeFunction) {
        out << " $" << instruction.operand;
        if (instruction.operand < program.functions().size()) {
            const BytecodeFunction& function = program.functions()[instruction.operand];
            out << " " << function.name << "/" << function.parameters.size();
        }
    } else if (instruction.op == BytecodeOp::Array) {
        out << " [";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg];
        }
        out << "]";
    } else if (instruction.op == BytecodeOp::Map) {
        out << " [";
        for (std::size_t arg = 0; arg + 1 < instruction.arguments.size(); arg += 2) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg] << ": " << instruction.arguments[arg + 1];
        }
        out << "]";
    } else if (instruction.op == BytecodeOp::Struct) {
        if (instruction.typeNameOperand) {
            printNameOperand(out, program, *instruction.typeNameOperand);
        }
        out << " {";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            if (arg < instruction.operands.size()) {
                printNameOperand(out, program, instruction.operands[arg]);
            } else {
                out << " @" << arg;
            }
            out << ": " << instruction.arguments[arg];
        }
        out << "}";
    } else if (instruction.op == BytecodeOp::Variant) {
        if (instruction.typeNameOperand) {
            printNameOperand(out, program, *instruction.typeNameOperand);
        }
        if (instruction.variantNameOperand) {
            out << ".";
            printNameOperand(out, program, *instruction.variantNameOperand);
        }
        out << " (";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg];
        }
        out << ")";
    } else if (instruction.op == BytecodeOp::VariantTag) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        if (instruction.typeNameOperand) {
            out << " ";
            printNameOperand(out, program, *instruction.typeNameOperand);
        }
        if (instruction.variantNameOperand) {
            out << ".";
            printNameOperand(out, program, *instruction.variantNameOperand);
        }
    } else if (instruction.op == BytecodeOp::VariantField) {
        if (instruction.left) {
            out << " " << *instruction.left << "[" << instruction.operand << "]";
        }
    } else if (instruction.op == BytecodeOp::Move) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
    } else if (instruction.op == BytecodeOp::LoadVar) {
        printNameOperand(out, program, instruction.operand);
    } else if (instruction.op == BytecodeOp::StoreVar || instruction.op == BytecodeOp::AssignVar) {
        printNameOperand(out, program, instruction.operand);
        if (instruction.left) {
            out << ", " << *instruction.left;
        }
    } else if (instruction.op == BytecodeOp::LoadLocal) {
        out << " l" << instruction.operand;
    } else if (instruction.op == BytecodeOp::BindLocal || instruction.op == BytecodeOp::SetLocal) {
        out << " l" << instruction.operand;
        if (instruction.left) {
            out << ", " << *instruction.left;
        }
    } else if (instruction.op == BytecodeOp::LoadUpvalue) {
        out << " u" << instruction.operand;
    } else if (instruction.op == BytecodeOp::SetUpvalue) {
        out << " u" << instruction.operand;
        if (instruction.left) {
            out << ", " << *instruction.left;
        }
    } else if (instruction.op == BytecodeOp::LoadGlobal) {
        out << " g" << instruction.operand;
    } else if (instruction.op == BytecodeOp::InitGlobal || instruction.op == BytecodeOp::SetGlobal) {
        out << " g" << instruction.operand;
        if (instruction.left) {
            out << ", " << *instruction.left;
        }
    } else if (instruction.op == BytecodeOp::Call) {
        if (instruction.left) {
            out << " " << *instruction.left << "(";
            for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
                if (arg != 0) {
                    out << ", ";
                }
                out << instruction.arguments[arg];
            }
            out << ")";
        }
    } else if (instruction.op == BytecodeOp::NativeCall) {
        printNameOperand(out, program, instruction.operand);
        out << " [";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg];
        }
        out << "]";
    } else if (instruction.op == BytecodeOp::Index || instruction.op == BytecodeOp::AssignIndex) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        if (instruction.right) {
            out << ", " << *instruction.right;
        }
        if (instruction.op == BytecodeOp::AssignIndex && !instruction.arguments.empty()) {
            out << ", " << instruction.arguments.front();
        }
    } else if (instruction.op == BytecodeOp::Field) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        out << ", ";
        printNameOperand(out, program, instruction.operand);
    } else if (instruction.op == BytecodeOp::AssignField) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        out << ", ";
        printNameOperand(out, program, instruction.operand);
        if (!instruction.arguments.empty()) {
            out << ", " << instruction.arguments.front();
        }
    } else if (instruction.op == BytecodeOp::Len || instruction.op == BytecodeOp::AssertArray) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
    } else if (instruction.op == BytecodeOp::AssertNumber) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        printNameOperand(out, program, instruction.operand);
    } else if (instruction.op == BytecodeOp::Print) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
    } else if (instruction.op == BytecodeOp::Return) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
    } else if (isUnary(instruction.op)) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
    } else if (isBinary(instruction.op)) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        if (instruction.right) {
            out << ", " << *instruction.right;
        }
    } else if (instruction.op == BytecodeOp::Jump) {
        out << " " << std::setw(4) << std::setfill('0') << instruction.operand << std::setfill(' ');
    } else if (instruction.op == BytecodeOp::JumpIfFalse || instruction.op == BytecodeOp::JumpIfTrue) {
        if (instruction.left) {
            out << " " << *instruction.left << ", ";
        } else {
            out << " ";
        }
        out << std::setw(4) << std::setfill('0') << instruction.operand << std::setfill(' ');
    } else if (instruction.op == BytecodeOp::Br) {
        out << " b" << instruction.operand;
    } else if (instruction.op == BytecodeOp::BrIf) {
        if (instruction.left) {
            out << " " << *instruction.left << ", b" << instruction.operand
                << ", b" << (instruction.operands.empty() ? 0 : instruction.operands.front());
        }
    } else if (instruction.op == BytecodeOp::BlockStart || instruction.op == BytecodeOp::ReturnNil) {
        // no operands
    }

    out << '\n';
}

} // namespace

void BytecodeProgram::setSources(std::vector<SourceFile> sources)
{
    sources_ = std::move(sources);
}

const std::vector<SourceFile>& BytecodeProgram::sources() const
{
    return sources_;
}

void BytecodeProgram::setConstants(std::vector<Value> constants)
{
    constants_ = std::move(constants);
}

void BytecodeProgram::setNames(std::vector<std::string> names)
{
    names_ = std::move(names);
}

void BytecodeProgram::setInstructions(std::vector<BytecodeInstruction> instructions)
{
    instructions_ = std::move(instructions);
}

void BytecodeProgram::setRegisterCount(std::uint32_t registerCount)
{
    registerCount_ = registerCount;
}

void BytecodeProgram::setFunctions(std::vector<BytecodeFunction> functions)
{
    functions_ = std::move(functions);
}

void BytecodeProgram::setGlobals(std::vector<std::uint32_t> globals)
{
    globals_ = std::move(globals);
}

void BytecodeProgram::setDependencyRemap(
    std::unordered_map<std::uint32_t, std::uint32_t> remap)
{
    dependencyRemap_ = std::move(remap);
}

const std::vector<Value>& BytecodeProgram::constants() const
{
    return constants_;
}

const std::vector<std::string>& BytecodeProgram::names() const
{
    return names_;
}

const std::vector<BytecodeInstruction>& BytecodeProgram::instructions() const
{
    return instructions_;
}

std::uint32_t BytecodeProgram::registerCount() const
{
    return registerCount_;
}

const std::vector<BytecodeFunction>& BytecodeProgram::functions() const
{
    return functions_;
}

const std::vector<std::uint32_t>& BytecodeProgram::globals() const
{
    return globals_;
}

std::uint32_t BytecodeProgram::remapDependencyOffset(std::uint32_t irOffset) const
{
    const auto found = dependencyRemap_.find(irOffset);
    return found == dependencyRemap_.end() ? irOffset : found->second;
}

void BytecodeProgram::print(std::ostream& out) const
{
    out << "main registers=" << registerCount_ << '\n';
    for (std::size_t i = 0; i < instructions_.size(); ++i) {
        printInstruction(out, *this, instructions_[i], i);
    }

    for (std::size_t functionIndex = 0; functionIndex < functions_.size(); ++functionIndex) {
        const BytecodeFunction& function = functions_[functionIndex];
        out << '\n'
            << "function $" << functionIndex << " " << function.name << "/" << function.parameters.size()
            << " registers=" << function.registerCount << '\n';
        for (std::size_t i = 0; i < function.upvalues.size(); ++i) {
            const char* source = function.upvalues[i].source == BytecodeUpvalueSource::Local
                ? "local l"
                : function.upvalues[i].source == BytecodeUpvalueSource::Upvalue ? "upvalue u" : "global g";
            out << "upvalue u" << i << " = " << source << function.upvalues[i].index << '\n';
        }
        for (std::size_t i = 0; i < function.instructions.size(); ++i) {
            printInstruction(out, *this, function.instructions[i], i);
        }
    }
}

std::string bytecodeOpName(BytecodeOp op)
{
    switch (op) {
    case BytecodeOp::Constant:
        return "constant";
    case BytecodeOp::MakeFunction:
        return "make_function";
    case BytecodeOp::Array:
        return "array";
    case BytecodeOp::Map:
        return "map";
    case BytecodeOp::Struct:
        return "struct";
    case BytecodeOp::Variant:
        return "variant";
    case BytecodeOp::VariantTag:
        return "variant_tag";
    case BytecodeOp::VariantField:
        return "variant_field";
    case BytecodeOp::Move:
        return "move";
    case BytecodeOp::LoadVar:
        return "load_var";
    case BytecodeOp::StoreVar:
        return "store_var";
    case BytecodeOp::AssignVar:
        return "assign_var";
    case BytecodeOp::LoadLocal:
        return "load_local";
    case BytecodeOp::BindLocal:
        return "bind_local";
    case BytecodeOp::SetLocal:
        return "set_local";
    case BytecodeOp::LoadUpvalue:
        return "load_upvalue";
    case BytecodeOp::SetUpvalue:
        return "set_upvalue";
    case BytecodeOp::LoadGlobal:
        return "load_global";
    case BytecodeOp::InitGlobal:
        return "init_global";
    case BytecodeOp::SetGlobal:
        return "set_global";
    case BytecodeOp::Call:
        return "call";
    case BytecodeOp::NativeCall:
        return "native_call";
    case BytecodeOp::Index:
        return "index";
    case BytecodeOp::AssignIndex:
        return "assign_index";
    case BytecodeOp::Field:
        return "field";
    case BytecodeOp::AssignField:
        return "assign_field";
    case BytecodeOp::Len:
        return "len";
    case BytecodeOp::AssertArray:
        return "assert_array";
    case BytecodeOp::AssertNumber:
        return "assert_number";
    case BytecodeOp::Print:
        return "print";
    case BytecodeOp::Return:
        return "return";
    case BytecodeOp::Negate:
        return "negate";
    case BytecodeOp::Not:
        return "not";
    case BytecodeOp::Add:
        return "add";
    case BytecodeOp::Subtract:
        return "subtract";
    case BytecodeOp::Multiply:
        return "multiply";
    case BytecodeOp::Divide:
        return "divide";
    case BytecodeOp::Equal:
        return "equal";
    case BytecodeOp::NotEqual:
        return "not_equal";
    case BytecodeOp::Greater:
        return "greater";
    case BytecodeOp::GreaterEqual:
        return "greater_equal";
    case BytecodeOp::Less:
        return "less";
    case BytecodeOp::LessEqual:
        return "less_equal";
    case BytecodeOp::Jump:
        return "jump";
    case BytecodeOp::JumpIfFalse:
        return "jump_if_false";
    case BytecodeOp::JumpIfTrue:
        return "jump_if_true";
    case BytecodeOp::BlockStart:
        return "block";
    case BytecodeOp::Br:
        return "br";
    case BytecodeOp::BrIf:
        return "br_if";
    case BytecodeOp::ReturnNil:
        return "return_nil";
    }

    return "unknown";
}

std::ostream& operator<<(std::ostream& out, BytecodeRegister reg)
{
    out << 'b' << reg.index;
    return out;
}
