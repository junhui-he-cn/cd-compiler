#include "IR.hpp"

#include <limits>

#include <algorithm>
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

void validateRegister(
    const std::optional<IRRegister>& reg,
    std::size_t registerCount,
    const std::string& context)
{
    if (reg && reg->index >= registerCount) {
        throw std::logic_error(context + " register is outside registerCount");
    }
}

void validateNameOperand(
    std::size_t operand,
    const std::vector<std::string>& names,
    const std::string& context)
{
    if (operand >= names.size()) {
        throw std::logic_error(context + " name operand is outside the name table");
    }
}

void validateInstructionStream(
    const std::vector<IRInstruction>& instructions,
    std::size_t registerCount,
    const std::vector<Value>& constants,
    const std::vector<std::string>& names,
    std::size_t functionCount,
    const std::vector<IRBinding>& bindings,
    const std::string& context)
{
    const auto hasBinding = [&bindings](BindingId id) {
        return std::find_if(
                   bindings.begin(),
                   bindings.end(),
                   [id](const IRBinding& binding) {
                       return binding.bindingId == id;
                   })
            != bindings.end();
    };

    for (std::size_t index = 0; index < instructions.size(); ++index) {
        const IRInstruction& instruction = instructions[index];
        validateRegister(instruction.dest, registerCount, context);
        validateRegister(instruction.left, registerCount, context);
        validateRegister(instruction.right, registerCount, context);
        for (const IRRegister argument : instruction.arguments) {
            validateRegister(argument, registerCount, context);
        }

        if (instruction.bindingId && !hasBinding(*instruction.bindingId)) {
            throw std::logic_error(context + " instruction references unknown binding metadata");
        }

        switch (instruction.op) {
        case IROp::Constant:
            if (instruction.operand >= constants.size()) {
                throw std::logic_error(context + " constant operand is outside the constant pool");
            }
            break;
        case IROp::MakeFunction:
            if (instruction.operand >= functionCount) {
                throw std::logic_error(context + " function operand is outside the function table");
            }
            break;
        case IROp::Map:
            if (instruction.arguments.size() % 2 != 0) {
                throw std::logic_error(context + " map has an incomplete key/value pair");
            }
            break;
        case IROp::Struct:
            if (instruction.operands.size() != instruction.arguments.size()) {
                throw std::logic_error(context + " struct field metadata does not match its values");
            }
            for (const std::size_t operand : instruction.operands) {
                validateNameOperand(operand, names, context);
            }
            break;
        case IROp::NativeCall:
        case IROp::LoadVar:
        case IROp::StoreVar:
        case IROp::AssignVar:
        case IROp::AssertNumber:
        case IROp::Field:
        case IROp::AssignField:
            validateNameOperand(instruction.operand, names, context);
            break;
        default:
            break;
        }

        if (instruction.typeNameOperand) {
            validateNameOperand(*instruction.typeNameOperand, names, context);
        }
        if (instruction.variantNameOperand) {
            validateNameOperand(*instruction.variantNameOperand, names, context);
        }

        if ((instruction.op == IROp::Jump
                || instruction.op == IROp::JumpIfFalse
                || instruction.op == IROp::JumpIfTrue)
            && instruction.operand > instructions.size()) {
            throw std::logic_error(context + " jump target is outside the instruction stream");
        }
    }
}

void validateFunctionBindings(
    const std::vector<IRFunction>& functions,
    const std::vector<IRBinding>& bindings)
{
    for (const IRFunction& function : functions) {
        std::vector<BindingId> seen;
        for (const IRBinding& binding : function.bindings) {
            if (!binding.bindingId.valid() || binding.resolvedName.empty()) {
                throw std::logic_error("function binding metadata is malformed");
            }
            if (std::find(seen.begin(), seen.end(), binding.bindingId) != seen.end()) {
                throw std::logic_error("function binding metadata contains a duplicate ID");
            }
            seen.push_back(binding.bindingId);
            const auto canonical = std::find_if(
                bindings.begin(),
                bindings.end(),
                [&binding](const IRBinding& current) {
                    return current.bindingId == binding.bindingId;
                });
            if (canonical == bindings.end()
                || canonical->resolvedName != binding.resolvedName
                || canonical->storage != binding.storage) {
                throw std::logic_error("function binding metadata is not canonical");
            }
        }
    }
}

void validateCanonicalBindings(const std::vector<IRBinding>& bindings)
{
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const IRBinding& binding = bindings[index];
        if (!binding.bindingId.valid() || binding.resolvedName.empty()) {
            throw std::logic_error("canonical binding metadata is malformed");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (bindings[previous].bindingId == binding.bindingId) {
                throw std::logic_error("canonical binding metadata contains a duplicate ID");
            }
        }
    }
}

} // namespace

