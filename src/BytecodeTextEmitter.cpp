#include "BytecodeTextEmitter.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string escapedString(const std::string& value)
{
    std::ostringstream out;
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
    return out.str();
}

std::string numberText(double value)
{
    std::ostringstream out;
    out << std::setprecision(15) << value;
    return out.str();
}

std::string constantText(const Value& value)
{
    switch (value.type()) {
    case Value::Type::Nil:
        return "nil";
    case Value::Type::Number:
        return "number " + numberText(value.asNumber());
    case Value::Type::Bool:
        return std::string("bool ") + (value.asBool() ? "true" : "false");
    case Value::Type::String:
        return "string " + escapedString(value.asString());
    case Value::Type::Function:
        throw std::runtime_error("cannot emit function value as bytecode constant");
    case Value::Type::Array:
        throw std::runtime_error("cannot emit array value as bytecode constant");
    case Value::Type::Map:
        throw std::runtime_error("cannot emit map value as bytecode constant");
    case Value::Type::Range:
        throw std::runtime_error("cannot emit range value as bytecode constant");
    case Value::Type::Struct:
        throw std::runtime_error("cannot emit struct value as bytecode constant");
    case Value::Type::Variant:
        throw std::runtime_error("cannot emit enum variant as bytecode constant");
    }
    throw std::runtime_error("unsupported bytecode constant");
}

std::string reg(BytecodeRegister reg)
{
    return "r" + std::to_string(reg.index);
}

std::string constantRef(std::uint32_t index)
{
    return "c" + std::to_string(index);
}

std::string nameRef(std::uint32_t index)
{
    return "n" + std::to_string(index);
}

std::string functionRef(std::uint32_t index)
{
    return "f" + std::to_string(index);
}

void writeRegisterList(std::ostream& out, const std::vector<BytecodeRegister>& registers)
{
    out << '[';
    for (std::size_t i = 0; i < registers.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << reg(registers[i]);
    }
    out << ']';
}

BytecodeRegister requireDest(const BytecodeInstruction& instruction)
{
    if (!instruction.dest) {
        throw std::runtime_error("bytecode instruction missing destination");
    }
    return *instruction.dest;
}

BytecodeRegister requireLeft(const BytecodeInstruction& instruction)
{
    if (!instruction.left) {
        throw std::runtime_error("bytecode instruction missing left operand");
    }
    return *instruction.left;
}

BytecodeRegister requireRight(const BytecodeInstruction& instruction)
{
    if (!instruction.right) {
        throw std::runtime_error("bytecode instruction missing right operand");
    }
    return *instruction.right;
}

