#include "LanguageServer.hpp"

#include "DeclarationIndex.hpp"
#include "Diagnostic.hpp"
#include "Formatter.hpp"
#include "FrontendSession.hpp"
#include "Lexer.hpp"
#include "SourceMap.hpp"
#include "TypeChecker.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct JsonValue {
    enum class Kind {
        Null,
        Boolean,
        Number,
        String,
        Object,
        Array,
    };

    using Object = std::map<std::string, JsonValue>;

    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text;
    Object members;
    std::vector<JsonValue> elements;

    static JsonValue null()
    {
        return {};
    }

    static JsonValue booleanValue(bool value)
    {
        JsonValue result;
        result.kind = Kind::Boolean;
        result.boolean = value;
        return result;
    }

    static JsonValue number(std::string value)
    {
        JsonValue result;
        result.kind = Kind::Number;
        result.text = std::move(value);
        return result;
    }

    static JsonValue string(std::string value)
    {
        JsonValue result;
        result.kind = Kind::String;
        result.text = std::move(value);
        return result;
    }

    static JsonValue object(Object value)
    {
        JsonValue result;
        result.kind = Kind::Object;
        result.members = std::move(value);
        return result;
    }

    static JsonValue array(std::vector<JsonValue> value)
    {
        JsonValue result;
        result.kind = Kind::Array;
        result.elements = std::move(value);
        return result;
    }
};

JsonValue makeObject(std::initializer_list<std::pair<std::string, JsonValue>> fields)
{
    JsonValue::Object members;
    for (const auto& field : fields) {
        members.emplace(field.first, field.second);
    }
    return JsonValue::object(std::move(members));
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input)
        : input_(input)
    {
    }

    JsonValue parse()
    {
        skipWhitespace();
        JsonValue value = parseValue();
        skipWhitespace();
        if (offset_ != input_.size()) {
            throw std::runtime_error("trailing data after JSON message");
        }
        return value;
    }

private:
    void skipWhitespace()
    {
        while (offset_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[offset_]);
            if (!std::isspace(character)) {
                break;
            }
            ++offset_;
        }
    }

    char peek() const
    {
        return offset_ < input_.size() ? input_[offset_] : '\0';
    }

    char consume()
    {
        if (offset_ >= input_.size()) {
            throw std::runtime_error("unexpected end of JSON message");
        }
        return input_[offset_++];
    }

    void expect(char expected)
    {
        if (consume() != expected) {
            throw std::runtime_error("invalid JSON message");
        }
    }

    JsonValue parseValue()
    {
        switch (peek()) {
        case 'n':
            consumeLiteral("null");
            return JsonValue::null();
        case 't':
            consumeLiteral("true");
            return JsonValue::booleanValue(true);
        case 'f':
            consumeLiteral("false");
            return JsonValue::booleanValue(false);
        case '"':
            return JsonValue::string(parseString());
        case '{':
            return parseObject();
        case '[':
            return parseArray();
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                return JsonValue::number(parseNumber());
            }
            throw std::runtime_error("invalid JSON value");
        }
    }

    void consumeLiteral(std::string_view literal)
    {
        for (const char expected : literal) {
            if (consume() != expected) {
                throw std::runtime_error("invalid JSON literal");
            }
        }
    }

    static int hexValue(char character)
    {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    }

    static void appendCodePoint(std::string& output, std::uint32_t codePoint)
    {
        if (codePoint <= 0x7f) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else if (codePoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
    }

    std::uint32_t parseUnicodeEscape()
    {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = hexValue(consume());
            if (digit < 0) {
                throw std::runtime_error("invalid JSON unicode escape");
            }
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    std::string parseString()
    {
        expect('"');
        std::string result;
        while (offset_ < input_.size()) {
            const char character = consume();
            if (character == '"') {
                return result;
            }
            if (character == '\\') {
                switch (const char escaped = consume()) {
                case '"':
                case '\\':
                case '/':
                    result.push_back(escaped);
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u': {
                    const std::uint32_t first = parseUnicodeEscape();
                    if (first >= 0xd800 && first <= 0xdbff
                        && offset_ + 6 <= input_.size()
                        && input_[offset_] == '\\'
                        && input_[offset_ + 1] == 'u') {
                        offset_ += 2;
                        const std::uint32_t second = parseUnicodeEscape();
                        if (second < 0xdc00 || second > 0xdfff) {
                            throw std::runtime_error("invalid JSON surrogate pair");
                        }
                        appendCodePoint(
                            result,
                            0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00));
                    } else {
                        appendCodePoint(result, first);
                    }
                    break;
                }
                default:
                    throw std::runtime_error("invalid JSON escape");
                }
                continue;
            }
            if (static_cast<unsigned char>(character) < 0x20) {
                throw std::runtime_error("control character in JSON string");
            }
            result.push_back(character);
        }
        throw std::runtime_error("unterminated JSON string");
    }

    std::string parseNumber()
    {
        const std::size_t start = offset_;
        if (peek() == '-') {
            ++offset_;
        }
        if (peek() == '0') {
            ++offset_;
        } else {
            if (peek() < '1' || peek() > '9') {
                throw std::runtime_error("invalid JSON number");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++offset_;
            }
        }
        if (peek() == '.') {
            ++offset_;
            if (peek() < '0' || peek() > '9') {
                throw std::runtime_error("invalid JSON number fraction");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++offset_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++offset_;
            if (peek() == '+' || peek() == '-') {
                ++offset_;
            }
            if (peek() < '0' || peek() > '9') {
                throw std::runtime_error("invalid JSON number exponent");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++offset_;
            }
        }
        return std::string(input_.substr(start, offset_ - start));
    }

    JsonValue parseObject()
    {
        expect('{');
        skipWhitespace();
        JsonValue::Object members;
        if (peek() == '}') {
            consume();
            return JsonValue::object(std::move(members));
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                throw std::runtime_error("JSON object key must be a string");
            }
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            members[std::move(key)] = parseValue();
            skipWhitespace();
            if (peek() == '}') {
                consume();
                return JsonValue::object(std::move(members));
            }
            expect(',');
        }
    }

    JsonValue parseArray()
    {
        expect('[');
        skipWhitespace();
        std::vector<JsonValue> elements;
        if (peek() == ']') {
            consume();
            return JsonValue::array(std::move(elements));
        }
        while (true) {
            skipWhitespace();
            elements.push_back(parseValue());
            skipWhitespace();
            if (peek() == ']') {
                consume();
                return JsonValue::array(std::move(elements));
            }
            expect(',');
        }
    }

    std::string_view input_;
    std::size_t offset_ = 0;
};

std::string escapeJsonString(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (character < 0x20) {
                static constexpr char digits[] = "0123456789abcdef";
                result += "\\u000";
                result.push_back(digits[character >> 4]);
                result.push_back(digits[character & 0x0f]);
            } else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string serializeJson(const JsonValue& value)
{
    switch (value.kind) {
    case JsonValue::Kind::Null:
        return "null";
    case JsonValue::Kind::Boolean:
        return value.boolean ? "true" : "false";
    case JsonValue::Kind::Number:
        return value.text;
    case JsonValue::Kind::String:
        return escapeJsonString(value.text);
    case JsonValue::Kind::Object: {
        std::string result = "{";
        bool first = true;
        for (const auto& member : value.members) {
            if (!first) {
                result.push_back(',');
            }
            first = false;
            result += escapeJsonString(member.first);
            result.push_back(':');
            result += serializeJson(member.second);
        }
        result.push_back('}');
        return result;
    }
    case JsonValue::Kind::Array: {
        std::string result = "[";
        for (std::size_t index = 0; index < value.elements.size(); ++index) {
            if (index != 0) {
                result.push_back(',');
            }
            result += serializeJson(value.elements[index]);
        }
        result.push_back(']');
        return result;
    }
    }
    return "null";
}

const JsonValue* member(const JsonValue& value, std::string_view name)
{
    if (value.kind != JsonValue::Kind::Object) {
        return nullptr;
    }
    const auto found = value.members.find(std::string(name));
    return found == value.members.end() ? nullptr : &found->second;
}

const JsonValue* memberObject(const JsonValue& value, std::string_view name)
{
    const JsonValue* result = member(value, name);
    return result && result->kind == JsonValue::Kind::Object ? result : nullptr;
}

std::optional<std::string> stringMember(const JsonValue& value, std::string_view name)
{
    const JsonValue* result = member(value, name);
    if (!result || result->kind != JsonValue::Kind::String) {
        return std::nullopt;
    }
    return result->text;
}

std::optional<std::string> readMessage(std::istream& input)
{
    std::size_t contentLength = 0;
    bool foundContentLength = false;
    bool readHeader = false;
    std::string line;
    while (std::getline(input, line)) {
        readHeader = true;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        const std::size_t separator = line.find(':');
        if (separator == std::string::npos) {
            throw std::runtime_error("invalid language-server header");
        }
        std::string name = line.substr(0, separator);
        std::transform(
            name.begin(),
            name.end(),
            name.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        if (name != "content-length") {
            continue;
        }
        const std::string value = line.substr(separator + 1);
        std::size_t first = 0;
        while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
            ++first;
        }
        if (first == value.size()) {
            throw std::runtime_error("empty Content-Length header");
        }
        std::size_t parsedCharacters = 0;
        unsigned long long parsedLength = 0;
        try {
            parsedLength = std::stoull(value.substr(first), &parsedCharacters);
        } catch (const std::exception&) {
            throw std::runtime_error("invalid Content-Length header");
        }
        if (parsedCharacters != value.size() - first
            || parsedLength > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("invalid Content-Length header");
        }
        contentLength = static_cast<std::size_t>(parsedLength);
        foundContentLength = true;
    }

    if (!readHeader) {
        return std::nullopt;
    }
    if (!foundContentLength) {
        throw std::runtime_error("language-server message omitted Content-Length");
    }

    std::string body(contentLength, '\0');
    input.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (input.gcount() != static_cast<std::streamsize>(body.size())) {
        throw std::runtime_error("truncated language-server message");
    }
    return body;
}

void writeMessage(std::ostream& output, const JsonValue& value)
{
    const std::string body = serializeJson(value);
    output << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
}

struct LspPosition {
    std::size_t line = 0;
    std::size_t character = 0;
};

enum class ReferenceSiteKind {
    Variable,
    Assignment,
    CompoundAssignment,
    FieldAccess,
    MemberCall,
};

struct ReferenceSite {
    ReferenceSiteKind kind = ReferenceSiteKind::Variable;
    const Expr* expression = nullptr;
    std::optional<SourceRange> range;
};

class ReferenceSiteCollector {
public:
    std::vector<ReferenceSite> collect(const Program& program)
    {
        for (const StmtPtr& statement : program.statements) {
            visitStatement(statement.get());
        }
        return std::move(sites_);
    }

private:
    void visitStatement(const Stmt* statement)
    {
        if (!statement) {
            return;
        }
        if (const auto* module = dynamic_cast<const ModuleStmt*>(statement)) {
            for (const StmtPtr& child : module->statements) {
                visitStatement(child.get());
            }
            return;
        }
        if (const auto* function = dynamic_cast<const FunctionStmt*>(statement)) {
            for (const StmtPtr& child : function->body) {
                visitStatement(child.get());
            }
            return;
        }
        if (const auto* methodOwner = dynamic_cast<const ImplStmt*>(statement)) {
            for (const MethodDecl& method : methodOwner->methods) {
                for (const StmtPtr& child : method.body) {
                    visitStatement(child.get());
                }
            }
            return;
        }
        if (const auto* let = dynamic_cast<const LetStmt*>(statement)) {
            visitExpression(let->initializer.get());
            return;
        }
        if (const auto* expression = dynamic_cast<const ExpressionStmt*>(statement)) {
            visitExpression(expression->expression.get());
            return;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(statement)) {
            for (const StmtPtr& child : block->statements) {
                visitStatement(child.get());
            }
            return;
        }
        if (const auto* ifStatement = dynamic_cast<const IfStmt*>(statement)) {
            visitExpression(ifStatement->condition.get());
            visitStatement(ifStatement->thenBranch.get());
            visitStatement(ifStatement->elseBranch.get());
            return;
        }
        if (const auto* ifLet = dynamic_cast<const IfLetStmt*>(statement)) {
            visitExpression(ifLet->value.get());
            visitStatement(ifLet->thenBranch.get());
            visitStatement(ifLet->elseBranch.get());
            return;
        }
        if (const auto* whileStatement = dynamic_cast<const WhileStmt*>(statement)) {
            visitExpression(whileStatement->condition.get());
            visitStatement(whileStatement->body.get());
            return;
        }
        if (const auto* whileLet = dynamic_cast<const WhileLetStmt*>(statement)) {
            visitExpression(whileLet->value.get());
            visitStatement(whileLet->body.get());
            return;
        }
        if (const auto* forStatement = dynamic_cast<const ForStmt*>(statement)) {
            visitStatement(forStatement->initializer.get());
            visitExpression(forStatement->condition.get());
            visitExpression(forStatement->increment.get());
            visitStatement(forStatement->body.get());
            return;
        }
        if (const auto* forIn = dynamic_cast<const ForInStmt*>(statement)) {
            visitExpression(forIn->iterable.get());
            visitStatement(forIn->body.get());
            return;
        }
        if (const auto* returnStatement = dynamic_cast<const ReturnStmt*>(statement)) {
            visitExpression(returnStatement->value.get());
        }
    }

    void addReference(
        ReferenceSiteKind kind,
        const Expr& expression,
        const std::optional<SourceRange>& range)
    {
        if (!range) {
            return;
        }
        sites_.push_back(ReferenceSite{kind, &expression, range});
    }

    void visitExpression(const Expr* expression)
    {
        if (!expression) {
            return;
        }
        if (const auto* variable = dynamic_cast<const VariableExpr*>(expression)) {
            addReference(ReferenceSiteKind::Variable, *variable, variable->name.range);
            return;
        }
        if (const auto* assign = dynamic_cast<const AssignExpr*>(expression)) {
            addReference(ReferenceSiteKind::Assignment, *assign, assign->name.range);
            visitExpression(assign->value.get());
            return;
        }
        if (const auto* compound = dynamic_cast<const CompoundAssignExpr*>(expression)) {
            addReference(
                ReferenceSiteKind::CompoundAssignment,
                *compound,
                compound->name.range);
            visitExpression(compound->value.get());
            return;
        }
        if (const auto* indexAssign = dynamic_cast<const IndexAssignExpr*>(expression)) {
            visitExpression(indexAssign->collection.get());
            visitExpression(indexAssign->index.get());
            visitExpression(indexAssign->value.get());
            return;
        }
        if (const auto* indexCompound = dynamic_cast<const IndexCompoundAssignExpr*>(expression)) {
            visitExpression(indexCompound->collection.get());
            visitExpression(indexCompound->index.get());
            visitExpression(indexCompound->value.get());
            return;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression)) {
            visitExpression(unary->right.get());
            return;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression)) {
            visitExpression(binary->left.get());
            visitExpression(binary->right.get());
            return;
        }
        if (const auto* logical = dynamic_cast<const LogicalExpr*>(expression)) {
            visitExpression(logical->left.get());
            visitExpression(logical->right.get());
            return;
        }
        if (const auto* match = dynamic_cast<const MatchExpr*>(expression)) {
            visitExpression(match->value.get());
            for (const MatchArm& arm : match->arms) {
                visitExpression(arm.guard.get());
                visitStatement(arm.body.get());
                visitExpression(arm.expression.get());
            }
            return;
        }
        if (const auto* coalesce = dynamic_cast<const CoalesceExpr*>(expression)) {
            visitExpression(coalesce->left.get());
            visitExpression(coalesce->right.get());
            return;
        }
        if (const auto* unwrap = dynamic_cast<const UnwrapOrReturnExpr*>(expression)) {
            visitExpression(unwrap->value.get());
            return;
        }
        if (const auto* grouping = dynamic_cast<const GroupingExpr*>(expression)) {
            visitExpression(grouping->expression.get());
            return;
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
            visitExpression(call->callee.get());
            for (const ExprPtr& argument : call->arguments) {
                visitExpression(argument.get());
            }
            return;
        }
        if (const auto* memberCall = dynamic_cast<const MemberCallExpr*>(expression)) {
            addReference(ReferenceSiteKind::MemberCall, *memberCall, memberCall->name.range);
            visitExpression(memberCall->receiver.get());
            for (const ExprPtr& argument : memberCall->arguments) {
                visitExpression(argument.get());
            }
            return;
        }
        if (const auto* array = dynamic_cast<const ArrayExpr*>(expression)) {
            for (const ExprPtr& element : array->elements) {
                visitExpression(element.get());
            }
            return;
        }
        if (const auto* map = dynamic_cast<const MapExpr*>(expression)) {
            for (const MapEntry& entry : map->entries) {
                visitExpression(entry.key.get());
                visitExpression(entry.value.get());
            }
            return;
        }
        if (const auto* construct = dynamic_cast<const StructConstructExpr*>(expression)) {
            for (const StructField& field : construct->fields) {
                visitExpression(field.value.get());
            }
            return;
        }
        if (const auto* index = dynamic_cast<const IndexExpr*>(expression)) {
            visitExpression(index->collection.get());
            visitExpression(index->index.get());
            return;
        }
        if (const auto* field = dynamic_cast<const FieldAccessExpr*>(expression)) {
            addReference(ReferenceSiteKind::FieldAccess, *field, field->name.range);
            visitExpression(field->object.get());
            return;
        }
        if (const auto* fieldAssign = dynamic_cast<const FieldAssignExpr*>(expression)) {
            visitExpression(fieldAssign->object.get());
            visitExpression(fieldAssign->value.get());
            return;
        }
        if (const auto* fieldCompound = dynamic_cast<const FieldCompoundAssignExpr*>(expression)) {
            visitExpression(fieldCompound->object.get());
            visitExpression(fieldCompound->value.get());
            return;
        }
        if (const auto* function = dynamic_cast<const FunctionExpr*>(expression)) {
            for (const StmtPtr& child : function->body) {
                visitStatement(child.get());
            }
            return;
        }
    }

    std::vector<ReferenceSite> sites_;
};

