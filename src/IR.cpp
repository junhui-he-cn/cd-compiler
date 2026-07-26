#include "IR.hpp"

#include <iomanip>
#include <stdexcept>
#include <utility>

namespace {

bool isUnary(IROp op)
{
    return op == IROp::Negate || op == IROp::Not;
}

bool isBinary(IROp op)
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
    case IROp::Constant:
    case IROp::MakeFunction:
    case IROp::Array:
    case IROp::Struct:
    case IROp::Variant:
    case IROp::VariantTag:
    case IROp::VariantField:
    case IROp::Map:
    case IROp::Copy:
    case IROp::LoadVar:
    case IROp::StoreVar:
    case IROp::AssignVar:
    case IROp::Call:
    case IROp::NativeCall:
    case IROp::Index:
    case IROp::AssignIndex:
    case IROp::Field:
    case IROp::AssignField:
    case IROp::Len:
    case IROp::AssertArray:
    case IROp::AssertNumber:
    case IROp::Print:
    case IROp::Return:
    case IROp::Negate:
    case IROp::Not:
    case IROp::Jump:
    case IROp::JumpIfFalse:
    case IROp::JumpIfTrue:
        return false;
    }

    return false;
}

bool hasActiveFunction(const std::vector<IRFunction>& functionStack)
{
    return !functionStack.empty();
}

IRFunction& activeFunction(std::vector<IRFunction>& functionStack)
{
    if (functionStack.empty()) {
        throw std::logic_error("no active IR function");
    }
    return functionStack.back();
}

