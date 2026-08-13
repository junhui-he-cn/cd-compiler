#include "ModuleInterfaceArtifact.hpp"

#include "ModuleCache.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::size_t kMaxCollectionSize = 100000;
constexpr std::size_t kMaxStringSize = 1U << 20;
constexpr std::size_t kMaxTypeDepth = 128;

const char* dependencyKindName(ModuleGraphEdgeKind kind)
{
    return kind == ModuleGraphEdgeKind::Import ? "import" : "re_export";
}

std::optional<ModuleGraphEdgeKind> parseDependencyKind(const std::string& value)
{
    if (value == "import") {
        return ModuleGraphEdgeKind::Import;
    }
    if (value == "re_export") {
        return ModuleGraphEdgeKind::ReExport;
    }
    return std::nullopt;
}

class CodecWriter {
public:
    void number(std::size_t value)
    {
        output_ << value << ';';
    }

    void boolean(bool value)
    {
        number(value ? 1 : 0);
    }

    void string(const std::string& value)
    {
        output_ << value.size() << ':' << value;
    }

    std::string str() const
    {
        return output_.str();
    }

private:
    std::ostringstream output_;
};

class CodecReader {
public:
    explicit CodecReader(const std::string& input)
        : input_(input)
    {
    }

    std::size_t number()
    {
        const std::size_t start = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (position_ == start || position_ >= input_.size() || input_[position_] != ';') {
            throw std::runtime_error("invalid length-coded number");
        }
        const std::string text = input_.substr(start, position_ - start);
        ++position_;
        try {
            const unsigned long long parsed = std::stoull(text);
            if (parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("length-coded number is out of range");
            }
            return static_cast<std::size_t>(parsed);
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("invalid length-coded number");
        } catch (const std::out_of_range&) {
            throw std::runtime_error("length-coded number is out of range");
        }
    }

    bool boolean()
    {
        const std::size_t value = number();
        if (value > 1) {
            throw std::runtime_error("invalid length-coded boolean");
        }
        return value != 0;
    }

    std::string string()
    {
        const std::size_t length = unsignedLength();
        if (length > kMaxStringSize || length > input_.size() - position_) {
            throw std::runtime_error("length-coded string is too large");
        }
        std::string value = input_.substr(position_, length);
        position_ += length;
        return value;
    }

    bool atEnd() const
    {
        return position_ == input_.size();
    }

private:
    std::size_t unsignedLength()
    {
        const std::size_t start = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
            ++position_;
        }
        if (position_ == start || position_ >= input_.size() || input_[position_] != ':') {
            throw std::runtime_error("invalid length-coded string");
        }
        const std::string text = input_.substr(start, position_ - start);
        ++position_;
        try {
            const unsigned long long parsed = std::stoull(text);
            if (parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("length-coded string is out of range");
            }
            return static_cast<std::size_t>(parsed);
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("invalid length-coded string");
        } catch (const std::out_of_range&) {
            throw std::runtime_error("length-coded string is out of range");
        }
    }

    const std::string& input_;
    std::size_t position_ = 0;
};

void encodeType(CodecWriter& writer, const TypeInfo& type, std::size_t depth = 0)
{
    if (depth > kMaxTypeDepth) {
        throw std::runtime_error("TypeInfo nesting exceeds sidecar limit");
    }
    writer.number(static_cast<std::size_t>(type.kind));

    writer.number(type.parameterTypes.size());
    for (const TypeInfo& parameter : type.parameterTypes) {
        encodeType(writer, parameter, depth + 1);
    }

    writer.boolean(type.returnType != nullptr);
    if (type.returnType) {
        encodeType(writer, *type.returnType, depth + 1);
    }

    writer.boolean(type.structName.has_value());
    if (type.structName) {
        writer.string(*type.structName);
    }
    writer.boolean(type.enumName.has_value());
    if (type.enumName) {
        writer.string(*type.enumName);
    }

    writer.number(type.typeArguments.size());
    for (const TypeInfo& argument : type.typeArguments) {
        encodeType(writer, argument, depth + 1);
    }

    writer.boolean(type.elementType != nullptr);
    if (type.elementType) {
        encodeType(writer, *type.elementType, depth + 1);
    }
    writer.boolean(type.keyType != nullptr);
    if (type.keyType) {
        encodeType(writer, *type.keyType, depth + 1);
    }
    writer.boolean(type.valueType != nullptr);
    if (type.valueType) {
        encodeType(writer, *type.valueType, depth + 1);
    }
    writer.boolean(type.nullableOf != nullptr);
    if (type.nullableOf) {
        encodeType(writer, *type.nullableOf, depth + 1);
    }

    writer.boolean(type.typeParameterName.has_value());
    if (type.typeParameterName) {
        writer.string(*type.typeParameterName);
    }
    writer.boolean(type.typeParameterConstraint != nullptr);
    if (type.typeParameterConstraint) {
        encodeType(writer, *type.typeParameterConstraint, depth + 1);
    }

    writer.number(type.genericParameters.size());
    for (const std::string& parameter : type.genericParameters) {
        writer.string(parameter);
    }
    writer.number(type.genericParameterConstraints.size());
    for (const std::shared_ptr<TypeInfo>& constraint : type.genericParameterConstraints) {
        writer.boolean(constraint != nullptr);
        if (constraint) {
            encodeType(writer, *constraint, depth + 1);
        }
    }
}

