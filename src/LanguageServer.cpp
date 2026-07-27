#include "LanguageServer.hpp"

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

std::vector<FileDiagnosticError> analyzeDocument(const std::string& source)
{
    std::vector<FileDiagnosticError> diagnostics;
    std::istringstream input(source);
    try {
        FrontendSession frontend;
        Program program = frontend.loadStdin(input);
        TypeChecker typeChecker;
        typeChecker.check(program);
    } catch (const TypeErrorList& errors) {
        diagnostics = errors.errors();
    } catch (const FileDiagnosticErrorList& errors) {
        diagnostics = errors.errors();
    } catch (const ParseErrorList& errors) {
        diagnostics.reserve(errors.errors().size());
        for (const ParseError& error : errors.errors()) {
            diagnostics.emplace_back(
                error,
                DiagnosticSourceContext{"<stdin>", source, true});
        }
    } catch (const LexErrorList& errors) {
        diagnostics.reserve(errors.errors().size());
        for (const DiagnosticError& error : errors.errors()) {
            diagnostics.emplace_back(
                error,
                DiagnosticSourceContext{"<stdin>", source, true});
        }
    } catch (const FileDiagnosticError& error) {
        diagnostics.push_back(error);
    } catch (const DiagnosticError& error) {
        diagnostics.emplace_back(
            error,
            DiagnosticSourceContext{"<stdin>", source, true});
    } catch (const std::exception& error) {
        DiagnosticError diagnostic(DiagnosticKind::Compile, error.what());
        diagnostics.emplace_back(
            diagnostic,
            DiagnosticSourceContext{"<stdin>", source, true});
    }
    return diagnostics;
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
            if (id) {
                writeMessage(output, errorResponse(*id, -32601, "method not found"));
            }
        }
    }

private:
    struct Document {
        std::string text;
        std::int64_t version = 0;
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

    void publish(
        std::ostream& output,
        const std::string& uri,
        const std::string& source)
    {
        writeMessage(output, publishDiagnostics(uri, source, analyzeDocument(source)));
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
        documents_[*uri] = Document{*text, 0};
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
        publish(output, *uri, *text);
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
        publish(output, *uri, document.text);
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

    std::map<std::string, Document> documents_;
    bool shuttingDown_ = false;
};

} // namespace

int runLanguageServer(std::istream& input, std::ostream& output)
{
    LanguageServer server;
    return server.run(input, output);
}
