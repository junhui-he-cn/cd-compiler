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
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
        if (const auto* print = dynamic_cast<const PrintStmt*>(statement)) {
            visitExpression(print->expression.get());
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
        if (const auto* whileStatement = dynamic_cast<const WhileStmt*>(statement)) {
            visitExpression(whileStatement->condition.get());
            visitStatement(whileStatement->body.get());
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
        if (const auto* match = dynamic_cast<const MatchStmt*>(statement)) {
            visitExpression(match->value.get());
            for (const MatchArm& arm : match->arms) {
                visitExpression(arm.guard.get());
                visitStatement(arm.body.get());
            }
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
        if (const auto* match = dynamic_cast<const MatchExpr*>(expression)) {
            visitExpression(match->value.get());
            for (const MatchExprArm& arm : match->arms) {
                visitExpression(arm.guard.get());
                visitExpression(arm.value.get());
            }
        }
    }

    std::vector<ReferenceSite> sites_;
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

bool rangeContains(const SourceRange& range, std::size_t byte)
{
    return range.source.valid() && range.source.value == 0
        && range.start <= byte && byte < range.end;
}

std::optional<SourceRange> declarationRange(const DeclarationRecord& declaration)
{
    if (!declaration.range || !declaration.range->valid()
        || declaration.range->source.value != 0) {
        return std::nullopt;
    }
    return declaration.range;
}

struct DocumentAnalysis {
    std::vector<FileDiagnosticError> diagnostics;
    std::optional<Program> program;
    DeclarationIndex declarationIndex;
    std::vector<ReferenceSite> referenceSites;
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

JsonValue diagnosticValue(const FileDiagnosticError& error, std::string_view source)
{
    LspPosition start = diagnosticFallbackPosition(error);
    LspPosition end = start;
    if (error.range()
        && error.range()->source.valid()
        && error.range()->source.value == 0
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
    std::istringstream input(source);
    TypeChecker typeChecker;
    try {
        FrontendSession frontend;
        analysis.program.emplace(frontend.loadStdin(input));
        analysis.declarationIndex = DeclarationIndex::collect(*analysis.program);
        ReferenceSiteCollector referenceCollector;
        analysis.referenceSites = referenceCollector.collect(*analysis.program);
        typeChecker.check(*analysis.program);
        analysis.declarationIndex = typeChecker.declarationIndex();
    } catch (const TypeErrorList& errors) {
        analysis.declarationIndex = typeChecker.declarationIndex();
        analysis.diagnostics = errors.errors();
    } catch (const FileDiagnosticErrorList& errors) {
        analysis.diagnostics = errors.errors();
    } catch (const ParseErrorList& errors) {
        analysis.diagnostics.reserve(errors.errors().size());
        for (const ParseError& error : errors.errors()) {
            analysis.diagnostics.emplace_back(
                error,
                DiagnosticSourceContext{"<stdin>", source, true});
        }
    } catch (const LexErrorList& errors) {
        analysis.diagnostics.reserve(errors.errors().size());
        for (const DiagnosticError& error : errors.errors()) {
            analysis.diagnostics.emplace_back(
                error,
                DiagnosticSourceContext{"<stdin>", source, true});
        }
    } catch (const FileDiagnosticError& error) {
        analysis.diagnostics.push_back(error);
    } catch (const DiagnosticError& error) {
        analysis.diagnostics.emplace_back(
            error,
            DiagnosticSourceContext{"<stdin>", source, true});
    } catch (const std::exception& error) {
        DiagnosticError diagnostic(DiagnosticKind::Compile, error.what());
        analysis.diagnostics.emplace_back(
            diagnostic,
            DiagnosticSourceContext{"<stdin>", source, true});
    }
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
    }
    return std::nullopt;
}

const DeclarationRecord* definitionAt(
    const DocumentAnalysis& analysis,
    std::size_t byte)
{
    const DeclarationRecord* best = nullptr;
    std::size_t bestWidth = std::numeric_limits<std::size_t>::max();
    const auto consider = [&](const DeclarationRecord* declaration) {
        if (!declaration) {
            return;
        }
        const std::optional<SourceRange> range = declarationRange(*declaration);
        if (!range || !rangeContains(*range, byte)
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

    for (const DeclarationRecord& declaration : analysis.declarationIndex.declarations()) {
        consider(&declaration);
    }

    for (const ReferenceSite& site : analysis.referenceSites) {
        if (!site.range || !rangeContains(*site.range, byte)) {
            continue;
        }
        const std::optional<ResolvedSymbol> resolved
            = resolvedReference(analysis.declarationIndex, site);
        if (resolved) {
            const DeclarationRecord* target
                = analysis.declarationIndex.declaration(resolved->declarationId);
            if (target && declarationRange(*target)) {
                return target;
            }
        }
    }
    return best;
}

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

std::vector<SourceRange> referenceRangesAt(
    const DocumentAnalysis& analysis,
    std::size_t byte,
    bool includeDeclaration)
{
    const DeclarationRecord* target = definitionAt(analysis, byte);
    if (!target) {
        return {};
    }

    std::vector<SourceRange> ranges;
    if (includeDeclaration && target->range && declarationRange(*target)) {
        ranges.push_back(*target->range);
    }
    for (const ReferenceSite& site : analysis.referenceSites) {
        const std::optional<ResolvedSymbol> resolved
            = resolvedReference(analysis.declarationIndex, site);
        if (!resolved || resolved->declarationId != target->declarationId || !site.range) {
            continue;
        }
        ranges.push_back(*site.range);
    }

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
    return ranges;
}

struct HoverInfo {
    TypeInfo type;
    SourceRange range;
};

std::optional<HoverInfo> hoverInfoAt(
    const DocumentAnalysis& analysis,
    std::size_t byte)
{
    const ReferenceSite* bestSite = nullptr;
    const TypedExpressionRecord* bestType = nullptr;
    std::size_t bestWidth = std::numeric_limits<std::size_t>::max();
    for (const ReferenceSite& site : analysis.referenceSites) {
        if (!site.expression || !site.range || !rangeContains(*site.range, byte)) {
            continue;
        }
        const TypedExpressionRecord* typed
            = analysis.declarationIndex.typedExpression(*site.expression);
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

    const DeclarationRecord* declaration = definitionAt(analysis, byte);
    if (!declaration || !declaration->range) {
        return std::nullopt;
    }
    if (const ResolvedSignatureRecord* signature
        = analysis.declarationIndex.resolvedSignature(declaration->declarationId)) {
        return HoverInfo{signature->type, *declaration->range};
    }
    if (const auto* let = dynamic_cast<const LetStmt*>(declaration->statement)) {
        if (let->initializer) {
            if (const TypedExpressionRecord* typed
                = analysis.declarationIndex.typedExpression(*let->initializer)) {
                return HoverInfo{typed->type, *declaration->range};
            }
        }
    }
    for (const ReferenceSite& site : analysis.referenceSites) {
        const std::optional<ResolvedSymbol> resolved
            = resolvedReference(analysis.declarationIndex, site);
        if (!resolved || resolved->declarationId != declaration->declarationId
            || !site.expression) {
            continue;
        }
        if (const TypedExpressionRecord* typed
            = analysis.declarationIndex.typedExpression(*site.expression)) {
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
    const std::vector<FileDiagnosticError>& diagnostics)
{
    std::vector<JsonValue> values;
    values.reserve(diagnostics.size());
    for (const FileDiagnosticError& diagnostic : diagnostics) {
        values.push_back(diagnosticValue(diagnostic, source));
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
                                    {"completionProvider", JsonValue::booleanValue(true)},
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

    void publish(std::ostream& output, const std::string& uri, const Document& document)
    {
        writeMessage(
            output,
            publishDiagnostics(uri, document.text, document.analysis.diagnostics));
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
        state.analysis = analyzeDocument(state.text);
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
        document.analysis = analyzeDocument(document.text);
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
        publish(output, *uri, document);
    }

    void handleDidClose(const JsonValue& request, std::ostream& output)
    {
        const std::optional<std::string> uri = documentUri(request);
        if (!uri) {
            return;
        }
        documents_.erase(*uri);
        writeMessage(
            output,
            publishDiagnostics(*uri, "", {}));
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
        if (found == documents_.end() || !found->second.analysis.program) {
            return JsonValue::null();
        }
        const std::optional<std::size_t> byte = sourceByteAtLspPosition(
            found->second.text,
            position->line,
            position->character);
        if (!byte) {
            return JsonValue::null();
        }
        const DeclarationRecord* declaration = definitionAt(
            found->second.analysis,
            *byte);
        if (!declaration || !declaration->range) {
            return JsonValue::null();
        }
        return definitionLocation(*uri, found->second.text, *declaration);
    }

    JsonValue handleReferences(const JsonValue& request) const
    {
        const std::optional<std::string> uri = documentUri(request);
        const std::optional<LspRequestPosition> position = requestPosition(request);
        if (!uri || !position) {
            return JsonValue::array({});
        }
        const auto found = documents_.find(*uri);
        if (found == documents_.end() || !found->second.analysis.program) {
            return JsonValue::array({});
        }
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
            found->second.analysis,
            *byte,
            includeDeclaration);
        std::vector<JsonValue> locations;
        locations.reserve(ranges.size());
        for (const SourceRange& range : ranges) {
            locations.push_back(sourceLocation(*uri, found->second.text, range));
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
        if (found == documents_.end() || !found->second.analysis.program) {
            return JsonValue::null();
        }
        const std::optional<std::size_t> byte = sourceByteAtLspPosition(
            found->second.text,
            position->line,
            position->character);
        if (!byte) {
            return JsonValue::null();
        }
        const std::optional<HoverInfo> info = hoverInfoAt(found->second.analysis, *byte);
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
        if (found == documents_.end() || !found->second.analysis.program
            || !found->second.analysis.diagnostics.empty()) {
            return JsonValue::null();
        }
        const std::optional<std::size_t> byte = sourceByteAtLspPosition(
            found->second.text,
            position->line,
            position->character);
        if (!byte) {
            return JsonValue::null();
        }
        const std::vector<SourceRange> ranges = referenceRangesAt(
            found->second.analysis,
            *byte,
            true);
        if (ranges.empty()) {
            return JsonValue::null();
        }

        std::string renamed = found->second.text;
        for (auto range = ranges.rbegin(); range != ranges.rend(); ++range) {
            if (!range->source.valid() || range->source.value != 0
                || range->start > range->end || range->end > renamed.size()) {
                return JsonValue::null();
            }
            renamed.replace(range->start, range->end - range->start, *newName);
        }
        const DocumentAnalysis renamedAnalysis = analyzeDocument(renamed);
        if (!renamedAnalysis.diagnostics.empty()) {
            return JsonValue::null();
        }

        std::vector<JsonValue> edits;
        edits.reserve(ranges.size());
        for (const SourceRange& range : ranges) {
            edits.push_back(makeObject({
                {"range", textDocumentRange(found->second.text, range)},
                {"newText", JsonValue::string(*newName)},
            }));
        }
        JsonValue::Object changes;
        changes.emplace(*uri, JsonValue::array(std::move(edits)));
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
        if (found == documents_.end() || !found->second.analysis.program) {
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
        const SourceRange replaceRange{SourceFileId{0}, prefixStart, *byte};
        std::vector<const DeclarationRecord*> declarations;
        for (const DeclarationRecord& declaration
             : found->second.analysis.declarationIndex.declarations()) {
            if (!declarationRange(declaration)
                || declaration.kind == DeclarationKind::Module
                || (declaration.name.size() < prefix.size())
                || declaration.name.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            declarations.push_back(&declaration);
        }
        std::sort(
            declarations.begin(),
            declarations.end(),
            [](const DeclarationRecord* left, const DeclarationRecord* right) {
                if (left->name != right->name) {
                    return left->name < right->name;
                }
                if (left->range->start != right->range->start) {
                    return left->range->start < right->range->start;
                }
                return left->range->end < right->range->end;
            });

        std::vector<JsonValue> items;
        items.reserve(declarations.size());
        for (const DeclarationRecord* declaration : declarations) {
            items.push_back(makeObject({
                {"label", JsonValue::string(declaration->name)},
                {"kind", JsonValue::number(std::to_string(completionItemKind(declaration->kind)))},
                {"detail", JsonValue::string(completionDetail(
                    found->second.analysis.declarationIndex,
                    *declaration))},
                {"textEdit", makeObject({
                    {"range", textDocumentRange(found->second.text, replaceRange)},
                    {"newText", JsonValue::string(declaration->name)},
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
            if (!document.analysis.program) {
                continue;
            }
            for (const DeclarationRecord& declaration
                 : document.analysis.declarationIndex.declarations()) {
                if (declaration.kind == DeclarationKind::Module
                    || !declarationRange(declaration)
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
        if (found == documents_.end() || !found->second.analysis.program) {
            return JsonValue::array({});
        }

        std::vector<const DeclarationRecord*> declarations;
        for (const DeclarationRecord& declaration : found->second.analysis.declarationIndex.declarations()) {
            if (declarationRange(declaration)) {
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
    bool shuttingDown_ = false;
};

} // namespace

int runLanguageServer(std::istream& input, std::ostream& output)
{
    LanguageServer server;
    return server.run(input, output);
}