void writeInstruction(std::ostream& out, const BytecodeInstruction& instruction)
{
    out << "  ";
    switch (instruction.op) {
    case BytecodeOp::Constant:
        out << reg(requireDest(instruction)) << " = constant " << constantRef(instruction.operand);
        break;
    case BytecodeOp::MakeFunction:
        out << reg(requireDest(instruction)) << " = make_function " << functionRef(instruction.operand);
        break;
    case BytecodeOp::Array:
        out << reg(requireDest(instruction)) << " = array ";
        writeRegisterList(out, instruction.arguments);
        break;
    case BytecodeOp::Map:
        if (instruction.arguments.size() % 2 != 0) {
            throw std::runtime_error("map expects key/value register pairs");
        }
        out << reg(requireDest(instruction)) << " = map [";
        for (std::size_t i = 0; i < instruction.arguments.size(); i += 2) {
            if (i != 0) {
                out << ", ";
            }
            out << reg(instruction.arguments[i]) << ": " << reg(instruction.arguments[i + 1]);
        }
        out << "]";
        break;
    case BytecodeOp::MakeStruct:
        out << reg(requireDest(instruction)) << " = make_struct t" << instruction.operand << " ";
        writeRegisterList(out, instruction.arguments);
        break;
    case BytecodeOp::Field:
        out << reg(requireDest(instruction)) << " = field "
            << reg(requireLeft(instruction)) << ", " << nameRef(instruction.operand);
        break;
    case BytecodeOp::AssignField:
        out << reg(requireDest(instruction)) << " = assign_field "
            << reg(requireLeft(instruction)) << ", " << nameRef(instruction.operand)
            << ", " << reg(instruction.arguments.empty() ? BytecodeRegister{0} : instruction.arguments.front());
        break;
    case BytecodeOp::StructGet:
        out << reg(requireDest(instruction)) << " = struct_get "
            << reg(requireLeft(instruction)) << ", t" << instruction.operand
            << ", " << (instruction.operands.empty() ? 0 : instruction.operands.front());
        break;
    case BytecodeOp::StructSet:
        out << reg(requireDest(instruction)) << " = struct_set "
            << reg(requireLeft(instruction)) << ", t" << instruction.operand
            << ", " << (instruction.operands.empty() ? 0 : instruction.operands.front())
            << ", " << reg(instruction.arguments.empty() ? BytecodeRegister{0} : instruction.arguments.front());
        break;
    case BytecodeOp::MakeVariant:
        out << reg(requireDest(instruction)) << " = make_variant t" << instruction.operand
            << ", v" << instruction.variantNameOperand.value_or(0) << " ";
        writeRegisterList(out, instruction.arguments);
        break;
    case BytecodeOp::IsVariant:
        out << reg(requireDest(instruction)) << " = is_variant "
            << reg(requireLeft(instruction)) << ", t" << instruction.operand
            << ", v" << instruction.variantNameOperand.value_or(0);
        break;
    case BytecodeOp::VariantGet:
        out << reg(requireDest(instruction)) << " = variant_get "
            << reg(requireLeft(instruction)) << ", t" << instruction.typeNameOperand.value_or(0)
            << ", v" << instruction.variantNameOperand.value_or(0)
            << ", " << instruction.operand;
        break;
    case BytecodeOp::Move:
        out << reg(requireDest(instruction)) << " = move " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::LoadVar:
        out << reg(requireDest(instruction)) << " = load_var " << nameRef(instruction.operand);
        break;
    case BytecodeOp::StoreVar:
        out << "store_var " << nameRef(instruction.operand) << ", " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::AssignVar:
        out << "assign_var " << nameRef(instruction.operand) << ", " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::LoadLocal:
        out << reg(requireDest(instruction)) << " = load_local l" << instruction.operand;
        break;
    case BytecodeOp::BindLocal:
        out << "bind_local l" << instruction.operand << ", " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::SetLocal:
        out << "set_local l" << instruction.operand << ", " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::LoadUpvalue:
        out << reg(requireDest(instruction)) << " = load_upvalue u" << instruction.operand;
        break;
    case BytecodeOp::SetUpvalue:
        out << "set_upvalue u" << instruction.operand << ", " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::LoadGlobal:
        out << reg(requireDest(instruction)) << " = load_global g" << instruction.operand;
        break;
    case BytecodeOp::InitGlobal:
        out << "init_global g" << instruction.operand << ", " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::SetGlobal:
        out << "set_global g" << instruction.operand << ", " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::Call:
        out << reg(requireDest(instruction)) << " = call " << reg(requireLeft(instruction)) << ' ';
        writeRegisterList(out, instruction.arguments);
        break;
    case BytecodeOp::CallNative:
        out << reg(requireDest(instruction)) << " = call_native i" << instruction.operand << ' ';
        writeRegisterList(out, instruction.arguments);
        break;
    case BytecodeOp::Index:
        out << reg(requireDest(instruction)) << " = index " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::AssignIndex:
        if (instruction.arguments.size() != 1) {
            throw std::runtime_error("assign_index expects one value operand");
        }
        out << reg(requireDest(instruction)) << " = assign_index " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction)) << ", " << reg(instruction.arguments.front());
        break;
    case BytecodeOp::Len:
        out << reg(requireDest(instruction)) << " = len " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::AssertArray:
        out << reg(requireDest(instruction)) << " = assert_array " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::AssertNumber:
        out << reg(requireDest(instruction)) << " = assert_number " << reg(requireLeft(instruction)) << ", " << nameRef(instruction.operand);
        break;
    case BytecodeOp::Return:
        out << "return " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::Negate:
        out << reg(requireDest(instruction)) << " = negate " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::Not:
        out << reg(requireDest(instruction)) << " = not " << reg(requireLeft(instruction));
        break;
    case BytecodeOp::Add:
        out << reg(requireDest(instruction)) << " = add " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::Subtract:
        out << reg(requireDest(instruction)) << " = subtract " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::Multiply:
        out << reg(requireDest(instruction)) << " = multiply " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::Divide:
        out << reg(requireDest(instruction)) << " = divide " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::Equal:
        out << reg(requireDest(instruction)) << " = equal " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::NotEqual:
        out << reg(requireDest(instruction)) << " = not_equal " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::Greater:
        out << reg(requireDest(instruction)) << " = greater " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::GreaterEqual:
        out << reg(requireDest(instruction)) << " = greater_equal " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::Less:
        out << reg(requireDest(instruction)) << " = less " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::LessEqual:
        out << reg(requireDest(instruction)) << " = less_equal " << reg(requireLeft(instruction)) << ", " << reg(requireRight(instruction));
        break;
    case BytecodeOp::Jump:
        out << "jump " << instruction.operand;
        break;
    case BytecodeOp::JumpIfFalse:
        out << "jump_if_false " << reg(requireLeft(instruction)) << ", " << instruction.operand;
        break;
    case BytecodeOp::JumpIfTrue:
        out << "jump_if_true " << reg(requireLeft(instruction)) << ", " << instruction.operand;
        break;
    case BytecodeOp::Br:
        out << "br b" << instruction.operand;
        break;
    case BytecodeOp::BrIf:
        out << "br_if " << reg(requireLeft(instruction)) << ", b" << instruction.operand
            << ", b" << (instruction.operands.empty() ? 0 : instruction.operands.front());
        break;
    case BytecodeOp::ReturnNil:
        out << "return_nil";
        break;
    }
    out << '\n';
}

