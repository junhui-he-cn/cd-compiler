#include "Formatter.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum class DelimiterKind {
    Parenthesis,
    Bracket,
    Brace,
    Angle,
};

struct Delimiter {
    DelimiterKind kind;
    bool empty = false;
    std::size_t baseIndent = 0;
    bool wrapped = false;
};

// Line width is measured in emitted source bytes. Strings and comments are
// never split; this width only decides whether a delimited comma list wraps.
constexpr std::size_t kCanonicalLineWidth = 100;

bool isKeyword(TokenType type)
{
    switch (type) {
    case TokenType::Break:
    case TokenType::Continue:
    case TokenType::For:
    case TokenType::If:
    case TokenType::Impl:
    case TokenType::Import:
    case TokenType::In:
    case TokenType::As:
    case TokenType::Export:
    case TokenType::Else:
    case TokenType::Enum:
    case TokenType::Fun:
    case TokenType::Let:
    case TokenType::Match:
    case TokenType::Print:
    case TokenType::Private:
    case TokenType::Return:
    case TokenType::Struct:
    case TokenType::While:
    case TokenType::True:
    case TokenType::False:
    case TokenType::Nil:
        return true;
    default:
        return false;
    }
}

bool isWordLike(TokenType type)
{
    return type == TokenType::Identifier
        || type == TokenType::Number
        || type == TokenType::String
        || isKeyword(type);
}

bool isBinaryOperator(TokenType type)
{
    switch (type) {
    case TokenType::Plus:
    case TokenType::Minus:
    case TokenType::Star:
    case TokenType::Slash:
    case TokenType::PlusEqual:
    case TokenType::MinusEqual:
    case TokenType::StarEqual:
    case TokenType::SlashEqual:
    case TokenType::BangEqual:
    case TokenType::Equal:
    case TokenType::EqualEqual:
    case TokenType::Less:
    case TokenType::LessEqual:
    case TokenType::Greater:
    case TokenType::GreaterEqual:
    case TokenType::AmpersandAmpersand:
    case TokenType::Pipe:
    case TokenType::PipePipe:
    case TokenType::FatArrow:
        return true;
    default:
        return false;
    }
}

bool isUnaryMinus(const std::vector<const Token*>& tokens, std::size_t index)
{
    if (index == 0) {
        return true;
    }

    switch (tokens[index - 1]->type) {
    case TokenType::LeftParen:
    case TokenType::LeftBracket:
    case TokenType::LeftBrace:
    case TokenType::Comma:
    case TokenType::Colon:
    case TokenType::Equal:
    case TokenType::Plus:
    case TokenType::Minus:
    case TokenType::Star:
    case TokenType::Slash:
    case TokenType::PlusEqual:
    case TokenType::MinusEqual:
    case TokenType::StarEqual:
    case TokenType::SlashEqual:
    case TokenType::Bang:
    case TokenType::BangEqual:
    case TokenType::EqualEqual:
    case TokenType::Less:
    case TokenType::LessEqual:
    case TokenType::Greater:
    case TokenType::GreaterEqual:
    case TokenType::AmpersandAmpersand:
    case TokenType::Pipe:
    case TokenType::PipePipe:
    case TokenType::FatArrow:
        return true;
    default:
        return tokens[index - 1]->type == TokenType::Return;
    }
}

bool rangesAreAdjacent(const Token& left, const Token& right)
{
    const std::size_t leftEnd = left.range ? left.range->end : left.endOffset;
    const std::size_t rightStart = right.range ? right.range->start : right.startOffset;
    return leftEnd == rightStart;
}

bool genericCloseCanBeFollowedBy(TokenType type)
{
    switch (type) {
    case TokenType::LeftParen:
    case TokenType::LeftBrace:
    case TokenType::Question:
    case TokenType::Comma:
    case TokenType::RightParen:
    case TokenType::RightBracket:
    case TokenType::Colon:
    case TokenType::Equal:
    case TokenType::Semicolon:
    case TokenType::RightBrace:
    case TokenType::Dot:
    case TokenType::Greater:
        return true;
    default:
        return false;
    }
}