std::size_t readCollectionSize(CodecReader& reader, const char* field)
{
    const std::size_t size = reader.number();
    if (size > kMaxCollectionSize) {
        throw std::runtime_error(std::string(field) + " is too large");
    }
    return size;
}

TypeInfo decodeType(CodecReader& reader, std::size_t depth = 0)
{
    if (depth > kMaxTypeDepth) {
        throw std::runtime_error("TypeInfo nesting exceeds sidecar limit");
    }
    const std::size_t kindValue = reader.number();
    if (kindValue > static_cast<std::size_t>(StaticType::Capability)) {
        throw std::runtime_error("invalid TypeInfo kind");
    }

    TypeInfo type;
    type.kind = static_cast<StaticType>(kindValue);

    const std::size_t parameterCount = readCollectionSize(reader, "parameter types");
    type.parameterTypes.reserve(parameterCount);
    for (std::size_t index = 0; index < parameterCount; ++index) {
        type.parameterTypes.push_back(decodeType(reader, depth + 1));
    }

    if (reader.boolean()) {
        type.returnType = std::make_shared<TypeInfo>(decodeType(reader, depth + 1));
    }

    if (reader.boolean()) {
        type.structName = reader.string();
    }
    if (reader.boolean()) {
        type.enumName = reader.string();
    }

    const std::size_t typeArgumentCount = readCollectionSize(reader, "type arguments");
    type.typeArguments.reserve(typeArgumentCount);
    for (std::size_t index = 0; index < typeArgumentCount; ++index) {
        type.typeArguments.push_back(decodeType(reader, depth + 1));
    }

    if (reader.boolean()) {
        type.elementType = std::make_shared<TypeInfo>(decodeType(reader, depth + 1));
    }
    if (reader.boolean()) {
        type.keyType = std::make_shared<TypeInfo>(decodeType(reader, depth + 1));
    }
    if (reader.boolean()) {
        type.valueType = std::make_shared<TypeInfo>(decodeType(reader, depth + 1));
    }
    if (reader.boolean()) {
        type.nullableOf = std::make_shared<TypeInfo>(decodeType(reader, depth + 1));
    }

    if (reader.boolean()) {
        type.typeParameterName = reader.string();
    }
    if (reader.boolean()) {
        type.typeParameterConstraint = std::make_shared<TypeInfo>(decodeType(reader, depth + 1));
    }

    const std::size_t genericParameterCount = readCollectionSize(reader, "generic parameters");
    type.genericParameters.reserve(genericParameterCount);
    for (std::size_t index = 0; index < genericParameterCount; ++index) {
        type.genericParameters.push_back(reader.string());
    }
    const std::size_t genericConstraintCount = readCollectionSize(reader, "generic constraints");
    type.genericParameterConstraints.reserve(genericConstraintCount);
    for (std::size_t index = 0; index < genericConstraintCount; ++index) {
        if (reader.boolean()) {
            type.genericParameterConstraints.push_back(
                std::make_shared<TypeInfo>(decodeType(reader, depth + 1)));
        } else {
            type.genericParameterConstraints.push_back(nullptr);
        }
    }
    return type;
}

void encodeOptionalTypes(
    CodecWriter& writer,
    const std::vector<std::shared_ptr<TypeInfo>>& constraints)
{
    writer.number(constraints.size());
    for (const std::shared_ptr<TypeInfo>& constraint : constraints) {
        writer.boolean(constraint != nullptr);
        if (constraint) {
            encodeType(writer, *constraint);
        }
    }
}

void encodeStringVector(CodecWriter& writer, const std::vector<std::string>& values)
{
    writer.number(values.size());
    for (const std::string& value : values) {
        writer.string(value);
    }
}

void encodeInterfaceBody(CodecWriter& writer, const ModuleInterface& interfaceInfo)
{
    writer.number(interfaceInfo.values.size());
    for (const ModuleInterfaceValue& value : interfaceInfo.values) {
        writer.string(value.name);
        writer.string(value.resolvedName);
        encodeType(writer, value.type);
    }

    writer.number(interfaceInfo.structs.size());
    for (const ModuleInterfaceStruct& structure : interfaceInfo.structs) {
        writer.string(structure.name);
        encodeStringVector(writer, structure.genericParameters);
        encodeOptionalTypes(writer, structure.genericParameterConstraints);
        writer.boolean(structure.hasPrivateFields);

        writer.number(structure.fields.size());
        for (const ModuleInterfaceField& field : structure.fields) {
            writer.string(field.name);
            encodeType(writer, field.type);
        }

        writer.number(structure.methods.size());
        for (const ModuleInterfaceMethod& method : structure.methods) {
            writer.string(method.name);
            writer.number(method.parameterTypes.size());
            for (const TypeInfo& parameter : method.parameterTypes) {
                encodeType(writer, parameter);
            }
            encodeType(writer, method.returnType);
            encodeStringVector(writer, method.genericParameters);
            encodeOptionalTypes(writer, method.genericParameterConstraints);
            encodeType(writer, method.receiverType);
            writer.string(method.resolvedName);
        }

    }

    writer.number(interfaceInfo.enums.size());
    for (const ModuleInterfaceEnum& enumeration : interfaceInfo.enums) {
        writer.string(enumeration.name);
        encodeStringVector(writer, enumeration.genericParameters);
        encodeOptionalTypes(writer, enumeration.genericParameterConstraints);
        writer.number(enumeration.variants.size());
        for (const ModuleInterfaceVariant& variant : enumeration.variants) {
            writer.string(variant.name);
            writer.number(variant.payloadTypes.size());
            for (std::size_t index = 0; index < variant.payloadTypes.size(); ++index) {
                const std::optional<std::string> payloadName
                    = index < variant.payloadNames.size() ? variant.payloadNames[index] : std::nullopt;
                writer.boolean(payloadName.has_value());
                if (payloadName) {
                    writer.string(*payloadName);
                }
                encodeType(writer, variant.payloadTypes[index]);
            }
        }
    }
}