enum class TypeNavigationKind {
    Type,
    Variant,
};

struct TypeNavigationSite {
    TypeNavigationKind kind = TypeNavigationKind::Type;
    std::string qualifier;
    std::string name;
    std::optional<SourceRange> range;
};

class TypeNavigationSiteCollector {
public:
    std::vector<TypeNavigationSite> collect(const Program& program)
    {
        for (const StmtPtr& statement : program.statements) {
            visitStatement(statement.get());
        }
        return std::move(sites_);
    }

private:
    void add(
        TypeNavigationKind kind,
        const Token& token,
        std::string qualifier,
        std::string name)
    {
        if (!token.range || !token.range->valid() || name.empty()) {
            return;
        }
        sites_.push_back(TypeNavigationSite{
            kind,
            std::move(qualifier),
            std::move(name),
            token.range});
    }

    void addTypeAnnotation(const TypeAnnotation& annotation)
    {
        switch (annotation.kind) {
        case TypeAnnotation::Kind::Simple:
            add(TypeNavigationKind::Type, annotation.token, {}, annotation.token.lexeme);
            break;
        case TypeAnnotation::Kind::Qualified:
            add(
                TypeNavigationKind::Type,
                annotation.token,
                annotation.qualifier.lexeme,
                annotation.token.lexeme);
            break;
        case TypeAnnotation::Kind::Function:
            for (const TypeAnnotation& parameter : annotation.parameterTypes) {
                addTypeAnnotation(parameter);
            }
            if (annotation.returnType) {
                addTypeAnnotation(*annotation.returnType);
            }
            break;
        case TypeAnnotation::Kind::Array:
            if (annotation.elementType) {
                addTypeAnnotation(*annotation.elementType);
            }
            break;
        case TypeAnnotation::Kind::Map:
            if (annotation.keyType) {
                addTypeAnnotation(*annotation.keyType);
            }
            if (annotation.valueType) {
                addTypeAnnotation(*annotation.valueType);
            }
            break;
        case TypeAnnotation::Kind::Nullable:
        case TypeAnnotation::Kind::Optional:
            if (annotation.innerType) {
                addTypeAnnotation(*annotation.innerType);
            }
            break;
        }
        for (const TypeAnnotation& argument : annotation.typeArguments) {
            addTypeAnnotation(argument);
        }
    }

    void addTypeParameters(const std::vector<TypeParameter>& parameters)
    {
        for (const TypeParameter& parameter : parameters) {
            for (const TypeAnnotation& constraint : parameter.constraints) {
                addTypeAnnotation(constraint);
            }
        }
    }

    void addParameters(const std::vector<Parameter>& parameters)
    {
        for (const Parameter& parameter : parameters) {
            if (parameter.typeName) {
                addTypeAnnotation(*parameter.typeName);
            }
        }
    }

    void visitMethod(const MethodDecl& method)
    {
        addTypeParameters(method.typeParameters);
        addParameters(method.parameters);
        if (method.returnTypeName) {
            addTypeAnnotation(*method.returnTypeName);
        }
        for (const StmtPtr& statement : method.body) {
            visitStatement(statement.get());
        }
    }

    void visitPattern(const Pattern* pattern)
    {
        if (!pattern) {
            return;
        }
        if (const auto* orPattern = dynamic_cast<const OrPattern*>(pattern)) {
            for (const PatternPtr& alternative : orPattern->alternatives) {
                visitPattern(alternative.get());
            }
            return;
        }
        if (const auto* record = dynamic_cast<const RecordPattern*>(pattern)) {
            add(
                TypeNavigationKind::Type,
                record->name,
                record->qualifier ? record->qualifier->lexeme : std::string(),
                record->name.lexeme);
            for (const RecordPatternField& field : record->fields) {
                visitPattern(field.pattern.get());
            }
            return;
        }
        if (const auto* variant = dynamic_cast<const VariantPattern*>(pattern)) {
            const std::string enumPath
                = variant->qualifier ? variant->qualifier->lexeme : std::string();
            if (variant->qualifier && enumPath.find('.') == std::string::npos) {
                add(
                    TypeNavigationKind::Type,
                    *variant->qualifier,
                    {},
                    enumPath);
            }
            add(
                TypeNavigationKind::Variant,
                variant->name,
                enumPath,
                variant->name.lexeme);
            for (const PatternPtr& argument : variant->arguments) {
                visitPattern(argument.get());
            }
        }
    }

    void visitStatement(const Stmt* statement)
    {
        if (!statement) {
            return;
        }
        if (const auto* module = dynamic_cast<const ModuleStmt*>(statement)) {
            for (const StmtPtr& child : module->statements) {
                visitStatement(child.get());
            }
            return;
        }
        if (const auto* enumDeclaration = dynamic_cast<const EnumDeclStmt*>(statement)) {
            addTypeParameters(enumDeclaration->typeParameters);
            for (const EnumVariantDecl& variant : enumDeclaration->variants) {
                for (const TypeAnnotation& payload : variant.payloadTypes) {
                    addTypeAnnotation(payload);
                }
            }
            return;
        }
        if (const auto* structDeclaration = dynamic_cast<const StructDeclStmt*>(statement)) {
            addTypeParameters(structDeclaration->typeParameters);
            for (const StructFieldDecl& field : structDeclaration->fields) {
                addTypeAnnotation(field.typeName);
            }
            return;
        }
        if (const auto* impl = dynamic_cast<const ImplStmt*>(statement)) {
            add(TypeNavigationKind::Type, impl->typeName, {}, impl->typeName.lexeme);
            addTypeParameters(impl->typeParameters);
            for (const MethodDecl& method : impl->methods) {
                visitMethod(method);
            }
            return;
        }
        if (const auto* function = dynamic_cast<const FunctionStmt*>(statement)) {
            addTypeParameters(function->typeParameters);
            addParameters(function->parameters);
            if (function->returnTypeName) {
                addTypeAnnotation(*function->returnTypeName);
            }
            for (const StmtPtr& child : function->body) {
                visitStatement(child.get());
            }
            return;
        }
        if (const auto* let = dynamic_cast<const LetStmt*>(statement)) {
            if (let->typeName) {
                addTypeAnnotation(*let->typeName);
            }
            visitExpression(let->initializer.get());
            return;
        }
        if (const auto* expression = dynamic_cast<const ExpressionStmt*>(statement)) {
            visitExpression(expression->expression.get());
            return;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(statement)) {
            for (const StmtPtr& child : block->statements) {
                visitStatement(child.get());
            }
            return;
        }
        if (const auto* ifStatement = dynamic_cast<const IfStmt*>(statement)) {
            visitExpression(ifStatement->condition.get());
            visitStatement(ifStatement->thenBranch.get());
            visitStatement(ifStatement->elseBranch.get());
            return;
        }
        if (const auto* ifLet = dynamic_cast<const IfLetStmt*>(statement)) {
            visitExpression(ifLet->value.get());
            visitStatement(ifLet->thenBranch.get());
            visitStatement(ifLet->elseBranch.get());
            return;
        }
        if (const auto* whileStatement = dynamic_cast<const WhileStmt*>(statement)) {
            visitExpression(whileStatement->condition.get());
            visitStatement(whileStatement->body.get());
            return;
        }
        if (const auto* whileLet = dynamic_cast<const WhileLetStmt*>(statement)) {
            visitExpression(whileLet->value.get());
            visitStatement(whileLet->body.get());
            return;
        }
        if (const auto* forStatement = dynamic_cast<const ForStmt*>(statement)) {
            visitStatement(forStatement->initializer.get());
            visitExpression(forStatement->condition.get());
            visitExpression(forStatement->increment.get());
            visitStatement(forStatement->body.get());
            return;
        }
        if (const auto* forIn = dynamic_cast<const ForInStmt*>(statement)) {
            visitExpression(forIn->iterable.get());
            visitStatement(forIn->body.get());
            return;
        }
        if (const auto* returnStatement = dynamic_cast<const ReturnStmt*>(statement)) {
            visitExpression(returnStatement->value.get());
        }
    }