const IRFunction& activeFunction(const std::vector<IRFunction>& functionStack)
{
    if (functionStack.empty()) {
        throw std::logic_error("no active IR function");
    }
    return functionStack.back();
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

void printIRConstantValue(std::ostream& out, const Value& value)
{
    if (value.type() == Value::Type::String) {
        printEscapedStringLiteral(out, value.asString());
        return;
    }

    out << value;
}

void printConstantOperand(std::ostream& out, const IRProgram& program, std::size_t operand)
{
    out << " #" << operand;
    if (operand < program.constants().size()) {
        out << " ";
        printIRConstantValue(out, program.constants()[operand]);
    }
}

void printNameOperand(std::ostream& out, const IRProgram& program, std::size_t operand)
{
    out << " @" << operand;
    if (operand < program.names().size()) {
        out << " " << program.names()[operand];
    }
}

void printInstruction(std::ostream& out, const IRProgram& program, const IRInstruction& instruction, std::size_t index)
{
    out << std::setw(4) << std::setfill('0') << index << std::setfill(' ') << "  ";

    if (instruction.dest) {
        out << *instruction.dest << " = ";
    }

    out << irOpName(instruction.op);

    if (instruction.op == IROp::Constant) {
        printConstantOperand(out, program, instruction.operand);
    } else if (instruction.op == IROp::MakeFunction) {
        out << " $" << instruction.operand;
        if (instruction.operand < program.functions().size()) {
            const IRFunction& function = program.functions()[instruction.operand];
            out << " " << function.name << "/" << function.parameters.size();
        }
    } else if (instruction.op == IROp::Array) {
        out << " [";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg];
        }
        out << "]";
    } else if (instruction.op == IROp::Map) {
        if (instruction.arguments.size() % 2 != 0) {
            throw std::logic_error("map expects key/value register pairs");
        }
        out << " [";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); arg += 2) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg] << ": " << instruction.arguments[arg + 1];
        }
        out << "]";
    } else if (instruction.op == IROp::Struct) {
        if (instruction.typeNameOperand) {
            out << " ";
            if (*instruction.typeNameOperand < program.names().size()) {
                out << program.names()[*instruction.typeNameOperand];
            } else {
                out << "@" << *instruction.typeNameOperand;
            }
        }
        out << " {";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            if (arg < instruction.operands.size() && instruction.operands[arg] < program.names().size()) {
                out << program.names()[instruction.operands[arg]];
            } else {
                out << "@" << arg;
            }
            out << ": " << instruction.arguments[arg];
        }
        out << "}";
    } else if (instruction.op == IROp::Variant) {
        if (instruction.typeNameOperand && *instruction.typeNameOperand < program.names().size()) {
            out << " " << program.names()[*instruction.typeNameOperand];
        }
        if (instruction.variantNameOperand && *instruction.variantNameOperand < program.names().size()) {
            out << "." << program.names()[*instruction.variantNameOperand];
        }
        out << " (";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg];
        }
        out << ")";
    } else if (instruction.op == IROp::VariantTag) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        if (instruction.typeNameOperand && *instruction.typeNameOperand < program.names().size()) {
            out << " " << program.names()[*instruction.typeNameOperand];
        }
        if (instruction.variantNameOperand && *instruction.variantNameOperand < program.names().size()) {
            out << "." << program.names()[*instruction.variantNameOperand];
        }
    } else if (instruction.op == IROp::VariantField) {
        if (instruction.left) {
            out << " " << *instruction.left << "[" << instruction.operand << "]";
        }
    } else if (instruction.op == IROp::Copy) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
    } else if (instruction.op == IROp::LoadVar) {
        printNameOperand(out, program, instruction.operand);
    } else if (instruction.op == IROp::StoreVar || instruction.op == IROp::AssignVar) {
        printNameOperand(out, program, instruction.operand);
        if (instruction.left) {
            out << ", " << *instruction.left;
        }
    } else if (instruction.op == IROp::Call) {
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
    } else if (instruction.op == IROp::NativeCall) {
        printNameOperand(out, program, instruction.operand);
        out << "(";
        for (std::size_t arg = 0; arg < instruction.arguments.size(); ++arg) {
            if (arg != 0) {
                out << ", ";
            }
            out << instruction.arguments[arg];
        }
        out << ")";
    } else if (instruction.op == IROp::Index || instruction.op == IROp::AssignIndex) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        if (instruction.right) {
            out << ", " << *instruction.right;
        }
        if (instruction.op == IROp::AssignIndex && !instruction.arguments.empty()) {
            out << ", " << instruction.arguments.front();
        }
    } else if (instruction.op == IROp::Field) {
        if (instruction.left) {
            out << " " << *instruction.left << ".";
            if (instruction.operand < program.names().size()) {
                out << program.names()[instruction.operand];
            } else {
                out << "@" << instruction.operand;
            }
        }
    } else if (instruction.op == IROp::AssignField) {
        if (instruction.left) {
            out << " " << *instruction.left << ".";
            if (instruction.operand < program.names().size()) {
                out << program.names()[instruction.operand];
            } else {
                out << "@" << instruction.operand;
            }
        }
        if (!instruction.arguments.empty()) {
            out << ", " << instruction.arguments.front();
        }
    } else if (instruction.op == IROp::Len || instruction.op == IROp::AssertArray) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
    } else if (instruction.op == IROp::AssertNumber) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
        printNameOperand(out, program, instruction.operand);
    } else if (instruction.op == IROp::Print) {
        if (instruction.left) {
            out << " " << *instruction.left;
        }
    } else if (instruction.op == IROp::Return) {
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
    } else if (instruction.op == IROp::Jump) {
        out << " " << std::setw(4) << std::setfill('0') << instruction.operand << std::setfill(' ');
    } else if (instruction.op == IROp::JumpIfFalse || instruction.op == IROp::JumpIfTrue) {
        if (instruction.left) {
            out << " " << *instruction.left << ", ";
        } else {
            out << " ";
        }
        out << std::setw(4) << std::setfill('0') << instruction.operand << std::setfill(' ');
    }

    out << '\n';
}

} // namespace

void IRProgram::setSources(std::vector<SourceFile> sources)
{
    sources_ = std::move(sources);
}

const std::vector<SourceFile>& IRProgram::sources() const
{
    return sources_;
}

void IRProgram::setCurrentSpan(std::optional<SourceSpan> span)
{
    currentSpan_ = std::move(span);
}

std::size_t IRProgram::addConstant(Value value)
{
    constants_.push_back(std::move(value));
    return constants_.size() - 1;
}