template <typename T, typename Getter>
void sortByName(std::vector<T>& values, Getter getter)
{
    std::sort(values.begin(), values.end(), [&](const T& left, const T& right) {
        return getter(left) < getter(right);
    });
}

ModuleInterface canonicalInterface(ModuleInterface interfaceInfo)
{
    sortByName(interfaceInfo.values, [](const ModuleInterfaceValue& value) { return value.name; });
    sortByName(interfaceInfo.structs, [](const ModuleInterfaceStruct& value) { return value.name; });
    for (ModuleInterfaceStruct& structure : interfaceInfo.structs) {
        sortByName(structure.methods, [](const ModuleInterfaceMethod& value) { return value.name; });
    }
    sortByName(interfaceInfo.enums, [](const ModuleInterfaceEnum& value) { return value.name; });
    return interfaceInfo;
}

bool strictlySortedByName(const std::vector<ModuleInterfaceValue>& values)
{
    return std::adjacent_find(
               values.begin(),
               values.end(),
               [](const ModuleInterfaceValue& left, const ModuleInterfaceValue& right) {
                   return left.name >= right.name;
               })
        == values.end();
}

bool strictlySortedByName(const std::vector<ModuleInterfaceStruct>& values)
{
    return std::adjacent_find(
               values.begin(),
               values.end(),
               [](const ModuleInterfaceStruct& left, const ModuleInterfaceStruct& right) {
                   return left.name >= right.name;
               })
        == values.end();
}

bool strictlySortedByName(const std::vector<ModuleInterfaceEnum>& values)
{
    return std::adjacent_find(
               values.begin(),
               values.end(),
               [](const ModuleInterfaceEnum& left, const ModuleInterfaceEnum& right) {
                   return left.name >= right.name;
               })
        == values.end();
}

bool strictlySortedByName(const std::vector<ModuleInterfaceMethod>& values)
{
    return std::adjacent_find(
               values.begin(),
               values.end(),
               [](const ModuleInterfaceMethod& left, const ModuleInterfaceMethod& right) {
                   return left.name >= right.name;
               })
        == values.end();
}

bool sameTypeShape(const TypeInfo& left, const TypeInfo& right)
{
    return typeInfoName(left) == typeInfoName(right);
}

bool sameOptionalTypeShape(
    const std::vector<std::shared_ptr<TypeInfo>>& left,
    const std::vector<std::shared_ptr<TypeInfo>>& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (static_cast<bool>(left[index]) != static_cast<bool>(right[index])) {
            return false;
        }
        if (left[index] && !sameTypeShape(*left[index], *right[index])) {
            return false;
        }
    }
    return true;
}

std::string quotedString(const std::string& value)
{
    if (value.find('\n') != std::string::npos || value.find('\r') != std::string::npos) {
        throw std::runtime_error("module interface sidecar strings cannot contain newlines");
    }
    std::ostringstream output;
    output << std::quoted(value);
    return output.str();
}

std::string parseQuoted(const std::string& line, const std::string& prefix)
{
    if (line.rfind(prefix, 0) != 0) {
        throw std::runtime_error("expected `" + prefix + "...`");
    }
    std::istringstream input(line.substr(prefix.size()));
    std::string value;
    if (!(input >> std::quoted(value))) {
        throw std::runtime_error("invalid quoted sidecar field");
    }
    input >> std::ws;
    if (!input.eof()) {
        throw std::runtime_error("trailing data in quoted sidecar field");
    }
    return value;
}

std::size_t parseNumber(const std::string& line, const std::string& prefix)
{
    if (line.rfind(prefix, 0) != 0) {
        throw std::runtime_error("expected `" + prefix + "...`");
    }
    const std::string value = line.substr(prefix.size());
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
        throw std::runtime_error("invalid numeric sidecar field");
    }
    try {
        const unsigned long long parsed = std::stoull(value);
        if (parsed > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("numeric sidecar field is out of range");
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("invalid numeric sidecar field");
    } catch (const std::out_of_range&) {
        throw std::runtime_error("numeric sidecar field is out of range");
    }
}

bool parseBoolean(const std::string& line, const std::string& prefix)
{
    if (line == prefix + "true") {
        return true;
    }
    if (line == prefix + "false") {
        return false;
    }
    throw std::runtime_error("invalid boolean sidecar field");
}