std::vector<bool> findGenericAngles(const std::vector<const Token*>& tokens)
{
    std::vector<bool> genericOpen(tokens.size(), false);
    std::vector<bool> genericClose(tokens.size(), false);

    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (tokens[index]->type != TokenType::Less || index == 0) {
            continue;
        }

        const Token& previous = *tokens[index - 1];
        const bool canStartAfterFun = previous.type == TokenType::Fun;
        const bool canStartAdjacent = isWordLike(previous.type)
            || previous.type == TokenType::RightBracket
            || previous.type == TokenType::RightParen
            || previous.type == TokenType::Greater;
        if (!canStartAfterFun && !canStartAdjacent) {
            continue;
        }

        // Explicit generic calls require adjacency in the parser.  Keeping
        // that signal prevents `left < right > (value)` from being rewritten
        // as a generic call, while still recognizing declarations and the
        // compact type forms used by the language.
        if (!canStartAfterFun && !rangesAreAdjacent(previous, *tokens[index])) {
            continue;
        }

        int depth = 0;
        std::size_t closeIndex = std::numeric_limits<std::size_t>::max();
        for (std::size_t cursor = index; cursor < tokens.size(); ++cursor) {
            if (tokens[cursor]->type == TokenType::Less) {
                ++depth;
                continue;
            }
            if (tokens[cursor]->type != TokenType::Greater) {
                continue;
            }
            --depth;
            if (depth == 0) {
                closeIndex = cursor;
                break;
            }
            if (depth < 0) {
                break;
            }
        }

        if (closeIndex == std::numeric_limits<std::size_t>::max()
            || (closeIndex + 1 < tokens.size()
                && !genericCloseCanBeFollowedBy(tokens[closeIndex + 1]->type))) {
            continue;
        }

        genericOpen[index] = true;
        genericClose[closeIndex] = true;
    }

    return [&]() {
        std::vector<bool> result(tokens.size(), false);
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            result[index] = genericOpen[index] || genericClose[index];
        }
        return result;
    }();
}

struct FormatterState {
    std::string output;
    std::size_t indentWidth = 2;
    std::size_t indent = 0;
    bool lineStart = true;
    bool pendingLineBreak = false;
    bool pendingClose = false;
    bool sourceNewlineSinceLastItem = false;
    bool sourceBlankLineSinceLastItem = false;
    bool inForHeader = false;
    std::vector<Delimiter> delimiters;
    std::optional<TokenType> previous;

    void trimTrailingSpaces()
    {
        while (!output.empty() && output.back() == ' ') {
            output.pop_back();
        }
    }

    void writeIndentIfNeeded()
    {
        if (!lineStart) {
            return;
        }
        output.append(indent * indentWidth, ' ');
        lineStart = false;
    }

    void writeRaw(std::string_view text)
    {
        if (text.empty()) {
            return;
        }
        writeIndentIfNeeded();
        output.append(text);
    }

    void space()
    {
        if (lineStart || output.empty() || output.back() == '\n' || output.back() == ' ') {
            return;
        }
        output.push_back(' ');
    }

    void newline()
    {
        trimTrailingSpaces();
        if (output.empty() || output.back() != '\n') {
            output.push_back('\n');
        }
        lineStart = true;
    }

    void ensureBlankLine()
    {
        trimTrailingSpaces();
        if (output.empty()) {
            return;
        }
        if (!lineStart) {
            output.push_back('\n');
            lineStart = true;
        }
        output.push_back('\n');
        lineStart = true;
    }

    std::size_t currentLineColumn() const
    {
        const std::size_t newline = output.find_last_of('\n');
        return newline == std::string::npos
            ? output.size()
            : output.size() - newline - 1;
    }

    void wrapTopList()
    {
        if (delimiters.empty()) {
            return;
        }
        Delimiter& delimiter = delimiters.back();
        if (delimiter.kind != DelimiterKind::Parenthesis
            && delimiter.kind != DelimiterKind::Bracket) {
            return;
        }
        delimiter.wrapped = true;
        newline();
        indent = delimiter.baseIndent + 1;
    }