std::size_t IRProgram::addName(std::string name)
{
    names_.push_back(std::move(name));
    return names_.size() - 1;
}

IRRegister IRProgram::makeRegister()
{
    if (hasActiveFunction(functionStack_)) {
        return IRRegister{activeFunction(functionStack_).registerCount++};
    }
    return IRRegister{registerCount_++};
}

void IRProgram::beginFunction(std::string name, std::vector<std::string> parameters)
{
    functionStack_.push_back(IRFunction{std::move(name), std::move(parameters), {}, 0});
}

std::size_t IRProgram::endFunction()
{
    if (!hasActiveFunction(functionStack_)) {
        throw std::logic_error("not building IR function");
    }
    IRFunction function = std::move(functionStack_.back());
    functionStack_.pop_back();
    functions_.push_back(std::move(function));
    return functions_.size() - 1;
}

void IRProgram::addModuleDependency(IRModuleDependency dependency)
{
    moduleDependencies_.push_back(std::move(dependency));
}

IRRegister IRProgram::emitConstant(Value value)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::Constant, dest, std::nullopt, std::nullopt, {}, addConstant(std::move(value))});
    return dest;
}

IRRegister IRProgram::emitMakeFunction(std::size_t functionIndex)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::MakeFunction, dest, std::nullopt, std::nullopt, {}, functionIndex});
    return dest;
}

IRRegister IRProgram::emitArray(std::vector<IRRegister> elements)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::Array, dest, std::nullopt, std::nullopt, std::move(elements), 0});
    return dest;
}

IRRegister IRProgram::emitMap(std::vector<IRRegister> keyValueRegisters)
{
    if (keyValueRegisters.size() % 2 != 0) {
        throw std::logic_error("map expects key/value register pairs");
    }
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::Map, dest, std::nullopt, std::nullopt, std::move(keyValueRegisters), 0});
    return dest;
}

IRRegister IRProgram::emitStruct(
    std::vector<std::size_t> fieldNames,
    std::vector<IRRegister> fieldValues,
    std::optional<std::size_t> typeNameOperand)
{
    IRRegister dest = makeRegister();
    IRInstruction instruction{IROp::Struct, dest, std::nullopt, std::nullopt, std::move(fieldValues), 0};
    instruction.operands = std::move(fieldNames);
    instruction.typeNameOperand = typeNameOperand;
    emit(std::move(instruction));
    return dest;
}

IRRegister IRProgram::emitVariant(
    std::string enumName,
    std::string variantName,
    std::vector<IRRegister> payload)
{
    IRRegister dest = makeRegister();
    IRInstruction instruction{IROp::Variant, dest, std::nullopt, std::nullopt, std::move(payload), 0};
    instruction.typeNameOperand = addName(std::move(enumName));
    instruction.variantNameOperand = addName(std::move(variantName));
    emit(std::move(instruction));
    return dest;
}

IRRegister IRProgram::emitVariantTag(IRRegister value, std::string enumName, std::string variantName)
{
    IRRegister dest = makeRegister();
    IRInstruction instruction{IROp::VariantTag, dest, value, std::nullopt, {}, 0};
    instruction.typeNameOperand = addName(std::move(enumName));
    instruction.variantNameOperand = addName(std::move(variantName));
    emit(std::move(instruction));
    return dest;
}

IRRegister IRProgram::emitVariantField(IRRegister value, std::size_t index)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::VariantField, dest, value, std::nullopt, {}, index});
    return dest;
}

IRRegister IRProgram::emitCopy(IRRegister value)
{
    IRRegister dest = makeRegister();
    emitCopyTo(dest, value);
    return dest;
}

void IRProgram::emitCopyTo(IRRegister dest, IRRegister value)
{
    emit(IRInstruction{IROp::Copy, dest, value, std::nullopt, {}, 0});
}

IRRegister IRProgram::emitLoadVar(std::string name)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::LoadVar, dest, std::nullopt, std::nullopt, {}, addName(std::move(name))});
    return dest;
}