class LineReader {
public:
    explicit LineReader(const std::string& source)
    {
        std::istringstream input(source);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines_.push_back(std::move(line));
        }
    }

    std::string next()
    {
        while (position_ < lines_.size() && lines_[position_].empty()) {
            ++position_;
        }
        if (position_ == lines_.size()) {
            throw std::runtime_error("unexpected end of sidecar");
        }
        return lines_[position_++];
    }

    void expect(const std::string& value)
    {
        const std::string actual = next();
        if (actual != value) {
            throw std::runtime_error("expected `" + value + "`");
        }
    }

    bool done()
    {
        while (position_ < lines_.size() && lines_[position_].empty()) {
            ++position_;
        }
        return position_ == lines_.size();
    }

private:
    std::vector<std::string> lines_;
    std::size_t position_ = 0;
};

std::string typeField(const TypeInfo& type)
{
    CodecWriter writer;
    encodeType(writer, type);
    return writer.str();
}

TypeInfo parseTypeField(const std::string& line, const std::string& prefix)
{
    if (line.rfind(prefix, 0) != 0) {
        throw std::runtime_error("expected TypeInfo sidecar field");
    }
    const std::string encoded = line.substr(prefix.size());
    CodecReader reader(encoded);
    TypeInfo type = decodeType(reader);
    if (!reader.atEnd()) {
        throw std::runtime_error("trailing TypeInfo data in sidecar");
    }
    return type;
}

void writeTypeField(std::ostream& out, const std::string& prefix, const TypeInfo& type)
{
    out << prefix << typeField(type) << '\n';
}

void writeStringVector(std::ostream& out, const std::string& prefix, const std::vector<std::string>& values)
{
    out << prefix << values.size() << '\n';
    for (std::size_t index = 0; index < values.size(); ++index) {
        out << "    generic_parameter " << index << " = " << quotedString(values[index]) << '\n';
    }
}

void writeOptionalTypes(
    std::ostream& out,
    const std::string& prefix,
    const std::vector<std::shared_ptr<TypeInfo>>& values)
{
    out << prefix << values.size() << '\n';
    for (std::size_t index = 0; index < values.size(); ++index) {
        out << "    generic_constraint " << index;
        if (!values[index]) {
            out << " = none\n";
        } else {
            out << '\n';
            writeTypeField(out, "      type = ", *values[index]);
        }
    }
}

void parseIndexedHeader(const std::string& line, const std::string& prefix, std::size_t expected)
{
    if (parseNumber(line, prefix) != expected) {
        throw std::runtime_error("sidecar records must be ordered from zero");
    }
}

void parseStringVector(LineReader& lines, const std::string& prefix, std::vector<std::string>& values)
{
    const std::size_t count = parseNumber(lines.next(), prefix);
    if (count > kMaxCollectionSize) {
        throw std::runtime_error("sidecar string vector is too large");
    }
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::string line = lines.next();
        const std::string recordPrefix = "    generic_parameter " + std::to_string(index) + " = ";
        values.push_back(parseQuoted(line, recordPrefix));
    }
}

void parseOptionalTypes(
    LineReader& lines,
    const std::string& prefix,
    std::vector<std::shared_ptr<TypeInfo>>& values)
{
    const std::size_t count = parseNumber(lines.next(), prefix);
    if (count > kMaxCollectionSize) {
        throw std::runtime_error("sidecar generic constraints are too large");
    }
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::string line = lines.next();
        const std::string recordPrefix = "    generic_constraint " + std::to_string(index);
        if (line == recordPrefix + " = none") {
            values.push_back(nullptr);
            continue;
        }
        if (line != recordPrefix) {
            throw std::runtime_error("invalid generic constraint record");
        }
        values.push_back(std::make_shared<TypeInfo>(parseTypeField(lines.next(), "      type = ")));
    }
}