    void flushLineBreak()
    {
        if (pendingLineBreak) {
            newline();
            pendingLineBreak = false;
        }
    }

    bool hasTopDelimiter(DelimiterKind kind) const
    {
        return !delimiters.empty() && delimiters.back().kind == kind;
    }

    bool hasDelimiter(DelimiterKind kind) const
    {
        return std::any_of(
            delimiters.rbegin(),
            delimiters.rend(),
            [kind](const Delimiter& delimiter) { return delimiter.kind == kind; });
    }
};

bool tokenIsPunctuationWithoutLeadingSpace(TokenType type)
{
    switch (type) {
    case TokenType::RightParen:
    case TokenType::RightBracket:
    case TokenType::RightBrace:
    case TokenType::Comma:
    case TokenType::Semicolon:
    case TokenType::Dot:
    case TokenType::Question:
        return true;
    default:
        return false;
    }
}

bool canFollowClosingBraceOnSameLine(TokenType type)
{
    return type == TokenType::Else
        || type == TokenType::Semicolon
        || type == TokenType::Comma
        || type == TokenType::RightParen
        || type == TokenType::RightBracket
        || type == TokenType::RightBrace
        || type == TokenType::Dot
        || type == TokenType::Question
        || type == TokenType::LeftParen
        || type == TokenType::LeftBracket
        || isBinaryOperator(type);
}

bool isListClosing(DelimiterKind kind, TokenType type)
{
    return (kind == DelimiterKind::Parenthesis && type == TokenType::RightParen)
        || (kind == DelimiterKind::Bracket && type == TokenType::RightBracket);
}

std::size_t estimatedListContentWidth(
    const std::vector<const Token*>& tokens,
    const std::vector<bool>& genericAngles,
    std::size_t openIndex,
    DelimiterKind enclosing)
{
    int parenthesisDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    int angleDepth = 0;
    std::size_t width = 0;

    for (std::size_t index = openIndex + 1; index < tokens.size(); ++index) {
        const TokenType type = tokens[index]->type;
        const bool atEnclosingDepth = parenthesisDepth == 0
            && bracketDepth == 0
            && braceDepth == 0
            && angleDepth == 0;
        if (atEnclosingDepth && isListClosing(enclosing, type)) {
            return width;
        }

        // Add a conservative separator allowance so operators and colons do
        // not make a supposedly fitting list cross the canonical width.
        width += tokens[index]->lexeme.size() + 2;
        if (genericAngles[index] && type == TokenType::Less) {
            ++angleDepth;
        } else if (genericAngles[index] && type == TokenType::Greater) {
            if (angleDepth > 0) {
                --angleDepth;
            }
        } else {
            switch (type) {
            case TokenType::LeftParen:
                ++parenthesisDepth;
                break;
            case TokenType::RightParen:
                if (parenthesisDepth > 0) {
                    --parenthesisDepth;
                }
                break;
            case TokenType::LeftBracket:
                ++bracketDepth;
                break;
            case TokenType::RightBracket:
                if (bracketDepth > 0) {
                    --bracketDepth;
                }
                break;
            case TokenType::LeftBrace:
                ++braceDepth;
                break;
            case TokenType::RightBrace:
                if (braceDepth > 0) {
                    --braceDepth;
                }
                break;
            default:
                break;
            }
        }
    }
    return width;
}