void writeInstructions(std::ostream& out, const std::vector<BytecodeInstruction>& instructions)
{
    for (const BytecodeInstruction& instruction : instructions) {
        if (instruction.op == BytecodeOp::BlockStart) {
            out << "block b" << instruction.operand << ":\n";
            continue;
        }
        writeInstruction(out, instruction);
    }
}

bool hasDebugLocations(const BytecodeProgram& program)
{
    for (const BytecodeInstruction& instruction : program.instructions()) {
        if (instruction.span) {
            return true;
        }
    }
    for (const BytecodeFunction& function : program.functions()) {
        for (const BytecodeInstruction& instruction : function.instructions) {
            if (instruction.span) {
                return true;
            }
        }
    }
    return false;
}

bool hasDebugRanges(const BytecodeProgram& program)
{
    for (const BytecodeInstruction& instruction : program.instructions()) {
        if (instruction.span && instruction.span->range) {
            return true;
        }
    }
    for (const BytecodeFunction& function : program.functions()) {
        for (const BytecodeInstruction& instruction : function.instructions) {
            if (instruction.span && instruction.span->range) {
                return true;
            }
        }
    }
    return false;
}

void writeDebugLocation(std::ostream& out, const std::string& section, std::size_t index, const SourceSpan& span)
{
    out << "  " << section << ' ' << index << " = s" << span.source << ':' << span.line << ':' << span.column << '\n';
}

void writeDebugRange(std::ostream& out, const std::string& section, std::size_t index, const SourceSpan& span)
{
    if (!span.range) {
        return;
    }
    out << "  " << section << ' ' << index << " = s" << span.source << ':'
        << span.range->start << ':' << span.range->end << '\n';
}

const char* moduleDependencyKindName(ModuleGraphEdgeKind kind)
{
    return kind == ModuleGraphEdgeKind::Import ? "import" : "re_export";
}