void IRProgram::emitStoreVar(std::string name, IRRegister value)
{
    emit(IRInstruction{IROp::StoreVar, std::nullopt, value, std::nullopt, {}, addName(std::move(name))});
}

void IRProgram::emitAssignVar(std::string name, IRRegister value)
{
    emit(IRInstruction{IROp::AssignVar, std::nullopt, value, std::nullopt, {}, addName(std::move(name))});
}

IRRegister IRProgram::emitCall(IRRegister callee, std::vector<IRRegister> arguments)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::Call, dest, callee, std::nullopt, std::move(arguments), 0});
    return dest;
}

IRRegister IRProgram::emitNativeCall(std::string name, std::vector<IRRegister> arguments)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::NativeCall, dest, std::nullopt, std::nullopt, std::move(arguments), addName(std::move(name))});
    return dest;
}

IRRegister IRProgram::emitIndex(IRRegister collection, IRRegister index)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::Index, dest, collection, index, {}, 0});
    return dest;
}

IRRegister IRProgram::emitAssignIndex(IRRegister collection, IRRegister index, IRRegister value)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::AssignIndex, dest, collection, index, {value}, 0});
    return dest;
}

IRRegister IRProgram::emitField(IRRegister object, std::string fieldName)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::Field, dest, object, std::nullopt, {}, addName(std::move(fieldName))});
    return dest;
}

IRRegister IRProgram::emitAssignField(IRRegister object, std::string fieldName, IRRegister value)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::AssignField, dest, object, std::nullopt, {value}, addName(std::move(fieldName))});
    return dest;
}

IRRegister IRProgram::emitLen(IRRegister value)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::Len, dest, value, std::nullopt, {}, 0});
    return dest;
}

IRRegister IRProgram::emitAssertArray(IRRegister value)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::AssertArray, dest, value, std::nullopt, {}, 0});
    return dest;
}

IRRegister IRProgram::emitAssertNumber(IRRegister value, std::string message)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{IROp::AssertNumber, dest, value, std::nullopt, {}, addName(std::move(message))});
    return dest;
}

void IRProgram::emitPrint(IRRegister value)
{
    emit(IRInstruction{IROp::Print, std::nullopt, value, std::nullopt, {}, 0});
}

void IRProgram::emitReturn(IRRegister value)
{
    emit(IRInstruction{IROp::Return, std::nullopt, value, std::nullopt, {}, 0});
}

IRRegister IRProgram::emitUnary(IROp op, IRRegister value)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{op, dest, value, std::nullopt, {}, 0});
    return dest;
}

IRRegister IRProgram::emitBinary(IROp op, IRRegister left, IRRegister right)
{
    IRRegister dest = makeRegister();
    emit(IRInstruction{op, dest, left, right, {}, 0});
    return dest;
}

std::size_t IRProgram::emitJump()
{
    const std::size_t instruction = instructionCount();
    emit(IRInstruction{IROp::Jump, std::nullopt, std::nullopt, std::nullopt, {}, 0});
    return instruction;
}

void IRProgram::emitJumpTo(std::size_t target)
{
    emit(IRInstruction{IROp::Jump, std::nullopt, std::nullopt, std::nullopt, {}, target});
}

std::size_t IRProgram::emitJumpIfFalse(IRRegister condition)
{
    const std::size_t instruction = instructionCount();
    emit(IRInstruction{IROp::JumpIfFalse, std::nullopt, condition, std::nullopt, {}, 0});
    return instruction;
}

std::size_t IRProgram::emitJumpIfTrue(IRRegister condition)
{
    const std::size_t instruction = instructionCount();
    emit(IRInstruction{IROp::JumpIfTrue, std::nullopt, condition, std::nullopt, {}, 0});
    return instruction;
}

void IRProgram::patchJump(std::size_t jumpInstruction)
{
    auto& instructions = hasActiveFunction(functionStack_)
        ? activeFunction(functionStack_).instructions
        : instructions_;
    if (jumpInstruction >= instructions.size()) {
        throw std::logic_error("jump instruction index out of range");
    }

    auto& instruction = instructions[jumpInstruction];
    if (instruction.op != IROp::Jump && instruction.op != IROp::JumpIfFalse
        && instruction.op != IROp::JumpIfTrue) {
        throw std::logic_error("cannot patch non-jump instruction");
    }

    instruction.operand = instructions.size();
}