bool IREffectSummary::isPure() const
{
    return !readsMemory && !writesMemory && !mayTrap && !allocates && !calls
        && !observable && !controlFlow;
}

IREffectSummary irEffectSummary(IROp op)
{
    IREffectSummary result;
    switch (op) {
    case IROp::Constant:
    case IROp::Copy:
        return result;
    case IROp::MakeFunction:
    case IROp::Array:
    case IROp::Map:
    case IROp::Struct:
    case IROp::Variant:
        result.allocates = true;
        return result;
    case IROp::VariantTag:
    case IROp::VariantField:
        result.readsMemory = true;
        result.mayTrap = true;
        return result;
    case IROp::LoadVar:
        result.readsMemory = true;
        return result;
    case IROp::StoreVar:
    case IROp::AssignVar:
        result.writesMemory = true;
        return result;
    case IROp::Call:
    case IROp::NativeCall:
        result.readsMemory = true;
        result.writesMemory = true;
        result.mayTrap = true;
        result.calls = true;
        return result;
    case IROp::Index:
    case IROp::Field:
    case IROp::Len:
    case IROp::AssertArray:
    case IROp::AssertNumber:
        result.readsMemory = true;
        result.mayTrap = true;
        return result;
    case IROp::AssignIndex:
    case IROp::AssignField:
        result.readsMemory = true;
        result.writesMemory = true;
        result.mayTrap = true;
        return result;
    case IROp::Print:
        result.observable = true;
        return result;
    case IROp::Return:
    case IROp::Jump:
    case IROp::JumpIfFalse:
    case IROp::JumpIfTrue:
        result.controlFlow = true;
        return result;
    case IROp::Negate:
    case IROp::Not:
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
        result.mayTrap = true;
        return result;
    }

    return result;
}

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
    const std::size_t parentId = functionStack_.empty()
        ? std::numeric_limits<std::size_t>::max()
        : functionStack_.back().id;
    functionStack_.push_back(IRFunction{
        std::move(name),
        std::move(parameters),
        {},
        0,
        {},
        {},
        nextFunctionId_,
        parentId,
    });
    ++nextFunctionId_;
}