    void visitExpression(const Expr* expression)
    {
        if (!expression) {
            return;
        }
        if (const auto* assign = dynamic_cast<const AssignExpr*>(expression)) {
            visitExpression(assign->value.get());
            return;
        }
        if (const auto* compound = dynamic_cast<const CompoundAssignExpr*>(expression)) {
            visitExpression(compound->value.get());
            return;
        }
        if (const auto* indexAssign = dynamic_cast<const IndexAssignExpr*>(expression)) {
            visitExpression(indexAssign->collection.get());
            visitExpression(indexAssign->index.get());
            visitExpression(indexAssign->value.get());
            return;
        }
        if (const auto* indexCompound = dynamic_cast<const IndexCompoundAssignExpr*>(expression)) {
            visitExpression(indexCompound->collection.get());
            visitExpression(indexCompound->index.get());
            visitExpression(indexCompound->value.get());
            return;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression)) {
            visitExpression(unary->right.get());
            return;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression)) {
            visitExpression(binary->left.get());
            visitExpression(binary->right.get());
            return;
        }
        if (const auto* logical = dynamic_cast<const LogicalExpr*>(expression)) {
            visitExpression(logical->left.get());
            visitExpression(logical->right.get());
            return;
        }
        if (const auto* match = dynamic_cast<const MatchExpr*>(expression)) {
            visitExpression(match->value.get());
            for (const MatchArm& arm : match->arms) {
                visitPattern(arm.pattern.get());
                visitExpression(arm.guard.get());
                visitStatement(arm.body.get());
                visitExpression(arm.expression.get());
            }
            return;
        }
        if (const auto* coalesce = dynamic_cast<const CoalesceExpr*>(expression)) {
            visitExpression(coalesce->left.get());
            visitExpression(coalesce->right.get());
            return;
        }
        if (const auto* unwrap = dynamic_cast<const UnwrapOrReturnExpr*>(expression)) {
            visitExpression(unwrap->value.get());
            return;
        }
        if (const auto* grouping = dynamic_cast<const GroupingExpr*>(expression)) {
            visitExpression(grouping->expression.get());
            return;
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
            for (const TypeAnnotation& argument : call->typeArguments) {
                addTypeAnnotation(argument);
            }
            visitExpression(call->callee.get());
            for (const ExprPtr& argument : call->arguments) {
                visitExpression(argument.get());
            }
            return;
        }
        if (const auto* memberCall = dynamic_cast<const MemberCallExpr*>(expression)) {
            for (const TypeAnnotation& argument : memberCall->typeArguments) {
                addTypeAnnotation(argument);
            }
            if (const auto* receiver = dynamic_cast<const VariableExpr*>(memberCall->receiver.get())) {
                add(TypeNavigationKind::Type, receiver->name, {}, receiver->name.lexeme);
                add(
                    TypeNavigationKind::Variant,
                    memberCall->name,
                    receiver->name.lexeme,
                    memberCall->name.lexeme);
            } else if (const auto* field
                       = dynamic_cast<const FieldAccessExpr*>(memberCall->receiver.get())) {
                const auto* namespaceAlias
                    = dynamic_cast<const VariableExpr*>(field->object.get());
                if (namespaceAlias) {
                    add(
                        TypeNavigationKind::Type,
                        field->name,
                        namespaceAlias->name.lexeme,
                        field->name.lexeme);
                    add(
                        TypeNavigationKind::Variant,
                        memberCall->name,
                        namespaceAlias->name.lexeme + "." + field->name.lexeme,
                        memberCall->name.lexeme);
                }
            }
            visitExpression(memberCall->receiver.get());
            for (const ExprPtr& argument : memberCall->arguments) {
                visitExpression(argument.get());
            }
            return;
        }
        if (const auto* array = dynamic_cast<const ArrayExpr*>(expression)) {
            for (const ExprPtr& element : array->elements) {
                visitExpression(element.get());
            }
            return;
        }
        if (const auto* map = dynamic_cast<const MapExpr*>(expression)) {
            for (const MapEntry& entry : map->entries) {
                visitExpression(entry.key.get());
                visitExpression(entry.value.get());
            }
            return;
        }
        if (const auto* construct = dynamic_cast<const StructConstructExpr*>(expression)) {
            add(
                TypeNavigationKind::Type,
                construct->name,
                construct->qualifier ? construct->qualifier->lexeme : std::string(),
                construct->name.lexeme);
            for (const TypeAnnotation& argument : construct->typeArguments) {
                addTypeAnnotation(argument);
            }
            for (const StructField& field : construct->fields) {
                visitExpression(field.value.get());
            }
            return;
        }
        if (const auto* index = dynamic_cast<const IndexExpr*>(expression)) {
            visitExpression(index->collection.get());
            visitExpression(index->index.get());
            return;
        }
        if (const auto* field = dynamic_cast<const FieldAccessExpr*>(expression)) {
            visitExpression(field->object.get());
            return;
        }
        if (const auto* fieldAssign = dynamic_cast<const FieldAssignExpr*>(expression)) {
            visitExpression(fieldAssign->object.get());
            visitExpression(fieldAssign->value.get());
            return;
        }
        if (const auto* fieldCompound = dynamic_cast<const FieldCompoundAssignExpr*>(expression)) {
            visitExpression(fieldCompound->object.get());
            visitExpression(fieldCompound->value.get());
            return;
        }
        if (const auto* function = dynamic_cast<const FunctionExpr*>(expression)) {
            addTypeParameters(function->typeParameters);
            addParameters(function->parameters);
            if (function->returnTypeName) {
                addTypeAnnotation(*function->returnTypeName);
            }
            for (const StmtPtr& child : function->body) {
                visitStatement(child.get());
            }
            return;
        }
    }

    std::vector<TypeNavigationSite> sites_;
};

std::optional<std::uint32_t> decodeUtf8(
    std::string_view text,
    std::size_t offset,
    std::size_t limit,
    std::size_t& width)
{
    if (offset >= limit) {
        width = 0;
        return std::nullopt;
    }
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) {
        width = 1;
        return first;
    }
    const auto continuation = [&](std::size_t index) {
        return index < limit
            && (static_cast<unsigned char>(text[index]) & 0xc0) == 0x80;
    };
    if (first >= 0xc2 && first <= 0xdf && continuation(offset + 1)) {
        width = 2;
        return ((first & 0x1f) << 6)
            | (static_cast<unsigned char>(text[offset + 1]) & 0x3f);
    }
    if (first >= 0xe0 && first <= 0xef
        && continuation(offset + 1)
        && continuation(offset + 2)) {
        const std::uint32_t codePoint = ((first & 0x0f) << 12)
            | ((static_cast<unsigned char>(text[offset + 1]) & 0x3f) << 6)
            | (static_cast<unsigned char>(text[offset + 2]) & 0x3f);
        if (codePoint >= 0x800 && !(codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            width = 3;
            return codePoint;
        }
    }
    if (first >= 0xf0 && first <= 0xf4
        && continuation(offset + 1)
        && continuation(offset + 2)
        && continuation(offset + 3)) {
        const std::uint32_t codePoint = ((first & 0x07) << 18)
            | ((static_cast<unsigned char>(text[offset + 1]) & 0x3f) << 12)
            | ((static_cast<unsigned char>(text[offset + 2]) & 0x3f) << 6)
            | (static_cast<unsigned char>(text[offset + 3]) & 0x3f);
        if (codePoint >= 0x10000 && codePoint <= 0x10ffff) {
            width = 4;
            return codePoint;
        }
    }
    width = 1;
    return std::nullopt;
}

LspPosition lspPositionAt(std::string_view text, std::size_t byte)
{
    const std::size_t limit = std::min(byte, text.size());
    LspPosition position;
    std::size_t offset = 0;
    while (offset < limit) {
        if (text[offset] == '\n') {
            ++position.line;
            position.character = 0;
            ++offset;
            continue;
        }
        std::size_t width = 1;
        const std::optional<std::uint32_t> codePoint = decodeUtf8(text, offset, limit, width);
        position.character += codePoint && *codePoint > 0xffff ? 2 : 1;
        offset += width;
    }
    return position;
}

std::optional<std::size_t> sourceByteAtLspPosition(
    std::string_view text,
    std::size_t line,
    std::size_t character)
{
    std::size_t currentLine = 0;
    std::size_t lineStart = 0;
    while (currentLine < line) {
        const std::size_t newline = text.find('\n', lineStart);
        if (newline == std::string_view::npos) {
            return std::nullopt;
        }
        lineStart = newline + 1;
        ++currentLine;
    }

    const std::size_t lineEnd = text.find('\n', lineStart);
    const std::size_t limit = lineEnd == std::string_view::npos ? text.size() : lineEnd;
    std::size_t offset = lineStart;
    std::size_t currentCharacter = 0;
    while (offset < limit) {
        if (currentCharacter >= character) {
            return offset;
        }
        std::size_t width = 1;
        const std::optional<std::uint32_t> codePoint = decodeUtf8(text, offset, limit, width);
        const std::size_t units = codePoint && *codePoint > 0xffff ? 2 : 1;
        if (currentCharacter + units > character) {
            return offset;
        }
        currentCharacter += units;
        offset += width;
    }
    return currentCharacter == character ? std::optional<std::size_t>(offset) : std::nullopt;
}

bool rangeContains(const SourceRange& range, SourceFileId sourceId, std::size_t byte)
{
    return range.source.valid() && range.source == sourceId
        && range.start <= byte && byte < range.end;
}

std::optional<SourceRange> declarationRange(const DeclarationRecord& declaration)
{
    if (!declaration.range || !declaration.range->valid()) {
        return std::nullopt;
    }
    return declaration.range;
}

struct AnalysisSnapshot {
    std::optional<Program> program;
    DeclarationIndex declarationIndex;
    std::vector<ReferenceSite> referenceSites;
    std::vector<TypeNavigationSite> typeNavigationSites;
    std::vector<SourceFile> sources;
    std::map<std::size_t, std::string> sourceUris;
    std::vector<FileDiagnosticError> diagnostics;
};

struct DocumentAnalysis {
    std::vector<FileDiagnosticError> diagnostics;
    std::shared_ptr<AnalysisSnapshot> snapshot;
    SourceFileId sourceId;
};

LspPosition diagnosticFallbackPosition(const DiagnosticError& error)
{
    if (!error.location()) {
        return {};
    }
    return LspPosition{
        error.location()->line > 0 ? static_cast<std::size_t>(error.location()->line - 1) : 0,
        error.location()->column > 0 ? static_cast<std::size_t>(error.location()->column - 1) : 0,
    };
}

JsonValue diagnosticValue(
    const FileDiagnosticError& error,
    std::string_view source,
    SourceFileId sourceId)
{
    LspPosition start = diagnosticFallbackPosition(error);
    LspPosition end = start;
    if (error.range()
        && error.range()->source.valid()
        && error.range()->source == sourceId
        && error.range()->end <= source.size()
        && error.range()->start <= error.range()->end) {
        start = lspPositionAt(source, error.range()->start);
        end = lspPositionAt(source, error.range()->end);
    } else if (error.location() && !error.location()->line) {
        start = {};
        end = {};
    } else if (error.location()) {
        end.character += 1;
    }

    return makeObject({
        {"range", makeObject({
            {"start", makeObject({
                {"line", JsonValue::number(std::to_string(start.line))},
                {"character", JsonValue::number(std::to_string(start.character))},
            })},
            {"end", makeObject({
                {"line", JsonValue::number(std::to_string(end.line))},
                {"character", JsonValue::number(std::to_string(end.character))},
            })},
        })},
        {"severity", JsonValue::number("1")},
        {"source", JsonValue::string("compiler_design")},
        {"message", JsonValue::string(error.message())},
    });
}

DocumentAnalysis analyzeDocument(const std::string& source)
{
    DocumentAnalysis analysis;
    analysis.snapshot = std::make_shared<AnalysisSnapshot>();
    analysis.sourceId = SourceFileId{0};
    AnalysisSnapshot& snapshot = *analysis.snapshot;
    std::istringstream input(source);
    TypeChecker typeChecker;
    try {
        FrontendSession frontend;
        snapshot.program.emplace(frontend.loadStdin(input));
        snapshot.sources = snapshot.program->sources;
        snapshot.declarationIndex = DeclarationIndex::collect(*snapshot.program);
        ReferenceSiteCollector referenceCollector;
        snapshot.referenceSites = referenceCollector.collect(*snapshot.program);
        TypeNavigationSiteCollector typeNavigationCollector;
        snapshot.typeNavigationSites = typeNavigationCollector.collect(*snapshot.program);
        typeChecker.check(*snapshot.program);
        snapshot.declarationIndex = typeChecker.declarationIndex();
    } catch (const FileDiagnosticErrorList& errors) {
        snapshot.declarationIndex = typeChecker.declarationIndex();
        snapshot.diagnostics = errors.errors();
    } catch (const FileDiagnosticError& error) {
        snapshot.diagnostics.push_back(error);
    } catch (const DiagnosticError& error) {
        snapshot.diagnostics.emplace_back(
            error,
            DiagnosticSourceContext{"<stdin>", source, true});
    } catch (const std::exception& error) {
        DiagnosticError diagnostic(DiagnosticKind::Compile, error.what());
        snapshot.diagnostics.emplace_back(
            diagnostic,
            DiagnosticSourceContext{"<stdin>", source, true});
    }
    analysis.diagnostics = snapshot.diagnostics;
    return analysis;
}