bool containsTopLevelListComma(
    const std::vector<const Token*>& tokens,
    const std::vector<bool>& genericAngles,
    std::size_t openIndex,
    DelimiterKind enclosing)
{
    int parenthesisDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    int angleDepth = 0;

    for (std::size_t index = openIndex + 1; index < tokens.size(); ++index) {
        const TokenType type = tokens[index]->type;
        const bool atEnclosingDepth = parenthesisDepth == 0
            && bracketDepth == 0
            && braceDepth == 0
            && angleDepth == 0;
        if (atEnclosingDepth) {
            if (type == TokenType::Comma) {
                return true;
            }
            if (isListClosing(enclosing, type)) {
                return false;
            }
        }

        if (genericAngles[index] && type == TokenType::Less) {
            ++angleDepth;
        } else if (genericAngles[index] && type == TokenType::Greater) {
            if (angleDepth > 0) {
                --angleDepth;
            }
        } else {
            switch (type) {
            case TokenType::LeftParen:
                ++parenthesisDepth;
                break;
            case TokenType::RightParen:
                if (parenthesisDepth > 0) {
                    --parenthesisDepth;
                }
                break;
            case TokenType::LeftBracket:
                ++bracketDepth;
                break;
            case TokenType::RightBracket:
                if (bracketDepth > 0) {
                    --bracketDepth;
                }
                break;
            case TokenType::LeftBrace:
                ++braceDepth;
                break;
            case TokenType::RightBrace:
                if (braceDepth > 0) {
                    --braceDepth;
                }
                break;
            default:
                break;
            }
        }
    }
    return false;
}

bool shouldWrapListAtOpen(
    const FormatterState& state,
    const std::vector<const Token*>& tokens,
    const std::vector<bool>& genericAngles,
    std::size_t tokenIndex,
    DelimiterKind kind)
{
    if (!containsTopLevelListComma(tokens, genericAngles, tokenIndex, kind)) {
        return false;
    }
    const std::size_t contentWidth = estimatedListContentWidth(
        tokens,
        genericAngles,
        tokenIndex,
        kind);
    return contentWidth > 0
        && state.currentLineColumn() + 1 + contentWidth > kCanonicalLineWidth;
}

bool shouldWrapAfterComma(const FormatterState& state)
{
    if (state.delimiters.empty()) {
        return false;
    }
    const Delimiter& delimiter = state.delimiters.back();
    if (delimiter.kind != DelimiterKind::Parenthesis
        && delimiter.kind != DelimiterKind::Bracket) {
        return false;
    }
    return delimiter.wrapped;
}

bool shouldSpaceBeforeLeftParen(TokenType previous)
{
    return previous == TokenType::If
        || previous == TokenType::While
        || previous == TokenType::For
        || previous == TokenType::Match
        || previous == TokenType::Return
        || previous == TokenType::Print;
}

bool shouldSpaceBefore(const FormatterState& state, const Token& token, bool genericAngle)
{
    if (state.lineStart || !state.previous) {
        return false;
    }

    const TokenType previous = *state.previous;
    if (tokenIsPunctuationWithoutLeadingSpace(token.type)) {
        return false;
    }
    if (token.type == TokenType::LeftParen) {
        if (isWordLike(previous)) {
            return shouldSpaceBeforeLeftParen(previous);
        }
        return false;
    }
    if (token.type == TokenType::LeftBracket) {
        if (isWordLike(previous)) {
            return previous == TokenType::Return
                || previous == TokenType::Print
                || previous == TokenType::If
                || previous == TokenType::While
                || previous == TokenType::For
                || previous == TokenType::Match;
        }
        return false;
    }
    if (token.type == TokenType::LeftBrace) {
        return true;
    }
    if (genericAngle && token.type == TokenType::Less) {
        return false;
    }
    if (genericAngle && token.type == TokenType::Greater) {
        return false;
    }
    if (previous == TokenType::Dot || token.type == TokenType::Dot) {
        return false;
    }
    if (previous == TokenType::LeftParen
        || previous == TokenType::LeftBracket
        || previous == TokenType::LeftBrace) {
        return false;
    }
    if (previous == TokenType::Colon) {
        return true;
    }
    if (previous == TokenType::Comma) {
        return true;
    }
    if (previous == TokenType::FatArrow) {
        return true;
    }
    if (previous == TokenType::Bang) {
        return false;
    }
    if (isWordLike(previous) && isWordLike(token.type)) {
        return true;
    }
    if (isWordLike(previous)
        && (token.type == TokenType::Bang || token.type == TokenType::Minus)) {
        return previous == TokenType::Return;
    }
    if (previous == TokenType::RightBrace
        && token.type == TokenType::Else) {
        return true;
    }
    if (previous == TokenType::RightBrace
        && !canFollowClosingBraceOnSameLine(token.type)) {
        return false;
    }
    return false;
}