void validateArtifact(const ModuleInterfaceArtifact& artifact)
{
    if (artifact.identity.empty() || artifact.identity != artifact.canonicalPath
        || artifact.path.empty() || artifact.sourceHash.empty()) {
        throw std::runtime_error("module interface sidecar has invalid identity metadata");
    }
    if (artifact.interfaceHash != moduleInterfaceArtifactHash(artifact.interfaceInfo)) {
        throw std::runtime_error("module interface sidecar public hash does not match its body");
    }
    if (artifact.interfaceInfo.path != artifact.path
        || artifact.interfaceInfo.canonicalPath != artifact.canonicalPath
        || artifact.interfaceInfo.isEntry != artifact.isEntry
        || artifact.interfaceInfo.resolvedNameNext != artifact.resolvedNameNext) {
        throw std::runtime_error("module interface sidecar metadata disagrees with its interface");
    }
    if (artifact.isEntry != artifact.entryOrder.has_value()) {
        throw std::runtime_error("module interface sidecar entry metadata is inconsistent");
    }
    if (!strictlySortedByName(artifact.interfaceInfo.values)
        || !strictlySortedByName(artifact.interfaceInfo.structs)
        || !strictlySortedByName(artifact.interfaceInfo.enums)) {
        throw std::runtime_error("module interface sidecar exports are not canonically ordered");
    }
    for (const ModuleInterfaceStruct& structure : artifact.interfaceInfo.structs) {
        if (!strictlySortedByName(structure.methods)) {
            throw std::runtime_error("module interface sidecar methods are not canonically ordered");
        }
        if (structure.genericParameters.size() != structure.genericParameterConstraints.size()) {
            throw std::runtime_error("module interface sidecar struct generic metadata is inconsistent");
        }
        for (const ModuleInterfaceMethod& method : structure.methods) {
            if (method.genericParameters.size() != method.genericParameterConstraints.size()) {
                throw std::runtime_error("module interface sidecar method generic metadata is inconsistent");
            }
        }
    }
    for (const ModuleInterfaceEnum& enumeration : artifact.interfaceInfo.enums) {
        if (enumeration.genericParameters.size() != enumeration.genericParameterConstraints.size()) {
            throw std::runtime_error("module interface sidecar enum generic metadata is inconsistent");
        }
    }
    if (artifact.interfaceInfo.dependencies.size() != artifact.dependencies.size()) {
        throw std::runtime_error("module interface sidecar dependency metadata is inconsistent");
    }
    // Repeated imports are legal graph edges; preserve their source order and
    // do not impose a uniqueness constraint on the serialized dependency list.
    for (std::size_t index = 0; index < artifact.dependencies.size(); ++index) {
        const ModuleInterfaceArtifactDependency& dependency = artifact.dependencies[index];
        const ModuleInterfaceDependency& interfaceDependency = artifact.interfaceInfo.dependencies[index];
        if (dependency.identity.empty() || dependency.interfaceHash.empty()
            || interfaceDependency.kind != dependency.kind
            || interfaceDependency.requestedPath != dependency.requestedPath) {
            throw std::runtime_error("module interface sidecar has invalid dependency metadata");
        }
    }
}

void writeInterfaceBody(std::ostream& out, const ModuleInterface& source)
{
    ModuleInterface interfaceInfo = canonicalInterface(source);
    out << "values = " << interfaceInfo.values.size() << '\n';
    for (std::size_t index = 0; index < interfaceInfo.values.size(); ++index) {
        const ModuleInterfaceValue& value = interfaceInfo.values[index];
        out << "value " << index << '\n'
            << "  name = " << quotedString(value.name) << '\n'
            << "  resolved = " << quotedString(value.resolvedName) << '\n';
        writeTypeField(out, "  type = ", value.type);
    }

    out << "structs = " << interfaceInfo.structs.size() << '\n';
    for (std::size_t index = 0; index < interfaceInfo.structs.size(); ++index) {
        const ModuleInterfaceStruct& structure = interfaceInfo.structs[index];
        out << "struct " << index << '\n'
            << "  name = " << quotedString(structure.name) << '\n';
        writeStringVector(out, "  generic_parameters = ", structure.genericParameters);
        writeOptionalTypes(out, "  generic_constraints = ", structure.genericParameterConstraints);
        out << "  private_fields = " << (structure.hasPrivateFields ? "true" : "false") << '\n';
        out << "  fields = " << structure.fields.size() << '\n';
        for (std::size_t fieldIndex = 0; fieldIndex < structure.fields.size(); ++fieldIndex) {
            const ModuleInterfaceField& field = structure.fields[fieldIndex];
            out << "  field " << fieldIndex << '\n'
                << "    name = " << quotedString(field.name) << '\n';
            writeTypeField(out, "    type = ", field.type);
        }
        out << "  methods = " << structure.methods.size() << '\n';
        for (std::size_t methodIndex = 0; methodIndex < structure.methods.size(); ++methodIndex) {
            const ModuleInterfaceMethod& method = structure.methods[methodIndex];
            out << "  method " << methodIndex << '\n'
                << "    name = " << quotedString(method.name) << '\n'
                << "    parameters = " << method.parameterTypes.size() << '\n';
            for (std::size_t parameterIndex = 0; parameterIndex < method.parameterTypes.size(); ++parameterIndex) {
                writeTypeField(out, "      parameter " + std::to_string(parameterIndex) + " = ", method.parameterTypes[parameterIndex]);
            }
            writeTypeField(out, "    return = ", method.returnType);
            writeStringVector(out, "    generic_parameters = ", method.genericParameters);
            writeOptionalTypes(out, "    generic_constraints = ", method.genericParameterConstraints);
            writeTypeField(out, "    receiver = ", method.receiverType);
            out << "    resolved = " << quotedString(method.resolvedName) << '\n';
        }
    }

    out << "enums = " << interfaceInfo.enums.size() << '\n';
    for (std::size_t index = 0; index < interfaceInfo.enums.size(); ++index) {
        const ModuleInterfaceEnum& enumeration = interfaceInfo.enums[index];
        out << "enum " << index << '\n'
            << "  name = " << quotedString(enumeration.name) << '\n';
        writeStringVector(out, "  generic_parameters = ", enumeration.genericParameters);
        writeOptionalTypes(out, "  generic_constraints = ", enumeration.genericParameterConstraints);
        out << "  variants = " << enumeration.variants.size() << '\n';
        for (std::size_t variantIndex = 0; variantIndex < enumeration.variants.size(); ++variantIndex) {
            const ModuleInterfaceVariant& variant = enumeration.variants[variantIndex];
            out << "  variant " << variantIndex << '\n'
                << "    name = " << quotedString(variant.name) << '\n'
                << "    payloads = " << variant.payloadTypes.size() << '\n';
            for (std::size_t payloadIndex = 0; payloadIndex < variant.payloadTypes.size(); ++payloadIndex) {
                const std::optional<std::string> payloadName
                    = payloadIndex < variant.payloadNames.size() ? variant.payloadNames[payloadIndex] : std::nullopt;
                out << "      payload " << payloadIndex << "\n"
                    << "        name = " << (payloadName ? quotedString(*payloadName) : "none") << '\n';
                writeTypeField(out, "        type = ", variant.payloadTypes[payloadIndex]);
            }
        }
    }
}