const std::vector<Value>& IRProgram::constants() const
{
    return constants_;
}

const std::vector<std::string>& IRProgram::names() const
{
    return names_;
}

const std::vector<IRInstruction>& IRProgram::instructions() const
{
    return instructions_;
}

const std::vector<IRFunction>& IRProgram::functions() const
{
    return functions_;
}

const std::vector<IRModuleDependency>& IRProgram::moduleDependencies() const
{
    return moduleDependencies_;
}

std::size_t IRProgram::registerCount() const
{
    return registerCount_;
}

std::size_t IRProgram::instructionCount() const
{
    if (hasActiveFunction(functionStack_)) {
        return activeFunction(functionStack_).instructions.size();
    }
    return instructions_.size();
}

void IRProgram::print(std::ostream& out) const
{
    out << "IR\n";
    for (std::size_t i = 0; i < instructions_.size(); ++i) {
        printInstruction(out, *this, instructions_[i], i);
    }

    for (std::size_t functionIndex = 0; functionIndex < functions_.size(); ++functionIndex) {
        const IRFunction& function = functions_[functionIndex];
        out << '\n' << "function $" << functionIndex << " " << function.name << "(";
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << function.parameters[i];
        }
        out << ")\n";
        for (std::size_t i = 0; i < function.instructions.size(); ++i) {
            printInstruction(out, *this, function.instructions[i], i);
        }
    }
}

void IRProgram::emit(IRInstruction instruction)
{
    instruction.span = currentSpan_;
    if (hasActiveFunction(functionStack_)) {
        activeFunction(functionStack_).instructions.push_back(std::move(instruction));
        return;
    }
    instructions_.push_back(std::move(instruction));
}

std::string irOpName(IROp op)
{
    switch (op) {
    case IROp::Constant:
        return "constant";
    case IROp::MakeFunction:
        return "make_function";
    case IROp::Array:
        return "array";
    case IROp::Map:
        return "map";
    case IROp::Struct:
        return "struct";
    case IROp::Variant:
        return "variant";
    case IROp::VariantTag:
        return "variant_tag";
    case IROp::VariantField:
        return "variant_field";
    case IROp::Copy:
        return "copy";
    case IROp::LoadVar:
        return "load_var";
    case IROp::StoreVar:
        return "store_var";
    case IROp::AssignVar:
        return "assign_var";
    case IROp::Call:
        return "call";
    case IROp::NativeCall:
        return "native_call";
    case IROp::Index:
        return "index";
    case IROp::AssignIndex:
        return "assign_index";
    case IROp::Field:
        return "field";
    case IROp::AssignField:
        return "assign_field";
    case IROp::Len:
        return "len";
    case IROp::AssertArray:
        return "assert_array";
    case IROp::AssertNumber:
        return "assert_number";
    case IROp::Print:
        return "print";
    case IROp::Return:
        return "return";
    case IROp::Negate:
        return "negate";
    case IROp::Not:
        return "not";
    case IROp::Add:
        return "add";
    case IROp::Subtract:
        return "subtract";
    case IROp::Multiply:
        return "multiply";
    case IROp::Divide:
        return "divide";
    case IROp::Equal:
        return "equal";
    case IROp::NotEqual:
        return "not_equal";
    case IROp::Greater:
        return "greater";
    case IROp::GreaterEqual:
        return "greater_equal";
    case IROp::Less:
        return "less";
    case IROp::LessEqual:
        return "less_equal";
    case IROp::Jump:
        return "jump";
    case IROp::JumpIfFalse:
        return "jump_if_false";
    case IROp::JumpIfTrue:
        return "jump_if_true";
    }

    return "unknown";
}

std::ostream& operator<<(std::ostream& out, IRRegister reg)
{
    out << 'v' << reg.index;
    return out;
}