void popDelimiter(FormatterState& state, DelimiterKind kind)
{
    for (auto iterator = state.delimiters.rbegin(); iterator != state.delimiters.rend(); ++iterator) {
        if (iterator->kind != kind) {
            continue;
        }
        state.delimiters.erase(std::next(iterator).base());
        return;
    }
}

void flushBeforeToken(FormatterState& state, TokenType current)
{
    const bool preserveTopLevelBlankLine = state.sourceBlankLineSinceLastItem
        && state.delimiters.empty();
    bool blankLineHandled = false;

    if (state.pendingLineBreak) {
        state.newline();
        state.pendingLineBreak = false;
        if (preserveTopLevelBlankLine) {
            state.ensureBlankLine();
            blankLineHandled = true;
        }
    }

    if (state.pendingClose) {
        if (!canFollowClosingBraceOnSameLine(current) && !state.lineStart) {
            if (preserveTopLevelBlankLine) {
                state.ensureBlankLine();
                blankLineHandled = true;
            } else {
                state.newline();
            }
        }
        state.pendingClose = false;
    }

    if (!state.pendingLineBreak && !state.pendingClose
        && !blankLineHandled && preserveTopLevelBlankLine && state.lineStart) {
        state.ensureBlankLine();
    }
}

void emitOperator(
    FormatterState& state,
    const Token& token,
    const std::vector<const Token*>& tokens,
    std::size_t index)
{
    if (token.type == TokenType::Bang || (token.type == TokenType::Minus && isUnaryMinus(tokens, index))) {
        if (token.type == TokenType::Minus && index > 0 && tokens[index - 1]->type == TokenType::Return) {
            state.space();
        }
        state.writeRaw(token.lexeme);
        return;
    }

    state.space();
    state.writeRaw(token.lexeme);
    state.space();
}