std::optional<std::string> formatDocument(const std::string& source)
{
    std::istringstream input(source);
    try {
        FrontendSession frontend;
        static_cast<void>(frontend.loadStdin(input));
        return formatLosslessSource(
            frontend.losslessSourceView().file(SourceFileId{0}));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

JsonValue response(const JsonValue& id, JsonValue result)
{
    return makeObject({
        {"jsonrpc", JsonValue::string("2.0")},
        {"id", id},
        {"result", std::move(result)},
    });
}

JsonValue errorResponse(const JsonValue& id, int code, std::string message)
{
    return makeObject({
        {"jsonrpc", JsonValue::string("2.0")},
        {"id", id},
        {"error", makeObject({
            {"code", JsonValue::number(std::to_string(code))},
            {"message", JsonValue::string(std::move(message))},
        })},
    });
}

JsonValue textDocumentPosition(std::string_view text, std::size_t byte)
{
    const LspPosition position = lspPositionAt(text, byte);
    return makeObject({
        {"line", JsonValue::number(std::to_string(position.line))},
        {"character", JsonValue::number(std::to_string(position.character))},
    });
}

JsonValue textDocumentRange(std::string_view text, const SourceRange& range)
{
    return makeObject({
        {"start", textDocumentPosition(text, range.start)},
        {"end", textDocumentPosition(text, range.end)},
    });
}

std::optional<std::size_t> unsignedJsonNumber(const JsonValue* value)
{
    if (!value || value->kind != JsonValue::Kind::Number || value->text.empty()
        || value->text.front() == '-') {
        return std::nullopt;
    }
    std::size_t parsedCharacters = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(value->text, &parsedCharacters);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (parsedCharacters != value->text.size()
        || parsed > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
}

struct LspRequestPosition {
    std::size_t line = 0;
    std::size_t character = 0;
};

std::optional<LspRequestPosition> requestPosition(const JsonValue& request)
{
    const JsonValue* params = memberObject(request, "params");
    const JsonValue* position = params ? memberObject(*params, "position") : nullptr;
    if (!position) {
        return std::nullopt;
    }
    const std::optional<std::size_t> line = unsignedJsonNumber(member(*position, "line"));
    const std::optional<std::size_t> character
        = unsignedJsonNumber(member(*position, "character"));
    if (!line || !character) {
        return std::nullopt;
    }
    return LspRequestPosition{*line, *character};
}

std::optional<ResolvedSymbol> resolvedReference(
    const DeclarationIndex& index,
    const ReferenceSite& site)
{
    if (!site.expression) {
        return std::nullopt;
    }
    switch (site.kind) {
    case ReferenceSiteKind::Variable: {
        const auto* variable = dynamic_cast<const VariableExpr*>(site.expression);
        if (const std::optional<ResolvedSymbol> resolved = index.variableReference(*variable)) {
            return resolved;
        }
        if (const BindingMetadataRecord* metadata = index.variableBindingMetadata(*variable)) {
            return metadata->symbol;
        }
        return std::nullopt;
    }
    case ReferenceSiteKind::Assignment: {
        const auto* assignment = dynamic_cast<const AssignExpr*>(site.expression);
        if (const std::optional<ResolvedSymbol> resolved = index.assignmentReference(*assignment)) {
            return resolved;
        }
        if (const BindingMetadataRecord* metadata = index.assignmentBindingMetadata(*assignment)) {
            return metadata->symbol;
        }
        return std::nullopt;
    }
    case ReferenceSiteKind::CompoundAssignment: {
        const auto* assignment = dynamic_cast<const CompoundAssignExpr*>(site.expression);
        if (const std::optional<ResolvedSymbol> resolved
            = index.compoundAssignmentReference(*assignment)) {
            return resolved;
        }
        if (const BindingMetadataRecord* metadata
            = index.compoundAssignmentBindingMetadata(*assignment)) {
            return metadata->symbol;
        }
        return std::nullopt;
    }
    case ReferenceSiteKind::FieldAccess:
        return std::nullopt;
    case ReferenceSiteKind::MemberCall:
        return std::nullopt;
    }
    return std::nullopt;
}

const DeclarationRecord* definitionAt(
    const AnalysisSnapshot& snapshot,
    SourceFileId sourceId,
    std::size_t byte)
{
    const DeclarationRecord* best = nullptr;
    std::size_t bestWidth = std::numeric_limits<std::size_t>::max();
    const auto consider = [&](const DeclarationRecord* declaration) {
        if (!declaration) {
            return;
        }
        const std::optional<SourceRange> range = declarationRange(*declaration);
        if (!range || !rangeContains(*range, sourceId, byte)
            || declaration->kind == DeclarationKind::Module) {
            return;
        }
        const std::size_t width = range->end - range->start;
        if (!best || width < bestWidth
            || (width == bestWidth && range->start < best->range->start)) {
            best = declaration;
            bestWidth = width;
        }
    };

    for (const DeclarationRecord& declaration : snapshot.declarationIndex.declarations()) {
        consider(&declaration);
    }

    for (const ReferenceSite& site : snapshot.referenceSites) {
        if (!site.range || !rangeContains(*site.range, sourceId, byte)) {
            continue;
        }
        const std::optional<ResolvedSymbol> resolved
            = resolvedReference(snapshot.declarationIndex, site);
        if (resolved) {
            const DeclarationRecord* target
                = snapshot.declarationIndex.declaration(resolved->declarationId);
            if (target && declarationRange(*target)
                && target->range->source == sourceId) {
                return target;
            }
        }
    }
    return best;
}

struct DefinitionTarget {
    const DeclarationRecord* declaration = nullptr;
    SourceFileId sourceId;
};

std::optional<DefinitionTarget> importedDefinitionAt(
    const AnalysisSnapshot& snapshot,
    SourceFileId sourceId,
    std::size_t byte);

int lspSymbolKind(DeclarationKind kind)
{
    switch (kind) {
    case DeclarationKind::Module:
        return 2; // Namespace
    case DeclarationKind::Variable:
    case DeclarationKind::Parameter:
    case DeclarationKind::ForInVariable:
        return 13; // Variable
    case DeclarationKind::Function:
        return 12; // Function
    case DeclarationKind::Struct:
        return 23; // Struct
    case DeclarationKind::Enum:
        return 10; // Enum
    case DeclarationKind::Method:
        return 6; // Method
    case DeclarationKind::NamespaceAlias:
        return 3; // Namespace
    }
    return 13;
}

JsonValue definitionLocation(
    const std::string& uri,
    std::string_view source,
    const DeclarationRecord& declaration)
{
    const SourceRange range = *declaration.range;
    return makeObject({
        {"uri", JsonValue::string(uri)},
        {"range", textDocumentRange(source, range)},
    });
}

JsonValue sourceLocation(
    const std::string& uri,
    std::string_view source,
    const SourceRange& range)
{
    return makeObject({
        {"uri", JsonValue::string(uri)},
        {"range", textDocumentRange(source, range)},
    });
}

JsonValue definitionLocation(
    const std::string& uri,
    std::string_view source,
    const SourceRange& range)
{
    return sourceLocation(uri, source, range);
}

std::vector<SourceRange> referenceRangesAt(
    const AnalysisSnapshot& snapshot,
    SourceFileId sourceId,
    std::size_t byte,
    bool includeDeclaration)
{
    std::optional<DefinitionTarget> target;
    if (const DeclarationRecord* local = definitionAt(snapshot, sourceId, byte)) {
        target = DefinitionTarget{local, local->range ? local->range->source : sourceId};
    } else {
        target = importedDefinitionAt(snapshot, sourceId, byte);
    }
    if (!target || !target->declaration) {
        return {};
    }

    std::vector<SourceRange> ranges;
    if (includeDeclaration
        && target->declaration->range
        && declarationRange(*target->declaration)) {
        ranges.push_back(*target->declaration->range);
    }
    for (const ReferenceSite& site : snapshot.referenceSites) {
        if (!site.range) {
            continue;
        }
        bool matches = false;
        const std::optional<ResolvedSymbol> resolved
            = resolvedReference(snapshot.declarationIndex, site);
        if (resolved) {
            const DeclarationRecord* declaration
                = snapshot.declarationIndex.declaration(resolved->declarationId);
            matches = declaration == target->declaration;
        }
        if (!matches) {
            const std::optional<DefinitionTarget> imported = importedDefinitionAt(
                snapshot,
                site.range->source,
                site.range->start);
            matches = imported && imported->declaration == target->declaration;
        }
        if (matches) {
            ranges.push_back(*site.range);
        }
    }

    std::sort(
        ranges.begin(),
        ranges.end(),
        [&snapshot](const SourceRange& left, const SourceRange& right) {
            const auto sourceUri = [&snapshot](SourceFileId source) {
                const auto found = snapshot.sourceUris.find(source.value);
                return found == snapshot.sourceUris.end()
                    ? std::string()
                    : found->second;
            };
            const std::string leftUri = sourceUri(left.source);
            const std::string rightUri = sourceUri(right.source);
            if (leftUri != rightUri) {
                return leftUri < rightUri;
            }
            if (left.start != right.start) {
                return left.start < right.start;
            }
            if (left.end != right.end) {
                return left.end < right.end;
            }
            if (left.source != right.source) {
                return left.source < right.source;
            }
            return false;
        });
    ranges.erase(
        std::unique(
            ranges.begin(),
            ranges.end(),
            [](const SourceRange& left, const SourceRange& right) {
                return left.source == right.source
                    && left.start == right.start
                    && left.end == right.end;
            }),
        ranges.end());
    return ranges;
}

struct HoverInfo {
    TypeInfo type;
    SourceRange range;
};

std::optional<HoverInfo> hoverInfoAt(
    const AnalysisSnapshot& snapshot,
    SourceFileId sourceId,
    std::size_t byte)
{
    const ReferenceSite* bestSite = nullptr;
    const TypedExpressionRecord* bestType = nullptr;
    std::size_t bestWidth = std::numeric_limits<std::size_t>::max();
    for (const ReferenceSite& site : snapshot.referenceSites) {
        if (!site.expression || !site.range
            || !rangeContains(*site.range, sourceId, byte)) {
            continue;
        }
        const TypedExpressionRecord* typed
            = snapshot.declarationIndex.typedExpression(*site.expression);
        if (!typed) {
            continue;
        }
        const std::size_t width = site.range->end - site.range->start;
        if (!bestSite || width < bestWidth) {
            bestSite = &site;
            bestType = typed;
            bestWidth = width;
        }
    }
    if (bestSite && bestType) {
        return HoverInfo{bestType->type, *bestSite->range};
    }

    const DeclarationRecord* declaration = definitionAt(snapshot, sourceId, byte);
    if (!declaration || !declaration->range) {
        return std::nullopt;
    }
    if (const ResolvedSignatureRecord* signature
        = snapshot.declarationIndex.resolvedSignature(declaration->declarationId)) {
        return HoverInfo{signature->type, *declaration->range};
    }
    if (const auto* let = dynamic_cast<const LetStmt*>(declaration->statement)) {
        if (let->initializer) {
            if (const TypedExpressionRecord* typed
                = snapshot.declarationIndex.typedExpression(*let->initializer)) {
                return HoverInfo{typed->type, *declaration->range};
            }
        }
    }
    for (const ReferenceSite& site : snapshot.referenceSites) {
        if (!site.range || site.range->source != sourceId) {
            continue;
        }
        const std::optional<ResolvedSymbol> resolved
            = resolvedReference(snapshot.declarationIndex, site);
        if (!resolved || resolved->declarationId != declaration->declarationId
            || !site.expression) {
            continue;
        }
        if (const TypedExpressionRecord* typed
            = snapshot.declarationIndex.typedExpression(*site.expression)) {
            return HoverInfo{typed->type, *declaration->range};
        }
    }
    return std::nullopt;
}

JsonValue hoverValue(
    std::string_view source,
    const HoverInfo& info)
{
    return makeObject({
        {"contents", makeObject({
            {"kind", JsonValue::string("plaintext")},
            {"value", JsonValue::string(typeInfoName(info.type))},
        })},
        {"range", textDocumentRange(source, info.range)},
    });
}

bool validRenameName(std::string_view name)
{
    try {
        const std::vector<Token> tokens = Lexer(std::string(name)).scanTokens();
        return tokens.size() == 2
            && tokens.front().type == TokenType::Identifier
            && tokens.front().lexeme == name
            && tokens.back().type == TokenType::EndOfFile;
    } catch (const LexErrorList&) {
        return false;
    }
}

std::string completionPrefix(std::string_view source, std::size_t byte)
{
    std::size_t start = std::min(byte, source.size());
    while (start > 0) {
        const unsigned char character = static_cast<unsigned char>(source[start - 1]);
        if (!(std::isalnum(character) || character == '_')) {
            break;
        }
        --start;
    }
    return std::string(source.substr(start, std::min(byte, source.size()) - start));
}

const std::vector<const char*>& completionKeywordNames()
{
    static const std::vector<const char*> names = {
        "as",
        "break",
        "continue",
        "else",
        "enum",
        "export",
        "false",
        "for",
        "fun",
        "if",
        "impl",
        "import",
        "in",
        "let",
        "match",
        "nil",
        "print",
        "private",
        "return",
        "struct",
        "true",
        "while",
    };
    return names;
}

JsonValue keywordCompletionList(
    std::string_view source,
    const SourceRange& replaceRange,
    std::string_view prefix)
{
    std::vector<JsonValue> items;
    for (const char* keyword : completionKeywordNames()) {
        if (std::string_view(keyword).size() < prefix.size()
            || std::string_view(keyword).compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        items.push_back(makeObject({
            {"label", JsonValue::string(keyword)},
            {"kind", JsonValue::number("14")},
            {"detail", JsonValue::string("keyword")},
            {"textEdit", makeObject({
                {"range", textDocumentRange(source, replaceRange)},
                {"newText", JsonValue::string(keyword)},
            })},
        }));
    }
    return makeObject({
        {"isIncomplete", JsonValue::booleanValue(false)},
        {"items", JsonValue::array(std::move(items))},
    });
}

std::optional<std::string> completionReceiverPath(
    std::string_view source,
    std::size_t prefixStart)
{
    if (prefixStart == 0 || source[prefixStart - 1] != '.') {
        return std::nullopt;
    }
    const std::size_t receiverEnd = prefixStart - 1;
    std::size_t receiverStart = receiverEnd;
    while (receiverStart > 0) {
        const unsigned char character
            = static_cast<unsigned char>(source[receiverStart - 1]);
        if (!(std::isalnum(character) || character == '_' || character == '.')) {
            break;
        }
        --receiverStart;
    }
    if (receiverStart == receiverEnd
        || source[receiverStart] == '.'
        || source[receiverEnd - 1] == '.') {
        return std::nullopt;
    }
    return std::string(source.substr(receiverStart, receiverEnd - receiverStart));
}

int completionItemKind(DeclarationKind kind)
{
    switch (kind) {
    case DeclarationKind::Function:
        return 3; // Function
    case DeclarationKind::Method:
        return 2; // Method
    case DeclarationKind::Struct:
        return 22; // Struct
    case DeclarationKind::Enum:
        return 13; // Enum
    case DeclarationKind::Module:
        return 9; // Module
    case DeclarationKind::Variable:
    case DeclarationKind::Parameter:
    case DeclarationKind::ForInVariable:
    case DeclarationKind::NamespaceAlias:
        return 6; // Variable
    }
    return 6;
}

std::string completionDetail(
    const DeclarationIndex& index,
    const DeclarationRecord& declaration)
{
    if (const ResolvedSignatureRecord* signature
        = index.resolvedSignature(declaration.declarationId)) {
        return typeInfoName(signature->type);
    }
    std::string detail = declarationKindName(declaration.kind);
    if (!declaration.ownerType.empty()) {
        detail += " " + declaration.ownerType;
    }
    return detail;
}

std::string canonicalPathFor(std::string_view path)
{
    const std::filesystem::path input{std::string(path)};
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(input, error);
    if (!error) {
        return canonical.lexically_normal().generic_string();
    }
    return std::filesystem::absolute(input, error).lexically_normal().generic_string();
}

std::optional<unsigned char> hexDigit(char character)
{
    if (character >= '0' && character <= '9') {
        return static_cast<unsigned char>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<unsigned char>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F') {
        return static_cast<unsigned char>(character - 'A' + 10);
    }
    return std::nullopt;
}

std::optional<std::string> uriFilePath(std::string_view uri)
{
    constexpr std::string_view prefix = "file://";
    if (uri.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }

    std::string encoded(uri.substr(prefix.size()));
    const std::size_t slash = encoded.find('/');
    if (slash == std::string::npos) {
        return std::nullopt;
    }
    if (slash != 0 && encoded.substr(0, slash) != "localhost") {
        return std::nullopt;
    }
    std::string decoded = encoded.substr(slash);
    std::string result;
    result.reserve(decoded.size());
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        if (decoded[index] != '%') {
            result.push_back(decoded[index]);
            continue;
        }
        if (index + 2 >= decoded.size()) {
            return std::nullopt;
        }
        const std::optional<unsigned char> high = hexDigit(decoded[index + 1]);
        const std::optional<unsigned char> low = hexDigit(decoded[index + 2]);
        if (!high || !low) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>((*high << 4) | *low));
        index += 2;
    }
    return result;
}

std::string fileUriForPath(std::string_view path)
{
    constexpr char hex[] = "0123456789ABCDEF";
    const std::string canonical = canonicalPathFor(path);
    std::string uri = "file://";
    for (const unsigned char character : canonical) {
        const bool unreserved = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-'
            || character == '.'
            || character == '_'
            || character == '~'
            || character == '/'
            || character == ':';
        if (unreserved) {
            uri.push_back(static_cast<char>(character));
        } else {
            uri.push_back('%');
            uri.push_back(hex[character >> 4]);
            uri.push_back(hex[character & 0x0f]);
        }
    }
    return uri;
}

std::vector<std::string> workspaceRootPaths(const JsonValue& request)
{
    std::vector<std::string> paths;
    const JsonValue* params = memberObject(request, "params");
    if (!params) {
        return paths;
    }

    const auto appendUri = [&paths](const JsonValue* value) {
        if (!value || value->kind != JsonValue::Kind::String) {
            return;
        }
        if (const std::optional<std::string> path = uriFilePath(value->text)) {
            paths.push_back(canonicalPathFor(*path));
        }
    };

    const JsonValue* folders = member(*params, "workspaceFolders");
    if (folders && folders->kind == JsonValue::Kind::Array) {
        for (const JsonValue& folder : folders->elements) {
            appendUri(folder.kind == JsonValue::Kind::Object
                    ? member(folder, "uri")
                    : nullptr);
        }
    }
    if (paths.empty()) {
        appendUri(member(*params, "rootUri"));
    }

    std::vector<std::string> unique;
    for (const std::string& path : paths) {
        if (std::find(unique.begin(), unique.end(), path) == unique.end()) {
            unique.push_back(path);
        }
    }
    return unique;
}

const std::string* sourceTextFor(const AnalysisSnapshot& snapshot, SourceFileId sourceId)
{
    for (const SourceFile& source : snapshot.sources) {
        if (source.id == sourceId) {
            return &source.text;
        }
    }
    return nullptr;
}

const ModuleStmt* moduleForSource(const AnalysisSnapshot& snapshot, SourceFileId sourceId)
{
    if (!snapshot.program) {
        return nullptr;
    }
    for (const StmtPtr& statement : snapshot.program->statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (module && module->sourceId == sourceId) {
            return module;
        }
    }
    return nullptr;
}

const ModuleStmt* moduleForId(const AnalysisSnapshot& snapshot, std::size_t moduleId)
{
    if (!snapshot.program) {
        return nullptr;
    }
    for (const StmtPtr& statement : snapshot.program->statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (module && module->moduleId == moduleId) {
            return module;
        }
    }
    return nullptr;
}

const DeclarationRecord* topLevelDeclaration(
    const AnalysisSnapshot& snapshot,
    const ModuleStmt& module,
    std::string_view name)
{
    for (const StmtPtr& statement : module.statements) {
        if (!statement) {
            continue;
        }
        const DeclarationRecord* declaration = snapshot.declarationIndex.declaration(*statement);
        if (declaration && declaration->name == name && declarationRange(*declaration)) {
            return declaration;
        }
    }
    return nullptr;
}

bool exportNamesContain(const ExportStmt& exportStatement, std::string_view name)
{
    return std::any_of(
        exportStatement.names.begin(),
        exportStatement.names.end(),
        [name](const Token& token) { return token.lexeme == name; });
}

std::optional<DefinitionTarget> exportedDefinition(
    const AnalysisSnapshot& snapshot,
    std::size_t moduleId,
    std::string_view name,
    std::unordered_set<std::size_t>& visiting)
{
    if (!visiting.insert(moduleId).second) {
        return std::nullopt;
    }
    const ModuleStmt* module = moduleForId(snapshot, moduleId);
    if (!module) {
        visiting.erase(moduleId);
        return std::nullopt;
    }

    for (const StmtPtr& statement : module->statements) {
        const auto* exportStatement = dynamic_cast<const ExportStmt*>(statement.get());
        if (!exportStatement || !exportNamesContain(*exportStatement, name)) {
            continue;
        }
        if (exportStatement->sourcePath) {
            const std::optional<DefinitionTarget> forwarded = exportedDefinition(
                snapshot,
                exportStatement->resolvedModuleId,
                name,
                visiting);
            if (forwarded) {
                visiting.erase(moduleId);
                return forwarded;
            }
            continue;
        }
        if (const DeclarationRecord* declaration = topLevelDeclaration(snapshot, *module, name)) {
            visiting.erase(moduleId);
            return DefinitionTarget{declaration, module->sourceId};
        }
    }

    visiting.erase(moduleId);
    return std::nullopt;
}

std::optional<DefinitionTarget> importedDefinitionAt(
    const AnalysisSnapshot& snapshot,
    SourceFileId sourceId,
    std::size_t byte)
{
    const ModuleStmt* module = moduleForSource(snapshot, sourceId);
    if (!module) {
        return std::nullopt;
    }

    std::string name;
    std::string namespaceAlias;
    for (const ReferenceSite& site : snapshot.referenceSites) {
        if (!site.range || !rangeContains(*site.range, sourceId, byte) || !site.expression) {
            continue;
        }
        if (const auto* field = dynamic_cast<const FieldAccessExpr*>(site.expression)) {
            const auto* receiver = dynamic_cast<const VariableExpr*>(field->object.get());
            if (receiver) {
                namespaceAlias = receiver->name.lexeme;
                name = field->name.lexeme;
                break;
            }
            continue;
        }
        if (const auto* variable = dynamic_cast<const VariableExpr*>(site.expression)) {
            name = variable->name.lexeme;
            break;
        }
        if (const auto* assignment = dynamic_cast<const AssignExpr*>(site.expression)) {
            name = assignment->name.lexeme;
            break;
        }
        if (const auto* compound = dynamic_cast<const CompoundAssignExpr*>(site.expression)) {
            name = compound->name.lexeme;
            break;
        }
    }
    if (name.empty()) {
        return std::nullopt;
    }

    if (!namespaceAlias.empty()) {
        for (const StmtPtr& statement : module->statements) {
            const auto* import = dynamic_cast<const ImportStmt*>(statement.get());
            if (!import || !import->alias
                || import->alias->lexeme != namespaceAlias
                || import->resolvedModuleId == static_cast<std::size_t>(-1)) {
                continue;
            }
            std::unordered_set<std::size_t> visiting;
            if (const std::optional<DefinitionTarget> target = exportedDefinition(
                    snapshot,
                    import->resolvedModuleId,
                    name,
                    visiting)) {
                return target;
            }
        }
        return std::nullopt;
    }

    for (const StmtPtr& statement : module->statements) {
        const auto* import = dynamic_cast<const ImportStmt*>(statement.get());
        if (!import || import->alias || import->resolvedModuleId == static_cast<std::size_t>(-1)) {
            continue;
        }
        std::unordered_set<std::size_t> visiting;
        if (const std::optional<DefinitionTarget> target = exportedDefinition(
                snapshot,
                import->resolvedModuleId,
                name,
                visiting)) {
            return target;
        }
    }
    return std::nullopt;
}

std::vector<SourceRange> exportRangesForTarget(
    const AnalysisSnapshot& snapshot,
    const DeclarationRecord& target)
{
    std::vector<SourceRange> ranges;
    if (!snapshot.program) {
        return ranges;
    }
    for (const StmtPtr& statement : snapshot.program->statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (!module) {
            continue;
        }
        for (const StmtPtr& child : module->statements) {
            const auto* exportStatement = dynamic_cast<const ExportStmt*>(child.get());
            if (!exportStatement) {
                continue;
            }
            for (const Token& name : exportStatement->names) {
                if (!name.range) {
                    continue;
                }
                std::unordered_set<std::size_t> visiting;
                const std::optional<DefinitionTarget> exported = exportedDefinition(
                    snapshot,
                    module->moduleId,
                    name.lexeme,
                    visiting);
                if (exported && exported->declaration == &target) {
                    ranges.push_back(*name.range);
                }
            }
        }
    }
    return ranges;
}

std::vector<std::pair<std::string, DefinitionTarget>> exportedDefinitionsForModule(
    const AnalysisSnapshot& snapshot,
    std::size_t moduleId)
{
    std::vector<std::pair<std::string, DefinitionTarget>> definitions;
    const ModuleStmt* module = moduleForId(snapshot, moduleId);
    if (!module) {
        return definitions;
    }
    for (const StmtPtr& statement : module->statements) {
        const auto* exportStatement = dynamic_cast<const ExportStmt*>(statement.get());
        if (!exportStatement) {
            continue;
        }
        for (const Token& name : exportStatement->names) {
            std::unordered_set<std::size_t> visiting;
            const std::optional<DefinitionTarget> target = exportedDefinition(
                snapshot,
                moduleId,
                name.lexeme,
                visiting);
            if (target && target->declaration) {
                definitions.emplace_back(name.lexeme, *target);
            }
        }
    }
    return definitions;
}

bool isTypeDeclaration(const DeclarationRecord* declaration)
{
    return declaration
        && (declaration->kind == DeclarationKind::Struct
            || declaration->kind == DeclarationKind::Enum);
}

std::optional<DefinitionTarget> typeDefinitionForName(
    const AnalysisSnapshot& snapshot,
    const ModuleStmt& module,
    std::string_view qualifier,
    std::string_view name)
{
    if (name.empty()) {
        return std::nullopt;
    }

    if (!qualifier.empty()) {
        for (const StmtPtr& statement : module.statements) {
            const auto* import = dynamic_cast<const ImportStmt*>(statement.get());
            if (!import || !import->alias
                || import->alias->lexeme != qualifier
                || import->resolvedModuleId == static_cast<std::size_t>(-1)) {
                continue;
            }
            std::unordered_set<std::size_t> visiting;
            if (const std::optional<DefinitionTarget> target = exportedDefinition(
                    snapshot,
                    import->resolvedModuleId,
                    name,
                    visiting);
                target && isTypeDeclaration(target->declaration)) {
                return target;
            }
        }
        return std::nullopt;
    }

    if (const DeclarationRecord* declaration = topLevelDeclaration(snapshot, module, name);
        isTypeDeclaration(declaration)) {
        return DefinitionTarget{declaration, module.sourceId};
    }

    for (const StmtPtr& statement : module.statements) {
        const auto* import = dynamic_cast<const ImportStmt*>(statement.get());
        if (!import || import->alias || import->resolvedModuleId == static_cast<std::size_t>(-1)) {
            continue;
        }
        std::unordered_set<std::size_t> visiting;
        if (const std::optional<DefinitionTarget> target = exportedDefinition(
                snapshot,
                import->resolvedModuleId,
                name,
                visiting);
            target && isTypeDeclaration(target->declaration)) {
            return target;
        }
    }
    return std::nullopt;
}

std::optional<DefinitionTarget> typeDefinitionForPath(
    const AnalysisSnapshot& snapshot,
    const ModuleStmt& module,
    std::string_view path)
{
    const std::size_t separator = path.find('.');
    if (separator == std::string_view::npos) {
        return typeDefinitionForName(snapshot, module, {}, path);
    }
    return typeDefinitionForName(
        snapshot,
        module,
        path.substr(0, separator),
        path.substr(separator + 1));
}

std::optional<SourceRange> variantDefinitionForSite(
    const AnalysisSnapshot& snapshot,
    const ModuleStmt& module,
    const TypeNavigationSite& site)
{
    const std::optional<DefinitionTarget> type = typeDefinitionForPath(
        snapshot,
        module,
        site.qualifier);
    if (!type || !type->declaration || type->declaration->kind != DeclarationKind::Enum
        || !type->declaration->statement) {
        return std::nullopt;
    }
    const auto* enumDeclaration
        = dynamic_cast<const EnumDeclStmt*>(type->declaration->statement);
    if (!enumDeclaration) {
        return std::nullopt;
    }
    for (const EnumVariantDecl& variant : enumDeclaration->variants) {
        if (variant.name.lexeme == site.name && variant.name.range
            && variant.name.range->valid()) {
            return variant.name.range;
        }
    }
    return std::nullopt;
}

std::optional<SourceRange> typeNavigationRangeAt(
    const AnalysisSnapshot& snapshot,
    SourceFileId sourceId,
    std::size_t byte)
{
    const ModuleStmt* module = moduleForSource(snapshot, sourceId);
    if (!module) {
        return std::nullopt;
    }

    std::optional<SourceRange> best;
    std::size_t bestWidth = std::numeric_limits<std::size_t>::max();
    for (const TypeNavigationSite& site : snapshot.typeNavigationSites) {
        if (!site.range || !rangeContains(*site.range, sourceId, byte)) {
            continue;
        }

        std::optional<SourceRange> candidate;
        if (site.kind == TypeNavigationKind::Type) {
            if (const std::optional<DefinitionTarget> type = typeDefinitionForPath(
                    snapshot,
                    *module,
                    site.qualifier.empty() ? site.name : site.qualifier + "." + site.name);
                type && type->declaration && type->declaration->range) {
                candidate = *type->declaration->range;
            }
        } else {
            candidate = variantDefinitionForSite(snapshot, *module, site);
        }
        if (!candidate) {
            continue;
        }

        const std::size_t width = site.range->end - site.range->start;
        if (!best || width < bestWidth
            || (width == bestWidth && site.range->start < best->start)) {
            best = candidate;
            bestWidth = width;
        }
    }
    return best;
}

const StructDeclStmt* structDeclarationForTypedReceiver(
    const AnalysisSnapshot& snapshot,
    const ModuleStmt& module,
    const Expr& receiver)
{
    const TypedExpressionRecord* typedObject
        = snapshot.declarationIndex.typedExpression(receiver);
    if (!typedObject) {
        return nullptr;
    }
    TypeInfo objectType = typedObject->type;
    if (objectType.kind == StaticType::Nullable && objectType.nullableOf) {
        objectType = *objectType.nullableOf;
    }
    if (objectType.kind != StaticType::Struct || !objectType.structName) {
        return nullptr;
    }
    const std::optional<DefinitionTarget> target = typeDefinitionForPath(
        snapshot,
        module,
        *objectType.structName);
    if (!target || !target->declaration || !target->declaration->statement) {
        return nullptr;
    }
    if (const auto* declaration
        = dynamic_cast<const StructDeclStmt*>(target->declaration->statement)) {
        return declaration;
    }
    return nullptr;
}

const StructDeclStmt* structFieldCompletionTargetAt(
    const AnalysisSnapshot& snapshot,
    SourceFileId sourceId,
    std::size_t byte)
{
    const ModuleStmt* module = moduleForSource(snapshot, sourceId);
    if (!module) {
        return nullptr;
    }
    for (const ReferenceSite& site : snapshot.referenceSites) {
        if (site.kind != ReferenceSiteKind::FieldAccess
            || !site.range
            || !rangeContains(*site.range, sourceId, byte)
            || !site.expression) {
            continue;
        }
        const auto* field = dynamic_cast<const FieldAccessExpr*>(site.expression);
        if (field) {
            if (const StructDeclStmt* declaration = structDeclarationForTypedReceiver(
                    snapshot,
                    *module,
                    *field->object)) {
                return declaration;
            }
        }
    }
    return nullptr;
}

const StructDeclStmt* structMethodCompletionTargetAt(
    const AnalysisSnapshot& snapshot,
    SourceFileId sourceId,
    std::size_t byte)
{
    const ModuleStmt* module = moduleForSource(snapshot, sourceId);
    if (!module) {
        return nullptr;
    }
    for (const ReferenceSite& site : snapshot.referenceSites) {
        if (site.kind != ReferenceSiteKind::MemberCall
            || !site.range
            || !rangeContains(*site.range, sourceId, byte)
            || !site.expression) {
            continue;
        }
        const auto* memberCall = dynamic_cast<const MemberCallExpr*>(site.expression);
        if (memberCall) {
            if (const StructDeclStmt* declaration = structDeclarationForTypedReceiver(
                    snapshot,
                    *module,
                    *memberCall->receiver)) {
                return declaration;
            }
        }
    }
    return nullptr;
}

std::shared_ptr<AnalysisSnapshot> analyzeVirtualWorkspace(
    const std::vector<FrontendVirtualFile>& files,
    const std::map<std::string, std::string>& uriByCanonicalPath,
    const std::vector<std::string>& workspaceRoots)
{
    auto snapshot = std::make_shared<AnalysisSnapshot>();
    TypeChecker typeChecker;
    try {
        FrontendSession frontend;
        frontend.setImportSearchPaths(workspaceRoots);
        frontend.setVirtualImportRoots(workspaceRoots);
        snapshot->program.emplace(frontend.loadVirtualFiles(files));
        snapshot->sources = snapshot->program->sources;
        snapshot->declarationIndex = DeclarationIndex::collect(*snapshot->program);
        ReferenceSiteCollector referenceCollector;
        snapshot->referenceSites = referenceCollector.collect(*snapshot->program);
        TypeNavigationSiteCollector typeNavigationCollector;
        snapshot->typeNavigationSites = typeNavigationCollector.collect(*snapshot->program);

        if (snapshot->program->moduleGraph) {
            for (const ModuleGraphNode& node : snapshot->program->moduleGraph->nodes) {
                const auto found = uriByCanonicalPath.find(node.canonicalPath);
                if (found != uriByCanonicalPath.end()) {
                    snapshot->sourceUris[node.sourceId.value] = found->second;
                }
            }
        }
        for (const SourceFile& source : snapshot->sources) {
            if (snapshot->sourceUris.find(source.id.value) != snapshot->sourceUris.end()) {
                continue;
            }
            const auto found = uriByCanonicalPath.find(canonicalPathFor(source.path));
            if (found != uriByCanonicalPath.end()) {
                snapshot->sourceUris[source.id.value] = found->second;
            } else {
                snapshot->sourceUris[source.id.value] = fileUriForPath(source.path);
            }
        }

        typeChecker.check(*snapshot->program);
        snapshot->declarationIndex = typeChecker.declarationIndex();
    } catch (const FileDiagnosticErrorList& errors) {
        snapshot->declarationIndex = typeChecker.declarationIndex();
        snapshot->diagnostics = errors.errors();
    } catch (const FileDiagnosticError& error) {
        snapshot->diagnostics.push_back(error);
    } catch (const DiagnosticError& error) {
        snapshot->diagnostics.emplace_back(
            error,
            DiagnosticSourceContext{"<workspace>", {}, false});
    } catch (const std::exception& error) {
        DiagnosticError diagnostic(DiagnosticKind::Compile, error.what());
        snapshot->diagnostics.emplace_back(
            diagnostic,
            DiagnosticSourceContext{"<workspace>", {}, false});
    }
    return snapshot;
}

DocumentAnalysis documentAnalysisFor(
    const std::shared_ptr<AnalysisSnapshot>& snapshot,
    const std::string& uri,
    const std::string& path)
{
    DocumentAnalysis analysis;
    analysis.snapshot = snapshot;
    for (const auto& entry : snapshot->sourceUris) {
        if (entry.second == uri) {
            analysis.sourceId = SourceFileId{entry.first};
            break;
        }
    }
    if (!analysis.sourceId.valid()) {
        const std::string canonicalPath = canonicalPathFor(path);
        for (const FileDiagnosticError& error : snapshot->diagnostics) {
            if (!error.range() || !error.range()->source.valid()) {
                continue;
            }
            if (canonicalPathFor(error.sourceContext().path) == canonicalPath) {
                analysis.sourceId = error.range()->source;
                break;
            }
        }
    }
    for (const FileDiagnosticError& error : snapshot->diagnostics) {
        if (error.range() && error.range()->source.valid()) {
            if (error.range()->source == analysis.sourceId) {
                analysis.diagnostics.push_back(error);
            }
            continue;
        }
        if (!error.sourceContext().path.empty()
            && canonicalPathFor(error.sourceContext().path) != canonicalPathFor(path)) {
            continue;
        }
        analysis.diagnostics.push_back(error);
    }
    return analysis;
}

JsonValue documentSymbol(
    std::string_view source,
    const DeclarationRecord& declaration)
{
    const SourceRange range = *declaration.range;
    std::string detail = declarationKindName(declaration.kind);
    if (!declaration.ownerType.empty()) {
        detail += " " + declaration.ownerType;
    }
    return makeObject({
        {"name", JsonValue::string(declaration.name)},
        {"kind", JsonValue::number(std::to_string(lspSymbolKind(declaration.kind)))},
        {"range", textDocumentRange(source, range)},
        {"selectionRange", textDocumentRange(source, range)},
        {"detail", JsonValue::string(std::move(detail))},
    });
}

JsonValue publishDiagnostics(
    const std::string& uri,
    const std::string& source,
    SourceFileId sourceId,
    const std::vector<FileDiagnosticError>& diagnostics)
{
    std::vector<JsonValue> values;
    values.reserve(diagnostics.size());
    for (const FileDiagnosticError& diagnostic : diagnostics) {
        values.push_back(diagnosticValue(diagnostic, source, sourceId));
    }
    return makeObject({
        {"jsonrpc", JsonValue::string("2.0")},
        {"method", JsonValue::string("textDocument/publishDiagnostics")},
        {"params", makeObject({
            {"uri", JsonValue::string(uri)},
            {"diagnostics", JsonValue::array(std::move(values))},
        })},
    });
}

class LanguageServer {
public:
    int run(std::istream& input, std::ostream& output)
    {
        while (true) {
            const std::optional<std::string> body = readMessage(input);
            if (!body) {
                return shuttingDown_ ? 0 : 0;
            }

            const JsonValue request = JsonParser(*body).parse();
            if (request.kind != JsonValue::Kind::Object) {
                continue;
            }
            const std::optional<JsonValue> id = requestId(request);
            const std::optional<std::string> method = stringMember(request, "method");
            if (!method) {
                if (id) {
                    writeMessage(output, errorResponse(*id, -32600, "request method is missing"));
                }
                continue;
            }

            if (*method == "exit") {
                return shuttingDown_ ? 0 : 0;
            }
            if (*method == "initialize") {
                workspaceRoots_ = workspaceRootPaths(request);
                if (id) {
                    writeMessage(
                        output,
                        response(
                            *id,
                            makeObject({
                                {"capabilities", makeObject({
                                    {"textDocumentSync", JsonValue::number("1")},
                                    {"documentFormattingProvider", JsonValue::booleanValue(true)},
                                    {"definitionProvider", JsonValue::booleanValue(true)},
                                    {"documentSymbolProvider", JsonValue::booleanValue(true)},
                                    {"referencesProvider", JsonValue::booleanValue(true)},
                                    {"hoverProvider", JsonValue::booleanValue(true)},
                                    {"renameProvider", JsonValue::booleanValue(true)},
                                    {"completionProvider", makeObject({
                                        {"triggerCharacters", JsonValue::array({JsonValue::string(".")})},
                                    })},
                                    {"workspaceSymbolProvider", JsonValue::booleanValue(true)},
                                })},
                            })));
                }
                continue;
            }
            if (*method == "shutdown") {
                shuttingDown_ = true;
                if (id) {
                    writeMessage(output, response(*id, JsonValue::null()));
                }
                continue;
            }
            if (*method == "initialized" || *method == "$/cancelRequest") {
                continue;
            }
            if (*method == "textDocument/didOpen") {
                handleDidOpen(request, output);
                continue;
            }
            if (*method == "textDocument/didChange") {
                handleDidChange(request, output);
                continue;
            }
            if (*method == "textDocument/didClose") {
                handleDidClose(request, output);
                continue;
            }
            if (*method == "textDocument/formatting") {
                if (id) {
                    writeMessage(output, response(*id, handleFormatting(request)));
                }
                continue;
            }
            if (*method == "textDocument/definition") {
                if (id) {
                    writeMessage(output, response(*id, handleDefinition(request)));
                }
                continue;
            }
            if (*method == "textDocument/documentSymbol") {
                if (id) {
                    writeMessage(output, response(*id, handleDocumentSymbols(request)));
                }
                continue;
            }
            if (*method == "textDocument/references") {
                if (id) {
                    writeMessage(output, response(*id, handleReferences(request)));
                }
                continue;
            }
            if (*method == "textDocument/hover") {
                if (id) {
                    writeMessage(output, response(*id, handleHover(request)));
                }
                continue;
            }
            if (*method == "textDocument/rename") {
                if (id) {
                    writeMessage(output, response(*id, handleRename(request)));
                }
                continue;
            }
            if (*method == "textDocument/completion") {
                if (id) {
                    writeMessage(output, response(*id, handleCompletion(request)));
                }
                continue;
            }
            if (*method == "workspace/symbol") {
                if (id) {
                    writeMessage(output, response(*id, handleWorkspaceSymbols(request)));
                }
                continue;
            }
            if (id) {
                writeMessage(output, errorResponse(*id, -32601, "method not found"));
            }
        }
    }

private:
    struct Document {
        std::string text;
        std::int64_t version = 0;
        DocumentAnalysis analysis;
    };

    static std::optional<JsonValue> requestId(const JsonValue& request)
    {
        if (request.kind != JsonValue::Kind::Object) {
            return std::nullopt;
        }
        const auto found = request.members.find("id");
        if (found == request.members.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    static std::optional<std::string> documentUri(const JsonValue& request)
    {
        const JsonValue* params = memberObject(request, "params");
        if (!params) {
            return std::nullopt;
        }
        const JsonValue* document = memberObject(*params, "textDocument");
        if (!document) {
            return std::nullopt;
        }
        return stringMember(*document, "uri");
    }

    void rebuildWorkspaceAnalyses(const std::optional<std::string>& preferredUri = std::nullopt)
    {
        if (documents_.empty()) {
            return;
        }

        std::vector<std::string> orderedUris;
        orderedUris.reserve(documents_.size());
        if (preferredUri && documents_.find(*preferredUri) != documents_.end()) {
            orderedUris.push_back(*preferredUri);
        }
        for (const auto& entry : documents_) {
            if (!preferredUri || entry.first != *preferredUri) {
                orderedUris.push_back(entry.first);
            }
        }

        std::vector<FrontendVirtualFile> files;
        files.reserve(orderedUris.size());
        std::map<std::string, std::string> uriByCanonicalPath;
        for (const std::string& uri : orderedUris) {
            const std::optional<std::string> path = uriFilePath(uri);
            if (!path) {
                for (auto& entry : documents_) {
                    entry.second.analysis = analyzeDocument(entry.second.text);
                }
                return;
            }
            files.push_back(FrontendVirtualFile{*path, documents_.at(uri).text});
            uriByCanonicalPath[canonicalPathFor(*path)] = uri;
        }

        const std::shared_ptr<AnalysisSnapshot> snapshot = analyzeVirtualWorkspace(
            files,
            uriByCanonicalPath,
            workspaceRoots_);
        for (auto& entry : documents_) {
            const std::optional<std::string> path = uriFilePath(entry.first);
            if (!path) {
                entry.second.analysis = analyzeDocument(entry.second.text);
                continue;
            }
            entry.second.analysis = documentAnalysisFor(snapshot, entry.first, *path);
        }
    }

    void publish(std::ostream& output, const std::string& uri, const Document& document)
    {
        writeMessage(
            output,
            publishDiagnostics(
                uri,
                document.text,
                document.analysis.sourceId,
                document.analysis.diagnostics));
    }

    void handleDidOpen(const JsonValue& request, std::ostream& output)
    {
        const JsonValue* params = memberObject(request, "params");
        if (!params) {
            return;
        }
        const JsonValue* document = memberObject(*params, "textDocument");
        if (!document) {
            return;
        }
        const std::optional<std::string> uri = stringMember(*document, "uri");
        const std::optional<std::string> text = stringMember(*document, "text");
        if (!uri || !text) {
            return;
        }
        Document state;
        state.text = *text;
        documents_[*uri] = std::move(state);
        if (const JsonValue* version = member(*document, "version")) {
            if (version->kind == JsonValue::Kind::Number) {
                try {
                    documents_[*uri].version = std::stoll(version->text);
                } catch (const std::exception&) {
                    // The first boundary does not need version arithmetic;
                    // retain the document text even for an unusual version.
                }
            }
        }
        rebuildWorkspaceAnalyses(*uri);
        publish(output, *uri, documents_.at(*uri));
    }

    void handleDidChange(const JsonValue& request, std::ostream& output)
    {
        const JsonValue* params = memberObject(request, "params");
        if (!params) {
            return;
        }
        const std::optional<std::string> uri = documentUri(request);
        const JsonValue* changes = member(*params, "contentChanges");
        if (!uri || !changes || changes->kind != JsonValue::Kind::Array
            || changes->elements.empty()) {
            return;
        }
        // The advertised sync kind is Full, so the first change's text is the
        // complete current document. Ignore later entries in this prototype.
        const std::optional<std::string> text = stringMember(changes->elements.front(), "text");
        if (!text) {
            return;
        }
        Document& document = documents_[*uri];
        document.text = *text;
        if (const JsonValue* version = member(*params, "textDocument")) {
            if (const JsonValue* value = member(*version, "version")) {
                if (value->kind == JsonValue::Kind::Number) {
                    try {
                        document.version = std::stoll(value->text);
                    } catch (const std::exception&) {
                    }
                }
            }
        }
        rebuildWorkspaceAnalyses(*uri);
        publish(output, *uri, document);
    }

    void handleDidClose(const JsonValue& request, std::ostream& output)
    {
        const std::optional<std::string> uri = documentUri(request);
        if (!uri) {
            return;
        }
        documents_.erase(*uri);
        rebuildWorkspaceAnalyses();
        writeMessage(
            output,
            publishDiagnostics(*uri, "", SourceFileId{}, {}));
    }

    JsonValue handleFormatting(const JsonValue& request) const
    {
        const std::optional<std::string> uri = documentUri(request);
        if (!uri) {
            return JsonValue::array({});
        }
        const auto found = documents_.find(*uri);
        if (found == documents_.end()) {
            return JsonValue::array({});
        }
        const std::optional<std::string> formatted = formatDocument(found->second.text);
        if (!formatted) {
            return JsonValue::array({});
        }
        return JsonValue::array({makeObject({
            {"range", makeObject({
                {"start", textDocumentPosition(found->second.text, 0)},
                {"end", textDocumentPosition(found->second.text, found->second.text.size())},
            })},
            {"newText", JsonValue::string(*formatted)},
        })});
    }

    JsonValue handleDefinition(const JsonValue& request) const
    {
        const std::optional<std::string> uri = documentUri(request);
        const std::optional<LspRequestPosition> position = requestPosition(request);
        if (!uri || !position) {
            return JsonValue::null();
        }
        const auto found = documents_.find(*uri);
        if (found == documents_.end()
            || !found->second.analysis.snapshot
            || !found->second.analysis.snapshot->program) {
            return JsonValue::null();
        }
        const AnalysisSnapshot& snapshot = *found->second.analysis.snapshot;
        const std::optional<std::size_t> byte = sourceByteAtLspPosition(
            found->second.text,
            position->line,
            position->character);
        if (!byte) {
            return JsonValue::null();
        }
        const DeclarationRecord* declaration = definitionAt(
            snapshot,
            found->second.analysis.sourceId,
            *byte);
        if (declaration && declaration->range) {
            return definitionLocation(*uri, found->second.text, *declaration);
        }
        const std::optional<DefinitionTarget> imported = importedDefinitionAt(
            snapshot,
            found->second.analysis.sourceId,
            *byte);
        if (!imported || !imported->declaration || !imported->declaration->range) {
            const std::optional<SourceRange> navigation = typeNavigationRangeAt(
                snapshot,
                found->second.analysis.sourceId,
                *byte);
            if (!navigation) {
                return JsonValue::null();
            }
            const auto uriForSource = snapshot.sourceUris.find(navigation->source.value);
            const std::string* source = sourceTextFor(snapshot, navigation->source);
            if (uriForSource != snapshot.sourceUris.end() && source) {
                return definitionLocation(uriForSource->second, *source, *navigation);
            }
            if (navigation->source == found->second.analysis.sourceId) {
                return definitionLocation(*uri, found->second.text, *navigation);
            }
            return JsonValue::null();
        }
        const auto uriForSource = snapshot.sourceUris.find(imported->sourceId.value);
        const std::string* source = sourceTextFor(snapshot, imported->sourceId);
        if (uriForSource == snapshot.sourceUris.end() || !source) {
            return JsonValue::null();
        }
        return definitionLocation(
            uriForSource->second,
            *source,
            *imported->declaration);
    }

    JsonValue handleReferences(const JsonValue& request) const
    {
        const std::optional<std::string> uri = documentUri(request);
        const std::optional<LspRequestPosition> position = requestPosition(request);
        if (!uri || !position) {
            return JsonValue::array({});
        }
        const auto found = documents_.find(*uri);
        if (found == documents_.end()
            || !found->second.analysis.snapshot
            || !found->second.analysis.snapshot->program) {
            return JsonValue::array({});
        }
        const AnalysisSnapshot& snapshot = *found->second.analysis.snapshot;
        const std::optional<std::size_t> byte = sourceByteAtLspPosition(
            found->second.text,
            position->line,
            position->character);
        if (!byte) {
            return JsonValue::array({});
        }

        bool includeDeclaration = false;
        const JsonValue* params = memberObject(request, "params");
        const JsonValue* context = params ? memberObject(*params, "context") : nullptr;
        const JsonValue* include = context ? member(*context, "includeDeclaration") : nullptr;
        if (include && include->kind == JsonValue::Kind::Boolean) {
            includeDeclaration = include->boolean;
        }

        const std::vector<SourceRange> ranges = referenceRangesAt(
            snapshot,
            found->second.analysis.sourceId,
            *byte,
            includeDeclaration);
        std::vector<JsonValue> locations;
        locations.reserve(ranges.size());
        for (const SourceRange& range : ranges) {
            const auto sourceUri = snapshot.sourceUris.find(range.source.value);
            const std::string* source = sourceTextFor(snapshot, range.source);
            if (sourceUri != snapshot.sourceUris.end() && source) {
                locations.push_back(sourceLocation(sourceUri->second, *source, range));
            } else if (range.source == found->second.analysis.sourceId) {
                locations.push_back(sourceLocation(*uri, found->second.text, range));
            }
        }
        return JsonValue::array(std::move(locations));
    }

    JsonValue handleHover(const JsonValue& request) const
    {
        const std::optional<std::string> uri = documentUri(request);
        const std::optional<LspRequestPosition> position = requestPosition(request);
        if (!uri || !position) {
            return JsonValue::null();
        }
        const auto found = documents_.find(*uri);
        if (found == documents_.end()
            || !found->second.analysis.snapshot
            || !found->second.analysis.snapshot->program) {
            return JsonValue::null();
        }
        const AnalysisSnapshot& snapshot = *found->second.analysis.snapshot;
        const std::optional<std::size_t> byte = sourceByteAtLspPosition(
            found->second.text,
            position->line,
            position->character);
        if (!byte) {
            return JsonValue::null();
        }
        const std::optional<HoverInfo> info = hoverInfoAt(
            snapshot,
            found->second.analysis.sourceId,
            *byte);
        return info
            ? hoverValue(found->second.text, *info)
            : JsonValue::null();
    }

    JsonValue handleRename(const JsonValue& request) const
    {
        const std::optional<std::string> uri = documentUri(request);
        const std::optional<LspRequestPosition> position = requestPosition(request);
        const JsonValue* params = memberObject(request, "params");
        const std::optional<std::string> newName
            = params ? stringMember(*params, "newName") : std::nullopt;
        if (!uri || !position || !newName || !validRenameName(*newName)) {
            return JsonValue::null();
        }
        const auto found = documents_.find(*uri);
        if (found == documents_.end()
            || !found->second.analysis.snapshot
            || !found->second.analysis.snapshot->program
            || !found->second.analysis.diagnostics.empty()) {
            return JsonValue::null();
        }
        const AnalysisSnapshot& snapshot = *found->second.analysis.snapshot;
        const std::optional<std::size_t> byte = sourceByteAtLspPosition(
            found->second.text,
            position->line,
            position->character);
        if (!byte) {
            return JsonValue::null();
        }
        std::optional<DefinitionTarget> target;
        if (const DeclarationRecord* local = definitionAt(
                snapshot,
                found->second.analysis.sourceId,
                *byte)) {
            target = DefinitionTarget{
                local,
                local->range ? local->range->source : found->second.analysis.sourceId};
        } else {
            target = importedDefinitionAt(
                snapshot,
                found->second.analysis.sourceId,
                *byte);
        }
        if (!target || !target->declaration) {
            return JsonValue::null();
        }

        std::vector<SourceRange> ranges = referenceRangesAt(
            snapshot,
            found->second.analysis.sourceId,
            *byte,
            true);
        const std::vector<SourceRange> exportRanges = exportRangesForTarget(
            snapshot,
            *target->declaration);
        ranges.insert(ranges.end(), exportRanges.begin(), exportRanges.end());
        std::sort(
            ranges.begin(),
            ranges.end(),
            [](const SourceRange& left, const SourceRange& right) {
                if (left.source != right.source) {
                    return left.source < right.source;
                }
                if (left.start != right.start) {
                    return left.start < right.start;
                }
                return left.end < right.end;
            });
        ranges.erase(
            std::unique(
                ranges.begin(),
                ranges.end(),
                [](const SourceRange& left, const SourceRange& right) {
                    return left.source == right.source
                        && left.start == right.start
                        && left.end == right.end;
                }),
            ranges.end());
        if (ranges.empty()) {
            return JsonValue::null();
        }

        std::map<std::string, std::vector<SourceRange>> rangesByUri;
        for (const SourceRange& range : ranges) {
            if (!range.source.valid() || range.start > range.end) {
                return JsonValue::null();
            }
            std::string targetUri;
            const auto sourceUri = snapshot.sourceUris.find(range.source.value);
            if (sourceUri != snapshot.sourceUris.end()) {
                targetUri = sourceUri->second;
            } else if (range.source == found->second.analysis.sourceId) {
                targetUri = *uri;
            } else {
                return JsonValue::null();
            }
            if (documents_.find(targetUri) == documents_.end()
                || range.end > documents_.at(targetUri).text.size()) {
                return JsonValue::null();
            }
            rangesByUri[targetUri].push_back(range);
        }

        std::map<std::string, std::string> renamedTexts;
        for (const auto& entry : documents_) {
            renamedTexts.emplace(entry.first, entry.second.text);
        }
        for (auto& entry : rangesByUri) {
            std::vector<SourceRange>& documentRanges = entry.second;
            std::sort(
                documentRanges.begin(),
                documentRanges.end(),
                [](const SourceRange& left, const SourceRange& right) {
                    if (left.start != right.start) {
                        return left.start > right.start;
                    }
                    return left.end > right.end;
                });
            std::string& renamed = renamedTexts.at(entry.first);
            for (const SourceRange& range : documentRanges) {
                renamed.replace(range.start, range.end - range.start, *newName);
            }
        }

        bool canAnalyzeVirtualWorkspace = true;
        std::vector<std::string> orderedUris;
        orderedUris.reserve(documents_.size());
        orderedUris.push_back(*uri);
        for (const auto& entry : documents_) {
            if (entry.first != *uri) {
                orderedUris.push_back(entry.first);
            }
        }
        std::vector<FrontendVirtualFile> virtualFiles;
        std::map<std::string, std::string> uriByCanonicalPath;
        for (const std::string& documentUri : orderedUris) {
            const std::optional<std::string> path = uriFilePath(documentUri);
            if (!path) {
                canAnalyzeVirtualWorkspace = false;
                break;
            }
            virtualFiles.push_back(FrontendVirtualFile{
                *path,
                renamedTexts.at(documentUri)});
            uriByCanonicalPath[canonicalPathFor(*path)] = documentUri;
        }
        if (canAnalyzeVirtualWorkspace) {
            const std::shared_ptr<AnalysisSnapshot> renamedSnapshot = analyzeVirtualWorkspace(
                virtualFiles,
                uriByCanonicalPath,
                workspaceRoots_);
            if (!renamedSnapshot->diagnostics.empty()) {
                return JsonValue::null();
            }
        } else {
            if ((snapshot.program->moduleGraph
                    && snapshot.program->moduleGraph->nodes.size() > 1)
                || rangesByUri.size() != 1
                || rangesByUri.find(*uri) == rangesByUri.end()) {
                return JsonValue::null();
            }
            const DocumentAnalysis renamedAnalysis = analyzeDocument(renamedTexts.at(*uri));
            if (!renamedAnalysis.diagnostics.empty()) {
                return JsonValue::null();
            }
        }

        JsonValue::Object changes;
        for (const auto& entry : rangesByUri) {
            const Document& document = documents_.at(entry.first);
            std::vector<SourceRange> documentRanges = entry.second;
            std::sort(
                documentRanges.begin(),
                documentRanges.end(),
                [](const SourceRange& left, const SourceRange& right) {
                    if (left.start != right.start) {
                        return left.start < right.start;
                    }
                    return left.end < right.end;
                });
            std::vector<JsonValue> edits;
            edits.reserve(documentRanges.size());
            for (const SourceRange& range : documentRanges) {
                edits.push_back(makeObject({
                    {"range", textDocumentRange(document.text, range)},
                    {"newText", JsonValue::string(*newName)},
                }));
            }
            changes.emplace(entry.first, JsonValue::array(std::move(edits)));
        }
        return makeObject({
            {"changes", JsonValue::object(std::move(changes))},
        });
    }

    JsonValue handleCompletion(const JsonValue& request) const
    {
        const std::optional<std::string> uri = documentUri(request);
        const std::optional<LspRequestPosition> position = requestPosition(request);
        if (!uri || !position) {
            return makeObject({
                {"isIncomplete", JsonValue::booleanValue(false)},
                {"items", JsonValue::array({})},
            });
        }
        const auto found = documents_.find(*uri);
        if (found == documents_.end()) {
            return makeObject({
                {"isIncomplete", JsonValue::booleanValue(false)},
                {"items", JsonValue::array({})},
            });
        }
        const std::optional<std::size_t> byte = sourceByteAtLspPosition(
            found->second.text,
            position->line,
            position->character);
        if (!byte) {
            return makeObject({
                {"isIncomplete", JsonValue::booleanValue(false)},
                {"items", JsonValue::array({})},
            });
        }

        const std::string prefix = completionPrefix(found->second.text, *byte);
        std::size_t prefixStart = *byte - prefix.size();
        const SourceRange replaceRange{
            found->second.analysis.sourceId,
            prefixStart,
            *byte};
        if (!found->second.analysis.snapshot
            || !found->second.analysis.snapshot->program) {
            return keywordCompletionList(found->second.text, replaceRange, prefix);
        }
        const AnalysisSnapshot& snapshot = *found->second.analysis.snapshot;
        const auto isOpenSource = [&snapshot, this](SourceFileId sourceId) {
            const auto sourceUri = snapshot.sourceUris.find(sourceId.value);
            return sourceUri != snapshot.sourceUris.end()
                && documents_.find(sourceUri->second) != documents_.end();
        };
        const std::optional<std::string> receiverPath = completionReceiverPath(
            found->second.text,
            prefixStart);
        std::optional<std::string> namespaceAlias;
        if (prefixStart > 0 && found->second.text[prefixStart - 1] == '.') {
            std::size_t aliasEnd = prefixStart - 1;
            std::size_t aliasStart = aliasEnd;
            while (aliasStart > 0) {
                const unsigned char character
                    = static_cast<unsigned char>(found->second.text[aliasStart - 1]);
                if (!(std::isalnum(character) || character == '_')) {
                    break;
                }
                --aliasStart;
            }
            if (aliasStart != aliasEnd) {
                namespaceAlias = found->second.text.substr(aliasStart, aliasEnd - aliasStart);
            }
        }

        struct Candidate {
            const DeclarationRecord* declaration = nullptr;
            const EnumVariantDecl* variant = nullptr;
            const StructFieldDecl* field = nullptr;
            const char* keyword = nullptr;
            SourceRange keywordRange{};
        };
        std::vector<Candidate> candidates;
        const auto matchesPrefix = [&prefix](const std::string& name) {
            return name.size() >= prefix.size()
                && name.compare(0, prefix.size(), prefix) == 0;
        };
        const auto hasCandidateName = [&candidates](std::string_view name) {
            for (const Candidate& candidate : candidates) {
                if (candidate.variant && candidate.variant->name.lexeme == name) {
                    return true;
                }
                if (candidate.field && candidate.field->name.lexeme == name) {
                    return true;
                }
                if (candidate.declaration && candidate.declaration->name == name) {
                    return true;
                }
                if (candidate.keyword && std::string_view(candidate.keyword) == name) {
                    return true;
                }
            }
            return false;
        };
        const ModuleStmt* module = moduleForSource(snapshot, found->second.analysis.sourceId);
        bool matchedQualifiedType = false;
        if (receiverPath && module) {
            if (const std::optional<DefinitionTarget> type = typeDefinitionForPath(
                    snapshot,
                    *module,
                    *receiverPath);
                type && isOpenSource(type->sourceId)
                && type->declaration && type->declaration->statement
                && isTypeDeclaration(type->declaration)) {
                matchedQualifiedType = true;
                const auto* enumDeclaration
                    = dynamic_cast<const EnumDeclStmt*>(type->declaration->statement);
                if (enumDeclaration) {
                    for (const EnumVariantDecl& variant : enumDeclaration->variants) {
                        if (variant.name.range && variant.name.range->valid()
                            && matchesPrefix(variant.name.lexeme)) {
                            candidates.push_back(Candidate{nullptr, &variant, nullptr});
                        }
                    }
                }
            }
        }
        bool matchedStructFields = false;
        if (!matchedQualifiedType && receiverPath && module) {
            if (const StructDeclStmt* structDeclaration = structFieldCompletionTargetAt(
                    snapshot,
                    found->second.analysis.sourceId,
                    *byte)) {
                matchedStructFields = true;
                const SourceFileId structSource = structDeclaration->name.range
                    ? structDeclaration->name.range->source
                    : SourceFileId{};
                if (isOpenSource(structSource)) {
                    const bool sameModule = structSource == module->sourceId;
                    for (const StructFieldDecl& field : structDeclaration->fields) {
                        if ((!field.isPrivate || sameModule)
                            && field.name.range && field.name.range->valid()
                            && matchesPrefix(field.name.lexeme)) {
                            candidates.push_back(Candidate{nullptr, nullptr, &field});
                        }
                    }
                    for (const DeclarationRecord& declaration
                         : snapshot.declarationIndex.declarations()) {
                        if (declaration.kind != DeclarationKind::Method
                            || declaration.ownerType != structDeclaration->name.lexeme
                            || !declaration.range
                            || declaration.range->source != structSource
                            || !matchesPrefix(declaration.name)) {
                            continue;
                        }
                        candidates.push_back(Candidate{&declaration, nullptr, nullptr});
                    }
                }
            } else if (const StructDeclStmt* structDeclaration = structMethodCompletionTargetAt(
                           snapshot,
                           found->second.analysis.sourceId,
                           *byte)) {
                matchedStructFields = true;
                const SourceFileId structSource = structDeclaration->name.range
                    ? structDeclaration->name.range->source
                    : SourceFileId{};
                if (isOpenSource(structSource)) {
                    for (const DeclarationRecord& declaration
                         : snapshot.declarationIndex.declarations()) {
                        if (declaration.kind != DeclarationKind::Method
                            || declaration.ownerType != structDeclaration->name.lexeme
                            || !declaration.range
                            || declaration.range->source != structSource
                            || !matchesPrefix(declaration.name)) {
                            continue;
                        }
                        candidates.push_back(Candidate{&declaration, nullptr, nullptr});
                    }
                }
            }
        }
        bool matchedNamespaceAlias = false;
        if (!matchedQualifiedType && !matchedStructFields && namespaceAlias && module) {
            for (const StmtPtr& statement : module->statements) {
                const auto* import = dynamic_cast<const ImportStmt*>(statement.get());
                if (!import || !import->alias
                    || import->alias->lexeme != *namespaceAlias
                    || import->resolvedModuleId == static_cast<std::size_t>(-1)) {
                    continue;
                }
                matchedNamespaceAlias = true;
                std::unordered_set<std::string> importedNames;
                for (const auto& exported : exportedDefinitionsForModule(
                        snapshot,
                        import->resolvedModuleId)) {
                    if (!isOpenSource(exported.second.sourceId)
                        || !matchesPrefix(exported.first)
                        || !importedNames.insert(exported.first).second) {
                        continue;
                    }
                    candidates.push_back(Candidate{exported.second.declaration, nullptr, nullptr});
                }
            }
        }

        if (!matchedQualifiedType && !matchedStructFields && !matchedNamespaceAlias) {
            for (const DeclarationRecord& declaration
                 : snapshot.declarationIndex.declarations()) {
                if (!declarationRange(declaration)
                    || declaration.range->source != found->second.analysis.sourceId
                    || declaration.kind == DeclarationKind::Module
                    || !matchesPrefix(declaration.name)) {
                    continue;
                }
                candidates.push_back(Candidate{&declaration, nullptr, nullptr});
            }

            if (module) {
                std::unordered_set<std::string> importedNames;
                for (const StmtPtr& statement : module->statements) {
                    const auto* import = dynamic_cast<const ImportStmt*>(statement.get());
                    if (!import || import->alias
                        || import->resolvedModuleId == static_cast<std::size_t>(-1)) {
                        continue;
                    }
                    for (const auto& exported : exportedDefinitionsForModule(
                            snapshot,
                            import->resolvedModuleId)) {
                        if (!isOpenSource(exported.second.sourceId)
                            || !matchesPrefix(exported.first)
                            || !importedNames.insert(exported.first).second) {
                            continue;
                        }
                        candidates.push_back(Candidate{exported.second.declaration, nullptr, nullptr});
                    }
                }
            }

            if (!receiverPath && snapshot.program) {
                for (const StmtPtr& statement : snapshot.program->statements) {
                    const auto* workspaceModule = dynamic_cast<const ModuleStmt*>(statement.get());
                    if (!workspaceModule
                        || workspaceModule->sourceId == found->second.analysis.sourceId
                        || !isOpenSource(workspaceModule->sourceId)) {
                        continue;
                    }
                    for (const auto& exported : exportedDefinitionsForModule(
                            snapshot,
                            workspaceModule->moduleId)) {
                        if (!isOpenSource(exported.second.sourceId)
                            || !matchesPrefix(exported.first)
                            || hasCandidateName(exported.first)) {
                            continue;
                        }
                        candidates.push_back(Candidate{
                            exported.second.declaration,
                            nullptr,
                            nullptr});
                    }
                }
            }
            for (const char* keyword : completionKeywordNames()) {
                if (matchesPrefix(keyword) && !hasCandidateName(keyword)) {
                    candidates.push_back(Candidate{nullptr, nullptr, nullptr, keyword, replaceRange});
                }
            }
        }
        const auto candidateName = [](const Candidate& candidate) -> std::string_view {
            if (candidate.variant) {
                return candidate.variant->name.lexeme;
            }
            if (candidate.field) {
                return candidate.field->name.lexeme;
            }
            if (candidate.keyword) {
                return std::string_view(candidate.keyword);
            }
            return candidate.declaration->name;
        };
        const auto candidateRange = [](const Candidate& candidate) -> const SourceRange& {
            if (candidate.variant) {
                return *candidate.variant->name.range;
            }
            if (candidate.field) {
                return *candidate.field->name.range;
            }
            if (candidate.keyword) {
                return candidate.keywordRange;
            }
            return *candidate.declaration->range;
        };
        std::sort(
            candidates.begin(),
            candidates.end(),
            [&](const Candidate& left, const Candidate& right) {
                const std::string_view leftName = candidateName(left);
                const std::string_view rightName = candidateName(right);
                if (leftName != rightName) {
                    return leftName < rightName;
                }
                const SourceRange& leftRange = candidateRange(left);
                const SourceRange& rightRange = candidateRange(right);
                if (leftRange.source != rightRange.source) {
                    return leftRange.source < rightRange.source;
                }
                if (leftRange.start != rightRange.start) {
                    return leftRange.start < rightRange.start;
                }
                return leftRange.end < rightRange.end;
            });

        std::vector<JsonValue> items;
        items.reserve(candidates.size());
        for (const Candidate& candidate : candidates) {
            const std::string label(candidateName(candidate));
            items.push_back(makeObject({
                {"label", JsonValue::string(label)},
                {"kind", JsonValue::number(std::to_string(
                    candidate.keyword
                        ? 14
                        : candidate.variant
                        ? 20
                        : candidate.field ? 5 : completionItemKind(candidate.declaration->kind)))},
                {"detail", JsonValue::string(candidate.keyword
                        ? "keyword"
                        : candidate.variant
                        ? "variant"
                        : candidate.field
                            ? "field"
                            : completionDetail(snapshot.declarationIndex, *candidate.declaration))},
                {"textEdit", makeObject({
                    {"range", textDocumentRange(found->second.text, replaceRange)},
                    {"newText", JsonValue::string(label)},
                })},
            }));
        }
        return makeObject({
            {"isIncomplete", JsonValue::booleanValue(false)},
            {"items", JsonValue::array(std::move(items))},
        });
    }

    JsonValue handleWorkspaceSymbols(const JsonValue& request) const
    {
        const JsonValue* params = memberObject(request, "params");
        const std::string query = params
            ? stringMember(*params, "query").value_or("")
            : "";
        struct Candidate {
            const std::string* uri = nullptr;
            const Document* document = nullptr;
            const DeclarationRecord* declaration = nullptr;
        };
        std::vector<Candidate> candidates;
        for (const auto& entry : documents_) {
            const Document& document = entry.second;
            if (!document.analysis.snapshot || !document.analysis.snapshot->program) {
                continue;
            }
            const AnalysisSnapshot& snapshot = *document.analysis.snapshot;
            for (const DeclarationRecord& declaration
                 : snapshot.declarationIndex.declarations()) {
                if (declaration.kind == DeclarationKind::Module
                    || !declarationRange(declaration)
                    || declaration.range->source != document.analysis.sourceId
                    || (!query.empty() && declaration.name.find(query) == std::string::npos)) {
                    continue;
                }
                candidates.push_back(Candidate{&entry.first, &document, &declaration});
            }
        }
        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                if (*left.uri != *right.uri) {
                    return *left.uri < *right.uri;
                }
                if (left.declaration->range->start != right.declaration->range->start) {
                    return left.declaration->range->start < right.declaration->range->start;
                }
                if (left.declaration->range->end != right.declaration->range->end) {
                    return left.declaration->range->end < right.declaration->range->end;
                }
                return left.declaration->name < right.declaration->name;
            });

        std::vector<JsonValue> symbols;
        symbols.reserve(candidates.size());
        for (const Candidate& candidate : candidates) {
            symbols.push_back(makeObject({
                {"name", JsonValue::string(candidate.declaration->name)},
                {"kind", JsonValue::number(std::to_string(
                    lspSymbolKind(candidate.declaration->kind)))},
                {"location", sourceLocation(
                    *candidate.uri,
                    candidate.document->text,
                    *candidate.declaration->range)},
            }));
        }
        return JsonValue::array(std::move(symbols));
    }

    JsonValue handleDocumentSymbols(const JsonValue& request) const
    {
        const std::optional<std::string> uri = documentUri(request);
        if (!uri) {
            return JsonValue::array({});
        }
        const auto found = documents_.find(*uri);
        if (found == documents_.end()
            || !found->second.analysis.snapshot
            || !found->second.analysis.snapshot->program) {
            return JsonValue::array({});
        }
        const AnalysisSnapshot& snapshot = *found->second.analysis.snapshot;

        std::vector<const DeclarationRecord*> declarations;
        for (const DeclarationRecord& declaration : snapshot.declarationIndex.declarations()) {
            if (declarationRange(declaration)
                && declaration.range->source == found->second.analysis.sourceId) {
                declarations.push_back(&declaration);
            }
        }
        std::sort(
            declarations.begin(),
            declarations.end(),
            [](const DeclarationRecord* left, const DeclarationRecord* right) {
                if (left->range->start != right->range->start) {
                    return left->range->start < right->range->start;
                }
                if (left->range->end != right->range->end) {
                    return left->range->end < right->range->end;
                }
                return left->name < right->name;
            });

        std::vector<JsonValue> symbols;
        symbols.reserve(declarations.size());
        for (const DeclarationRecord* declaration : declarations) {
            symbols.push_back(documentSymbol(found->second.text, *declaration));
        }
        return JsonValue::array(std::move(symbols));
    }

    std::map<std::string, Document> documents_;
    std::vector<std::string> workspaceRoots_;
    bool shuttingDown_ = false;
};

} // namespace

int runLanguageServer(std::istream& input, std::ostream& output)
{
    LanguageServer server;
    return server.run(input, output);
}