ModuleInterface parseInterfaceBody(LineReader& lines)
{
    ModuleInterface interfaceInfo;
    const std::size_t valueCount = parseNumber(lines.next(), "values = ");
    if (valueCount > kMaxCollectionSize) {
        throw std::runtime_error("sidecar values are too large");
    }
    interfaceInfo.values.reserve(valueCount);
    for (std::size_t index = 0; index < valueCount; ++index) {
        parseIndexedHeader(lines.next(), "value ", index);
        ModuleInterfaceValue value;
        value.name = parseQuoted(lines.next(), "  name = ");
        value.resolvedName = parseQuoted(lines.next(), "  resolved = ");
        value.type = parseTypeField(lines.next(), "  type = ");
        interfaceInfo.values.push_back(std::move(value));
    }

    const std::size_t structCount = parseNumber(lines.next(), "structs = ");
    if (structCount > kMaxCollectionSize) {
        throw std::runtime_error("sidecar structs are too large");
    }
    interfaceInfo.structs.reserve(structCount);
    for (std::size_t index = 0; index < structCount; ++index) {
        parseIndexedHeader(lines.next(), "struct ", index);
        ModuleInterfaceStruct structure;
        structure.name = parseQuoted(lines.next(), "  name = ");
        parseStringVector(lines, "  generic_parameters = ", structure.genericParameters);
        parseOptionalTypes(lines, "  generic_constraints = ", structure.genericParameterConstraints);
        structure.hasPrivateFields = parseBoolean(lines.next(), "  private_fields = ");

        const std::size_t fieldCount = parseNumber(lines.next(), "  fields = ");
        if (fieldCount > kMaxCollectionSize) {
            throw std::runtime_error("sidecar struct fields are too large");
        }
        structure.fields.reserve(fieldCount);
        for (std::size_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
            parseIndexedHeader(lines.next(), "  field ", fieldIndex);
            ModuleInterfaceField field;
            field.name = parseQuoted(lines.next(), "    name = ");
            field.type = parseTypeField(lines.next(), "    type = ");
            structure.fields.push_back(std::move(field));
        }

        const std::size_t methodCount = parseNumber(lines.next(), "  methods = ");
        if (methodCount > kMaxCollectionSize) {
            throw std::runtime_error("sidecar struct methods are too large");
        }
        structure.methods.reserve(methodCount);
        for (std::size_t methodIndex = 0; methodIndex < methodCount; ++methodIndex) {
            parseIndexedHeader(lines.next(), "  method ", methodIndex);
            ModuleInterfaceMethod method;
            method.name = parseQuoted(lines.next(), "    name = ");
            const std::size_t parameterCount = parseNumber(lines.next(), "    parameters = ");
            if (parameterCount > kMaxCollectionSize) {
                throw std::runtime_error("sidecar method parameters are too large");
            }
            method.parameterTypes.reserve(parameterCount);
            for (std::size_t parameterIndex = 0; parameterIndex < parameterCount; ++parameterIndex) {
                method.parameterTypes.push_back(parseTypeField(
                    lines.next(),
                    "      parameter " + std::to_string(parameterIndex) + " = "));
            }
            method.returnType = parseTypeField(lines.next(), "    return = ");
            parseStringVector(lines, "    generic_parameters = ", method.genericParameters);
            parseOptionalTypes(lines, "    generic_constraints = ", method.genericParameterConstraints);
            method.receiverType = parseTypeField(lines.next(), "    receiver = ");
            method.resolvedName = parseQuoted(lines.next(), "    resolved = ");
            structure.methods.push_back(std::move(method));
        }

        interfaceInfo.structs.push_back(std::move(structure));
    }

    const std::size_t enumCount = parseNumber(lines.next(), "enums = ");
    if (enumCount > kMaxCollectionSize) {
        throw std::runtime_error("sidecar enums are too large");
    }
    interfaceInfo.enums.reserve(enumCount);
    for (std::size_t index = 0; index < enumCount; ++index) {
        parseIndexedHeader(lines.next(), "enum ", index);
        ModuleInterfaceEnum enumeration;
        enumeration.name = parseQuoted(lines.next(), "  name = ");
        parseStringVector(lines, "  generic_parameters = ", enumeration.genericParameters);
        parseOptionalTypes(lines, "  generic_constraints = ", enumeration.genericParameterConstraints);
        const std::size_t variantCount = parseNumber(lines.next(), "  variants = ");
        if (variantCount > kMaxCollectionSize) {
            throw std::runtime_error("sidecar enum variants are too large");
        }
        enumeration.variants.reserve(variantCount);
        for (std::size_t variantIndex = 0; variantIndex < variantCount; ++variantIndex) {
            parseIndexedHeader(lines.next(), "  variant ", variantIndex);
            ModuleInterfaceVariant variant;
            variant.name = parseQuoted(lines.next(), "    name = ");
            const std::size_t payloadCount = parseNumber(lines.next(), "    payloads = ");
            if (payloadCount > kMaxCollectionSize) {
                throw std::runtime_error("sidecar variant payloads are too large");
            }
            variant.payloadTypes.reserve(payloadCount);
            variant.payloadNames.reserve(payloadCount);
            for (std::size_t payloadIndex = 0; payloadIndex < payloadCount; ++payloadIndex) {
                parseIndexedHeader(lines.next(), "      payload ", payloadIndex);
                const std::string nameLine = lines.next();
                if (nameLine == "        name = none") {
                    variant.payloadNames.push_back(std::nullopt);
                } else {
                    variant.payloadNames.push_back(parseQuoted(nameLine, "        name = "));
                }
                variant.payloadTypes.push_back(parseTypeField(lines.next(), "        type = "));
            }
            enumeration.variants.push_back(std::move(variant));
        }
        interfaceInfo.enums.push_back(std::move(enumeration));
    }
    return interfaceInfo;
}