void emitToken(
    FormatterState& state,
    const Token& token,
    std::size_t tokenIndex,
    const std::vector<const Token*>& tokens,
    const std::vector<bool>& genericAngles,
    std::size_t nextTokenPieceIndex,
    std::size_t currentPieceIndex,
    const std::vector<LosslessPiece>& pieces)
{
    const bool genericAngle = genericAngles[tokenIndex];
    flushBeforeToken(state, token.type);
    if (shouldSpaceBefore(state, token, genericAngle)) {
        state.space();
    }

    switch (token.type) {
    case TokenType::LeftParen:
        state.writeRaw(token.lexeme);
        state.delimiters.push_back(Delimiter{
            DelimiterKind::Parenthesis,
            false,
            state.indent,
            false});
        if (shouldWrapListAtOpen(
                state,
                tokens,
                genericAngles,
                tokenIndex,
                DelimiterKind::Parenthesis)) {
            state.wrapTopList();
        }
        break;
    case TokenType::RightParen: {
        bool wrapped = false;
        std::size_t baseIndent = state.indent;
        if (!state.delimiters.empty()
            && state.delimiters.back().kind == DelimiterKind::Parenthesis) {
            wrapped = state.delimiters.back().wrapped;
            baseIndent = state.delimiters.back().baseIndent;
        }
        if (wrapped) {
            if (!state.lineStart) {
                state.newline();
            }
            state.indent = baseIndent;
        }
        state.writeRaw(token.lexeme);
        popDelimiter(state, DelimiterKind::Parenthesis);
        break;
    }
    case TokenType::LeftBracket:
        state.writeRaw(token.lexeme);
        state.delimiters.push_back(Delimiter{
            DelimiterKind::Bracket,
            false,
            state.indent,
            false});
        if (shouldWrapListAtOpen(
                state,
                tokens,
                genericAngles,
                tokenIndex,
                DelimiterKind::Bracket)) {
            state.wrapTopList();
        }
        break;
    case TokenType::RightBracket: {
        bool wrapped = false;
        std::size_t baseIndent = state.indent;
        if (!state.delimiters.empty()
            && state.delimiters.back().kind == DelimiterKind::Bracket) {
            wrapped = state.delimiters.back().wrapped;
            baseIndent = state.delimiters.back().baseIndent;
        }
        if (wrapped) {
            if (!state.lineStart) {
                state.newline();
            }
            state.indent = baseIndent;
        }
        state.writeRaw(token.lexeme);
        popDelimiter(state, DelimiterKind::Bracket);
        break;
    }
    case TokenType::LeftBrace: {
        bool hasComment = false;
        for (std::size_t pieceIndex = currentPieceIndex + 1;
             pieceIndex < nextTokenPieceIndex;
             ++pieceIndex) {
            if (pieces[pieceIndex].isTrivia()
                && pieces[pieceIndex].triviaKind
                && *pieces[pieceIndex].triviaKind == TriviaKind::LineComment) {
                hasComment = true;
                break;
            }
        }
        const bool empty = nextTokenPieceIndex == pieces.size()
            || (!hasComment
                && nextTokenPieceIndex < pieces.size()
                && pieces[nextTokenPieceIndex].isToken()
                && pieces[nextTokenPieceIndex].token
                && pieces[nextTokenPieceIndex].token->type == TokenType::RightBrace);
        state.writeRaw(token.lexeme);
        state.delimiters.push_back(Delimiter{DelimiterKind::Brace, empty});
        if (!empty) {
            ++state.indent;
            state.pendingLineBreak = true;
        }
        if (token.type == TokenType::LeftBrace && state.inForHeader) {
            state.inForHeader = false;
        }
        break;
    }
    case TokenType::RightBrace: {
        bool empty = false;
        if (!state.delimiters.empty() && state.delimiters.back().kind == DelimiterKind::Brace) {
            empty = state.delimiters.back().empty;
        }
        if (!empty) {
            state.flushLineBreak();
            if (!state.lineStart) {
                state.newline();
            }
            if (state.indent > 0) {
                --state.indent;
            }
        }
        state.writeRaw(token.lexeme);
        popDelimiter(state, DelimiterKind::Brace);
        state.pendingClose = true;
        break;
    }
    case TokenType::Comma:
        state.writeRaw(token.lexeme);
        if (state.hasTopDelimiter(DelimiterKind::Brace)) {
            state.pendingLineBreak = true;
        } else if (shouldWrapAfterComma(state)) {
            state.wrapTopList();
        } else {
            state.space();
        }
        break;
    case TokenType::Colon:
        state.trimTrailingSpaces();
        state.writeRaw(token.lexeme);
        state.space();
        break;
    case TokenType::Semicolon:
        state.trimTrailingSpaces();
        state.writeRaw(token.lexeme);
        if (state.inForHeader) {
            state.space();
        } else {
            state.pendingLineBreak = true;
        }
        break;
    case TokenType::Dot:
        state.trimTrailingSpaces();
        state.writeRaw(token.lexeme);
        break;
    case TokenType::Question:
        state.trimTrailingSpaces();
        state.writeRaw(token.lexeme);
        break;
    case TokenType::Less:
    case TokenType::Greater:
        if (genericAngle) {
            if (token.type == TokenType::Less) {
                state.trimTrailingSpaces();
                state.writeRaw(token.lexeme);
                state.delimiters.push_back(Delimiter{DelimiterKind::Angle, false});
            } else {
                state.trimTrailingSpaces();
                state.writeRaw(token.lexeme);
                popDelimiter(state, DelimiterKind::Angle);
            }
        } else {
            emitOperator(state, token, tokens, tokenIndex);
        }
        break;
    case TokenType::Bang:
    case TokenType::Plus:
    case TokenType::Minus:
    case TokenType::Star:
    case TokenType::Slash:
    case TokenType::PlusEqual:
    case TokenType::MinusEqual:
    case TokenType::StarEqual:
    case TokenType::SlashEqual:
    case TokenType::BangEqual:
    case TokenType::Equal:
    case TokenType::EqualEqual:
    case TokenType::LessEqual:
    case TokenType::GreaterEqual:
    case TokenType::AmpersandAmpersand:
    case TokenType::Pipe:
    case TokenType::PipePipe:
    case TokenType::FatArrow:
        emitOperator(state, token, tokens, tokenIndex);
        break;
    default:
        state.writeRaw(token.lexeme);
        break;
    }

    if (token.type == TokenType::For) {
        state.inForHeader = true;
    }
    state.previous = token.type;
    state.sourceNewlineSinceLastItem = false;
    state.sourceBlankLineSinceLastItem = false;
}

} // namespace

