#include "Bytecode.hpp"

#include <iomanip>
#include <utility>

namespace {

bool isUnary(BytecodeOp op)
{
    return op == BytecodeOp::Negate || op == BytecodeOp::NegNum || op == BytecodeOp::Not;
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
    case BytecodeOp::AddNum:
    case BytecodeOp::SubNum:
    case BytecodeOp::MulNum:
    case BytecodeOp::DivNum:
    case BytecodeOp::ConcatStr:
    case BytecodeOp::LessNum:
    case BytecodeOp::LessEqualNum:
    case BytecodeOp::GreaterNum:
    case BytecodeOp::GreaterEqualNum:
    case BytecodeOp::LessStr:
    case BytecodeOp::LessEqualStr:
    case BytecodeOp::GreaterStr:
    case BytecodeOp::GreaterEqualStr:
        return true;
    case BytecodeOp::Constant:
    case BytecodeOp::MakeFunction:
    case BytecodeOp::Array:
    case BytecodeOp::Map:
    case BytecodeOp::MakeStruct:
    case BytecodeOp::StructGet:
    case BytecodeOp::StructSet:
    case BytecodeOp::MakeVariant:
    case BytecodeOp::IsVariant:
    case BytecodeOp::VariantGet:
    case BytecodeOp::Field:
    case BytecodeOp::AssignField:
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
    case BytecodeOp::CallNative:
    case BytecodeOp::Index:
    case BytecodeOp::AssignIndex:
    case BytecodeOp::Len:
    case BytecodeOp::AssertArray:
    case BytecodeOp::AssertNumber:
    case BytecodeOp::Return:
    case BytecodeOp::Negate:
    case BytecodeOp::NegNum:
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
    } else if (instruction.op == BytecodeOp::MakeStruct) {
        out << " t" << instruction.operand << " [";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg];
        }
        out << "]";
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
    } else if (instruction.op == BytecodeOp::StructGet || instruction.op == BytecodeOp::StructSet) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        out << ", t" << instruction.operand << ", "
            << (instruction.operands.empty() ? 0 : instruction.operands.front());
        if (instruction.op == BytecodeOp::StructSet && !instruction.arguments.empty()) {
            out << ", " << instruction.arguments.front();
        }
    } else if (instruction.op == BytecodeOp::MakeVariant) {
        out << " t" << instruction.operand << ", v" << instruction.variantNameOperand.value_or(0) << " (";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg];
        }
        out << ")";
    } else if (instruction.op == BytecodeOp::IsVariant) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        out << ", t" << instruction.operand << ", v" << instruction.variantNameOperand.value_or(0);
    } else if (instruction.op == BytecodeOp::VariantGet) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        out << ", t" << instruction.typeNameOperand.value_or(0)
            << ", v" << instruction.variantNameOperand.value_or(0)
            << ", " << instruction.operand;
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
    } else if (instruction.op == BytecodeOp::CallNative) {
        out << " i" << instruction.operand;
        out << " [";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg];
        }
        out << "]";
    } else if (instruction.op == BytecodeOp::Index
        || instruction.op == BytecodeOp::AssignIndex
        || instruction.op == BytecodeOp::ArrayGet
        || instruction.op == BytecodeOp::ArraySet
        || instruction.op == BytecodeOp::MapGet
        || instruction.op == BytecodeOp::MapSet
        || instruction.op == BytecodeOp::RangeGet) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        if (instruction.right) {
            out << ", " << *instruction.right;
        }
        if ((instruction.op == BytecodeOp::AssignIndex
                || instruction.op == BytecodeOp::ArraySet
                || instruction.op == BytecodeOp::MapSet)
            && !instruction.arguments.empty()) {
            out << ", " << instruction.arguments.front();
        }
    } else if (instruction.op == BytecodeOp::Len
        || instruction.op == BytecodeOp::LenArray
        || instruction.op == BytecodeOp::LenMap
        || instruction.op == BytecodeOp::LenRange
        || instruction.op == BytecodeOp::LenStr
        || instruction.op == BytecodeOp::AssertArray) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
    } else if (instruction.op == BytecodeOp::AssertNumber) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        printNameOperand(out, program, instruction.operand);
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

void BytecodeProgram::setTypes(std::vector<BytecodeType> types)
{
    types_ = std::move(types);
}

void BytecodeProgram::setNativeImports(std::vector<BytecodeNativeImport> nativeImports)
{
    nativeImports_ = std::move(nativeImports);
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

const std::vector<BytecodeType>& BytecodeProgram::types() const
{
    return types_;
}

const std::vector<BytecodeNativeImport>& BytecodeProgram::nativeImports() const
{
    return nativeImports_;
}

std::uint32_t BytecodeProgram::remapDependencyOffset(std::uint32_t irOffset) const
{
    const auto found = dependencyRemap_.find(irOffset);
    return found == dependencyRemap_.end() ? irOffset : found->second;
}

void BytecodeProgram::print(std::ostream& out) const
{
    for (std::size_t i = 0; i < nativeImports_.size(); ++i) {
        out << "native_import i" << i << " = " << nativeImports_[i].name
            << " abi=" << nativeImports_[i].abiVersion << '\n';
    }
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
    case BytecodeOp::MakeStruct:
        return "make_struct";
    case BytecodeOp::Field:
        return "field";
    case BytecodeOp::AssignField:
        return "assign_field";
    case BytecodeOp::StructGet:
        return "struct_get";
    case BytecodeOp::StructSet:
        return "struct_set";
    case BytecodeOp::MakeVariant:
        return "make_variant";
    case BytecodeOp::IsVariant:
        return "is_variant";
    case BytecodeOp::VariantGet:
        return "variant_get";
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
    case BytecodeOp::CallNative:
        return "call_native";
    case BytecodeOp::Index:
        return "index";
    case BytecodeOp::AssignIndex:
        return "assign_index";
    case BytecodeOp::ArrayGet:
        return "array_get";
    case BytecodeOp::ArraySet:
        return "array_set";
    case BytecodeOp::MapGet:
        return "map_get";
    case BytecodeOp::MapSet:
        return "map_set";
    case BytecodeOp::RangeGet:
        return "range_get";
    case BytecodeOp::Len:
        return "len";
    case BytecodeOp::LenArray:
        return "len_array";
    case BytecodeOp::LenMap:
        return "len_map";
    case BytecodeOp::LenRange:
        return "len_range";
    case BytecodeOp::LenStr:
        return "len_str";
    case BytecodeOp::AssertArray:
        return "assert_array";
    case BytecodeOp::AssertNumber:
        return "assert_number";
    case BytecodeOp::Return:
        return "return";
    case BytecodeOp::Negate:
        return "negate";
    case BytecodeOp::NegNum:
        return "neg_num";
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
    case BytecodeOp::AddNum:
        return "add_num";
    case BytecodeOp::SubNum:
        return "sub_num";
    case BytecodeOp::MulNum:
        return "mul_num";
    case BytecodeOp::DivNum:
        return "div_num";
    case BytecodeOp::ConcatStr:
        return "concat_str";
    case BytecodeOp::LessNum:
        return "lt_num";
    case BytecodeOp::LessEqualNum:
        return "le_num";
    case BytecodeOp::GreaterNum:
        return "gt_num";
    case BytecodeOp::GreaterEqualNum:
        return "ge_num";
    case BytecodeOp::LessStr:
        return "lt_str";
    case BytecodeOp::LessEqualStr:
        return "le_str";
    case BytecodeOp::GreaterStr:
        return "gt_str";
    case BytecodeOp::GreaterEqualStr:
        return "ge_str";
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