void writeBytecodeSections(
    std::ostream& out,
    const BytecodeProgram& program,
    bool emitGlobals)
{
    out << "constants:\n";
    for (std::size_t i = 0; i < program.constants().size(); ++i) {
        out << "  " << constantRef(static_cast<std::uint32_t>(i)) << " = " << constantText(program.constants()[i]) << '\n';
    }

    out << "\nnames:\n";
    for (std::size_t i = 0; i < program.names().size(); ++i) {
        out << "  " << nameRef(static_cast<std::uint32_t>(i)) << " = " << escapedString(program.names()[i]) << '\n';
    }

    if (emitGlobals && !program.globals().empty()) {
        out << "\nglobals:\n";
        for (std::size_t i = 0; i < program.globals().size(); ++i) {
            out << "  g" << i << " = " << nameRef(program.globals()[i]) << '\n';
        }
    }

    if (!program.types().empty()) {
        out << "\ntypes:\n";
        for (std::size_t index = 0; index < program.types().size(); ++index) {
            const BytecodeType& type = program.types()[index];
            out << "  t" << index << " = " << (type.isEnum ? "enum " : "struct ")
                << escapedString(type.name);
            if (type.isEnum) {
                for (std::size_t variant = 0; variant < type.variants.size(); ++variant) {
                    out << " v" << variant << "=" << escapedString(type.variants[variant].name)
                        << " payload=" << type.variants[variant].payloadCount;
                }
            } else {
                for (std::size_t field = 0; field < type.fieldNames.size(); ++field) {
                    out << " field" << field << "=" << escapedString(type.fieldNames[field]);
                }
            }
            out << '\n';
        }
    }

    if (!program.nativeImports().empty()) {
        out << "\nnative_imports:\n";
        for (std::size_t index = 0; index < program.nativeImports().size(); ++index) {
            const BytecodeNativeImport& import = program.nativeImports()[index];
            out << "  i" << index << " = " << escapedString(import.name)
                << " abi=" << import.abiVersion << '\n';
        }
    }

    out << "\nmain registers=" << program.registerCount() << ":\n";
    writeInstructions(out, program.instructions());

    for (std::size_t i = 0; i < program.functions().size(); ++i) {
        const BytecodeFunction& function = program.functions()[i];
        out << "\nfunction " << functionRef(static_cast<std::uint32_t>(i))
            << " name=" << escapedString(function.name)
            << " arity=" << function.parameters.size()
            << " registers=" << function.registerCount << ":\n";
        for (std::size_t parameter = 0; parameter < function.parameters.size(); ++parameter) {
            out << "  param " << parameter << " = " << escapedString(function.parameters[parameter]) << '\n';
        }
        for (std::size_t upvalue = 0; upvalue < function.upvalues.size(); ++upvalue) {
            const BytecodeUpvalue& descriptor = function.upvalues[upvalue];
            const char* source = descriptor.source == BytecodeUpvalueSource::Local
                ? "local l"
                : descriptor.source == BytecodeUpvalueSource::Upvalue ? "upvalue u" : "global g";
            out << "  upvalue u" << upvalue << " = " << source << descriptor.index << '\n';
        }
        writeInstructions(out, function.instructions);
    }

    if (!program.sources().empty()) {
        out << "\ndebug_sources:\n";
        for (std::size_t index = 0; index < program.sources().size(); ++index) {
            const SourceFile& source = program.sources()[index];
            out << "  s" << index;
            if (source.moduleIdentity) {
                out << " module=" << escapedString(*source.moduleIdentity);
            }
            out << " path=" << escapedString(source.path)
                << " text=" << escapedString(source.text)
                << '\n';
        }
    }

    if (hasDebugLocations(program)) {
        out << "\ndebug_locations:\n";
        for (std::size_t index = 0; index < program.instructions().size(); ++index) {
            const auto& instruction = program.instructions()[index];
            if (instruction.span) {
                writeDebugLocation(out, "main", index, *instruction.span);
            }
        }
        for (std::size_t functionIndex = 0; functionIndex < program.functions().size(); ++functionIndex) {
            const BytecodeFunction& function = program.functions()[functionIndex];
            for (std::size_t instructionIndex = 0; instructionIndex < function.instructions.size(); ++instructionIndex) {
                const auto& instruction = function.instructions[instructionIndex];
                if (instruction.span) {
                    writeDebugLocation(out, "function f" + std::to_string(functionIndex), instructionIndex, *instruction.span);
                }
            }
        }
    }

    if (hasDebugRanges(program)) {
        out << "\ndebug_ranges:\n";
        for (std::size_t index = 0; index < program.instructions().size(); ++index) {
            const auto& instruction = program.instructions()[index];
            if (instruction.span) {
                writeDebugRange(out, "main", index, *instruction.span);
            }
        }
        for (std::size_t functionIndex = 0; functionIndex < program.functions().size(); ++functionIndex) {
            const BytecodeFunction& function = program.functions()[functionIndex];
            for (std::size_t instructionIndex = 0; instructionIndex < function.instructions.size(); ++instructionIndex) {
                const auto& instruction = function.instructions[instructionIndex];
                if (instruction.span) {
                    writeDebugRange(out, "function f" + std::to_string(functionIndex), instructionIndex, *instruction.span);
                }
            }
        }
    }
}

} // namespace

void writeBytecodeText(std::ostream& out, const BytecodeProgram& program)
{
    out << "cdbc 0.2\n\n";
    writeBytecodeSections(out, program, true);
}

void writeBytecodeModuleText(std::ostream& out, const BytecodeModuleArtifact& artifact)
{
    out << "cdbc 0.2\n\n"
        << "artifact: module\n\n"
        << "module:\n"
        << "  identity = " << escapedString(artifact.identity) << '\n'
        << "  path = " << escapedString(artifact.path) << '\n'
        << "  canonical_path = " << escapedString(artifact.canonicalPath) << '\n'
        << "  entry = " << (artifact.isEntry ? "true" : "false") << '\n';
    if (artifact.entryOrder) {
        out << "  entry_order = " << *artifact.entryOrder << '\n';
    }
    out << "  dependencies:\n";
    for (std::size_t index = 0; index < artifact.dependencies.size(); ++index) {
        const BytecodeModuleDependency& dependency = artifact.dependencies[index];
        out << "    d" << index
            << " target=" << escapedString(dependency.moduleIdentity)
            << " kind=" << moduleDependencyKindName(dependency.kind)
            << " at=" << dependency.instructionOffset
            << " requested=" << escapedString(dependency.requestedPath)
            << '\n';
    }
    out << '\n';
    writeBytecodeSections(out, artifact.program, true);
}