void IRProgram::setFunctionParameterBindingIds(std::vector<BindingId> bindingIds)
{
    if (!hasActiveFunction(functionStack_)) {
        throw std::logic_error("parameter binding metadata requires an active IR function");
    }
    activeFunction(functionStack_).parameterBindingIds = std::move(bindingIds);
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

void IRProgram::addStructLayout(IRStructLayout layout)
{
    structLayouts_.push_back(std::move(layout));
}

void IRProgram::addEnumLayout(IREnumLayout layout)
{
    enumLayouts_.push_back(std::move(layout));
}

const std::vector<IRStructLayout>& IRProgram::structLayouts() const
{
    return structLayouts_;
}

const std::vector<IREnumLayout>& IRProgram::enumLayouts() const
{
    return enumLayouts_;
}

void IRProgram::addBinding(IRBinding binding)
{
    if (!binding.bindingId.valid()) {
        throw std::logic_error("IR binding metadata requires a valid binding ID");
    }
    if (binding.resolvedName.empty()) {
        throw std::logic_error("IR binding metadata requires a resolved name");
    }

    const auto existing = std::find_if(
        bindings_.begin(),
        bindings_.end(),
        [&binding](const IRBinding& current) {
            return current.bindingId == binding.bindingId;
        });
    if (existing != bindings_.end()) {
        throw std::logic_error("duplicate IR binding metadata");
    }

    bindings_.push_back(std::move(binding));
}

void IRProgram::addFunctionBinding(IRBinding binding)
{
    if (!binding.bindingId.valid()) {
        throw std::logic_error("IR binding metadata requires a valid binding ID");
    }
    if (binding.resolvedName.empty()) {
        throw std::logic_error("IR binding metadata requires a resolved name");
    }
    if (!hasActiveFunction(functionStack_)) {
        throw std::logic_error("function binding metadata requires an active IR function");
    }

    const auto canonical = std::find_if(
        bindings_.begin(),
        bindings_.end(),
        [&binding](const IRBinding& current) {
            return current.bindingId == binding.bindingId;
        });
    if (canonical == bindings_.end()) {
        throw std::logic_error("function binding metadata has no canonical binding");
    }
    if (canonical->resolvedName != binding.resolvedName
        || canonical->storage != binding.storage) {
        throw std::logic_error("conflicting IR binding metadata");
    }

    IRFunction& function = activeFunction(functionStack_);
    const auto existing = std::find_if(
        function.bindings.begin(),
        function.bindings.end(),
        [&binding](const IRBinding& current) {
            return current.bindingId == binding.bindingId;
        });
    if (existing == function.bindings.end()) {
        function.bindings.push_back(std::move(binding));
    } else if (existing->resolvedName != canonical->resolvedName
        || existing->storage != canonical->storage) {
        throw std::logic_error("conflicting function IR binding metadata");
    }
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

IRRegister IRProgram::emitVariantField(
    IRRegister value,
    std::size_t index,
    std::string enumName,
    std::string variantName)
{
    IRRegister dest = makeRegister();
    IRInstruction instruction{
        IROp::VariantField, dest, value, std::nullopt, {}, index};
    instruction.typeNameOperand = addName(std::move(enumName));
    instruction.variantNameOperand = addName(std::move(variantName));
    emit(std::move(instruction));
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

IRRegister IRProgram::emitLoadVar(
    std::string name,
    std::optional<BindingId> bindingId)
{
    IRRegister dest = makeRegister();
    IRInstruction instruction{
        IROp::LoadVar,
        dest,
        std::nullopt,
        std::nullopt,
        {},
        addName(std::move(name))};
    instruction.bindingId = bindingId;
    emit(std::move(instruction));
    return dest;
}

void IRProgram::emitStoreVar(
    std::string name,
    IRRegister value,
    std::optional<BindingId> bindingId)
{
    IRInstruction instruction{
        IROp::StoreVar,
        std::nullopt,
        value,
        std::nullopt,
        {},
        addName(std::move(name))};
    instruction.bindingId = bindingId;
    emit(std::move(instruction));
}

void IRProgram::emitAssignVar(
    std::string name,
    IRRegister value,
    std::optional<BindingId> bindingId)
{
    IRInstruction instruction{
        IROp::AssignVar,
        std::nullopt,
        value,
        std::nullopt,
        {},
        addName(std::move(name))};
    instruction.bindingId = bindingId;
    emit(std::move(instruction));
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

IRRegister IRProgram::emitField(
    IRRegister object,
    std::string fieldName,
    std::optional<std::string> structTypeName)
{
    IRRegister dest = makeRegister();
    IRInstruction instruction{
        IROp::Field, dest, object, std::nullopt, {}, addName(std::move(fieldName))};
    if (structTypeName) {
        instruction.typeNameOperand = addName(std::move(*structTypeName));
    }
    emit(std::move(instruction));
    return dest;
}

IRRegister IRProgram::emitAssignField(
    IRRegister object,
    std::string fieldName,
    std::optional<std::string> structTypeName,
    IRRegister value)
{
    IRRegister dest = makeRegister();
    IRInstruction instruction{
        IROp::AssignField, dest, object, std::nullopt, {value}, addName(std::move(fieldName))};
    if (structTypeName) {
        instruction.typeNameOperand = addName(std::move(*structTypeName));
    }
    emit(std::move(instruction));
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

const std::vector<IRBinding>& IRProgram::bindings() const
{
    return bindings_;
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

IRProgram IRProgram::rebuildWithStreams(
    std::vector<IRInstruction> instructions,
    std::size_t registerCount,
    std::vector<IRFunction> functions,
    std::vector<IRModuleDependency> moduleDependencies) const
{
    if (!functionStack_.empty()) {
        throw std::logic_error("cannot rebuild an IR program while a function is active");
    }

    validateCanonicalBindings(bindings_);
    validateInstructionStream(
        instructions,
        registerCount,
        constants_,
        names_,
        functions.size(),
        bindings_,
        "main IR stream");
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const IRFunction& function = functions[index];
        validateInstructionStream(
            function.instructions,
            function.registerCount,
            constants_,
            names_,
            functions.size(),
            bindings_,
            "IR function " + std::to_string(index));
    }
    validateFunctionBindings(functions, bindings_);
    for (const IRModuleDependency& dependency : moduleDependencies) {
        if (dependency.instructionOffset > instructions.size()) {
            throw std::logic_error("IR module dependency offset is outside the main stream");
        }
    }

    IRProgram rebuilt = *this;
    rebuilt.instructions_ = std::move(instructions);
    rebuilt.registerCount_ = registerCount;
    rebuilt.functions_ = std::move(functions);
    rebuilt.moduleDependencies_ = std::move(moduleDependencies);
    return rebuilt;
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