namespace {

std::size_t countLineBreaks(std::string_view text)
{
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

} // namespace

std::string formatLosslessSource(const LosslessSourceFileView& source, FormatterOptions options)
{
    if (options.indentWidth == 0) {
        throw std::invalid_argument("formatter indentation width must be positive");
    }

    const std::vector<LosslessPiece>& pieces = source.pieces();
    std::vector<const Token*> tokens;
    std::vector<std::size_t> tokenPieceIndexes;
    for (std::size_t pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex) {
        if (!pieces[pieceIndex].isToken() || !pieces[pieceIndex].token) {
            continue;
        }
        tokenPieceIndexes.push_back(pieceIndex);
        tokens.push_back(&*pieces[pieceIndex].token);
    }

    if (tokens.empty()) {
        std::string comments;
        for (const LosslessPiece& piece : pieces) {
            if (piece.isTrivia() && piece.triviaKind && *piece.triviaKind == TriviaKind::LineComment) {
                if (!comments.empty()) {
                    comments.push_back('\n');
                }
                comments += piece.text;
            }
        }
        if (!comments.empty()) {
            comments.push_back('\n');
        }
        return comments;
    }

    std::vector<bool> genericAngles = findGenericAngles(tokens);
    FormatterState state;
    state.indentWidth = options.indentWidth;

    std::size_t tokenIndex = 0;
    for (std::size_t pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex) {
        const LosslessPiece& piece = pieces[pieceIndex];
        if (piece.isTrivia()) {
            if (piece.triviaKind && *piece.triviaKind == TriviaKind::LineComment) {
                if (state.sourceBlankLineSinceLastItem && state.delimiters.empty()) {
                    state.ensureBlankLine();
                }
                if (state.pendingLineBreak) {
                    if (state.sourceNewlineSinceLastItem) {
                        state.newline();
                    } else {
                        state.space();
                    }
                    state.pendingLineBreak = false;
                } else if (state.pendingClose) {
                    if (state.sourceNewlineSinceLastItem) {
                        state.newline();
                    } else {
                        state.space();
                    }
                    state.pendingClose = false;
                } else if (state.sourceNewlineSinceLastItem && !state.lineStart) {
                    state.newline();
                } else if (!state.lineStart) {
                    state.space();
                }
                state.writeRaw(piece.text);
                state.newline();
                state.sourceNewlineSinceLastItem = false;
                state.sourceBlankLineSinceLastItem = false;
            } else if (piece.text.find('\n') != std::string::npos) {
                state.sourceNewlineSinceLastItem = true;
                if (state.delimiters.empty() && countLineBreaks(piece.text) >= 2) {
                    state.sourceBlankLineSinceLastItem = true;
                }
            }
            continue;
        }

        if (!piece.token || tokenIndex >= tokens.size()) {
            continue;
        }
        std::size_t nextTokenPieceIndex = pieces.size();
        if (tokenIndex + 1 < tokenPieceIndexes.size()) {
            nextTokenPieceIndex = tokenPieceIndexes[tokenIndex + 1];
        }
        emitToken(
            state,
            *piece.token,
            tokenIndex,
            tokens,
            genericAngles,
            nextTokenPieceIndex,
            pieceIndex,
            pieces);
        ++tokenIndex;
    }

    state.trimTrailingSpaces();
    while (!state.output.empty() && state.output.back() == '\n') {
        state.output.pop_back();
    }
    if (!state.output.empty()) {
        state.output.push_back('\n');
    }
    return state.output;
}