void writeArtifactBody(std::ostream& out, const ModuleInterfaceArtifact& source)
{
    ModuleInterfaceArtifact artifact = source;
    if (artifact.resolvedNameNext == 0) {
        artifact.resolvedNameNext = artifact.interfaceInfo.resolvedNameNext;
    }
    artifact.interfaceInfo.resolvedNameNext = artifact.resolvedNameNext;
    artifact.interfaceInfo = canonicalInterface(std::move(artifact.interfaceInfo));
    artifact.interfaceHash = moduleInterfaceArtifactHash(artifact.interfaceInfo);
    artifact.interfaceInfo.path = artifact.path;
    artifact.interfaceInfo.canonicalPath = artifact.canonicalPath;
    artifact.interfaceInfo.isEntry = artifact.isEntry;
    artifact.interfaceInfo.dependencies.clear();
    for (const ModuleInterfaceArtifactDependency& dependency : artifact.dependencies) {
        artifact.interfaceInfo.dependencies.push_back(ModuleInterfaceDependency{
            0,
            dependency.kind,
            dependency.requestedPath});
    }

    out << "cdi 0.1\n\n"
        << "identity = " << quotedString(artifact.identity) << '\n'
        << "path = " << quotedString(artifact.path) << '\n'
        << "canonical_path = " << quotedString(artifact.canonicalPath) << '\n'
        << "source = " << quotedString(artifact.sourceHash) << '\n'
        << "interface = " << quotedString(artifact.interfaceHash) << '\n'
        << "entry = " << (artifact.isEntry ? "true" : "false") << '\n'
        << "entry_order = " << (artifact.entryOrder ? std::to_string(*artifact.entryOrder) : "none") << '\n'
        << "resolved_name_next = " << artifact.resolvedNameNext << '\n'
        << "dependencies = " << artifact.dependencies.size() << '\n';
    for (std::size_t index = 0; index < artifact.dependencies.size(); ++index) {
        const ModuleInterfaceArtifactDependency& dependency = artifact.dependencies[index];
        out << "dependency " << index << '\n'
            << "  identity = " << quotedString(dependency.identity) << '\n'
            << "  kind = " << dependencyKindName(dependency.kind) << '\n'
            << "  requested = " << quotedString(dependency.requestedPath) << '\n'
            << "  interface = " << quotedString(dependency.interfaceHash) << '\n';
    }
    out << "interface:\n";
    writeInterfaceBody(out, artifact.interfaceInfo);
    out << "end\n";
}

ModuleInterfaceArtifact parseArtifactBody(const std::string& source)
{
    LineReader lines(source);
    lines.expect("cdi 0.1");

    ModuleInterfaceArtifact artifact;
    artifact.identity = parseQuoted(lines.next(), "identity = ");
    artifact.path = parseQuoted(lines.next(), "path = ");
    artifact.canonicalPath = parseQuoted(lines.next(), "canonical_path = ");
    artifact.sourceHash = parseQuoted(lines.next(), "source = ");
    artifact.interfaceHash = parseQuoted(lines.next(), "interface = ");
    artifact.isEntry = parseBoolean(lines.next(), "entry = ");
    const std::string entryOrder = lines.next();
    if (entryOrder == "entry_order = none") {
        artifact.entryOrder.reset();
    } else {
        artifact.entryOrder = parseNumber(entryOrder, "entry_order = ");
    }

    std::string dependencyLine = lines.next();
    if (dependencyLine.rfind("resolved_name_next = ", 0) == 0) {
        artifact.resolvedNameNext = parseNumber(dependencyLine, "resolved_name_next = ");
        dependencyLine = lines.next();
    }
    const std::size_t dependencyCount = parseNumber(dependencyLine, "dependencies = ");
    if (dependencyCount > kMaxCollectionSize) {
        throw std::runtime_error("sidecar dependencies are too large");
    }
    artifact.dependencies.reserve(dependencyCount);
    for (std::size_t index = 0; index < dependencyCount; ++index) {
        parseIndexedHeader(lines.next(), "dependency ", index);
        ModuleInterfaceArtifactDependency dependency;
        dependency.identity = parseQuoted(lines.next(), "  identity = ");
        const std::string kind = lines.next();
        if (kind.rfind("  kind = ", 0) != 0) {
            throw std::runtime_error("invalid sidecar dependency kind");
        }
        const auto parsedKind = parseDependencyKind(kind.substr(9));
        if (!parsedKind) {
            throw std::runtime_error("invalid sidecar dependency kind");
        }
        dependency.kind = *parsedKind;
        dependency.requestedPath = parseQuoted(lines.next(), "  requested = ");
        dependency.interfaceHash = parseQuoted(lines.next(), "  interface = ");
        artifact.dependencies.push_back(std::move(dependency));
    }

    lines.expect("interface:");
    artifact.interfaceInfo = parseInterfaceBody(lines);
    lines.expect("end");
    if (!lines.done()) {
        throw std::runtime_error("trailing records after sidecar end");
    }
    artifact.interfaceInfo.path = artifact.path;
    artifact.interfaceInfo.canonicalPath = artifact.canonicalPath;
    artifact.interfaceInfo.isEntry = artifact.isEntry;
    artifact.interfaceInfo.resolvedNameNext = artifact.resolvedNameNext;
    artifact.interfaceInfo.dependencies.clear();
    for (const ModuleInterfaceArtifactDependency& dependency : artifact.dependencies) {
        artifact.interfaceInfo.dependencies.push_back(ModuleInterfaceDependency{
            0,
            dependency.kind,
            dependency.requestedPath});
    }
    validateArtifact(artifact);
    return artifact;
}

} // namespace

std::string moduleInterfaceArtifactHash(const ModuleInterface& interfaceInfo)
{
    CodecWriter writer;
    encodeInterfaceBody(writer, canonicalInterface(interfaceInfo));
    return moduleCacheHash("module-interface-shape-v1\0" + writer.str());
}

std::string moduleInterfaceArtifactFileName(const std::string& identity)
{
    return "interface-" + moduleCacheHash("module-interface-identity-v1\0" + identity) + ".cdi";
}

std::filesystem::path moduleInterfaceArtifactPath(
    const std::filesystem::path& cacheDirectory,
    const std::string& identity)
{
    return cacheDirectory / "interfaces" / moduleInterfaceArtifactFileName(identity);
}

void writeModuleInterfaceArtifactText(
    std::ostream& out,
    const ModuleInterfaceArtifact& artifact)
{
    ModuleInterfaceArtifact normalized = artifact;
    if (normalized.resolvedNameNext == 0) {
        normalized.resolvedNameNext = normalized.interfaceInfo.resolvedNameNext;
    }
    normalized.interfaceInfo.resolvedNameNext = normalized.resolvedNameNext;
    normalized.interfaceInfo = canonicalInterface(std::move(normalized.interfaceInfo));
    normalized.interfaceInfo.path = normalized.path;
    normalized.interfaceInfo.canonicalPath = normalized.canonicalPath;
    normalized.interfaceInfo.isEntry = normalized.isEntry;
    normalized.interfaceInfo.dependencies.clear();
    for (const ModuleInterfaceArtifactDependency& dependency : normalized.dependencies) {
        normalized.interfaceInfo.dependencies.push_back(ModuleInterfaceDependency{
            0,
            dependency.kind,
            dependency.requestedPath});
    }
    normalized.interfaceHash = moduleInterfaceArtifactHash(normalized.interfaceInfo);
    validateArtifact(normalized);
    writeArtifactBody(out, normalized);
    if (!out) {
        throw std::runtime_error("failed to write module interface sidecar");
    }
}

ModuleInterfaceArtifactLoadResult readModuleInterfaceArtifactText(const std::string& source)
{
    ModuleInterfaceArtifactLoadResult result;
    result.found = true;
    try {
        result.artifact = parseArtifactBody(source);
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}

void writeModuleInterfaceArtifact(
    const std::filesystem::path& path,
    const ModuleInterfaceArtifact& artifact)
{
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            throw std::runtime_error("failed to create module interface directory: " + error.message());
        }
    }
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("failed to open module interface sidecar: " + path.string());
    }
    writeModuleInterfaceArtifactText(output, artifact);
    if (!output) {
        throw std::runtime_error("failed to write module interface sidecar: " + path.string());
    }
}

ModuleInterfaceArtifactLoadResult readModuleInterfaceArtifact(
    const std::filesystem::path& path)
{
    ModuleInterfaceArtifactLoadResult result;
    std::ifstream input(path);
    if (!input) {
        std::error_code error;
        result.found = std::filesystem::exists(path, error);
        if (result.found) {
            result.error = "failed to read module interface sidecar";
        }
        return result;
    }
    std::ostringstream source;
    source << input.rdbuf();
    if (!input) {
        result.found = true;
        result.error = "failed to read module interface sidecar";
        return result;
    }
    return readModuleInterfaceArtifactText(source.str());
}
