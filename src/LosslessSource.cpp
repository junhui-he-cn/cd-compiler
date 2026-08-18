#include "LosslessSource.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace {

SourceFileId sourceIdFor(const SourceFile& source)
{
    return source.id.valid() ? source.id : SourceFileId{0};
}

std::optional<SourceRange> rangeForToken(const Token& token, SourceFileId sourceId)
{
    if (token.range) {
        return token.range;
    }
    if (token.startOffset <= token.endOffset) {
        return SourceRange{sourceId, token.startOffset, token.endOffset};
    }
    return std::nullopt;
}

void appendTrivia(
    std::vector<LosslessPiece>& pieces,
    const SourceFile& source,
    SourceFileId sourceId,
    std::size_t begin,
    std::size_t end)
{
    std::size_t cursor = begin;
    while (cursor < end) {
        const bool startsComment = source.text[cursor] == '/'
            && cursor + 1 < end
            && source.text[cursor + 1] == '/';
        if (startsComment) {
            std::size_t commentEnd = cursor + 2;
            while (commentEnd < end && source.text[commentEnd] != '\n') {
                ++commentEnd;
            }
            pieces.push_back(LosslessPiece{
                LosslessPieceKind::Trivia,
                SourceRange{sourceId, cursor, commentEnd},
                source.text.substr(cursor, commentEnd - cursor),
                std::nullopt,
                TriviaKind::LineComment});
            cursor = commentEnd;
            continue;
        }

        const bool startsBlockComment = source.text[cursor] == '/'
            && cursor + 1 < end
            && source.text[cursor + 1] == '*';
        if (startsBlockComment) {
            std::size_t commentEnd = cursor + 2;
            while (commentEnd + 1 < end
                && !(source.text[commentEnd] == '*'
                    && source.text[commentEnd + 1] == '/')) {
                ++commentEnd;
            }
            if (commentEnd + 1 < end) {
                commentEnd += 2;
            } else {
                commentEnd = end;
            }
            pieces.push_back(LosslessPiece{
                LosslessPieceKind::Trivia,
                SourceRange{sourceId, cursor, commentEnd},
                source.text.substr(cursor, commentEnd - cursor),
                std::nullopt,
                TriviaKind::BlockComment});
            cursor = commentEnd;
            continue;
        }

        std::size_t triviaEnd = cursor + 1;
        while (triviaEnd < end
            && !(source.text[triviaEnd] == '/'
                && triviaEnd + 1 < end
                && (source.text[triviaEnd + 1] == '/'
                    || source.text[triviaEnd + 1] == '*'))) {
            ++triviaEnd;
        }
        const std::string text = source.text.substr(cursor, triviaEnd - cursor);
        const bool allWhitespace = std::all_of(
            text.begin(),
            text.end(),
            [](char value) {
                return std::isspace(static_cast<unsigned char>(value)) != 0;
            });
        pieces.push_back(LosslessPiece{
            LosslessPieceKind::Trivia,
            SourceRange{sourceId, cursor, triviaEnd},
            text,
            std::nullopt,
            allWhitespace ? std::optional<TriviaKind>(TriviaKind::Whitespace)
                          : std::optional<TriviaKind>(TriviaKind::Other)});
        cursor = triviaEnd;
    }
}

} // namespace

LosslessSourceFileView::LosslessSourceFileView(
    SourceFileId sourceId,
    std::vector<LosslessPiece> pieces)
    : sourceId_(sourceId)
    , pieces_(std::move(pieces))
{
}

SourceFileId LosslessSourceFileView::sourceId() const
{
    return sourceId_;
}

const std::vector<LosslessPiece>& LosslessSourceFileView::pieces() const
{
    return pieces_;
}

std::string LosslessSourceFileView::reconstruct() const
{
    std::string result;
    for (const LosslessPiece& piece : pieces_) {
        result += piece.text;
    }
    return result;
}

bool LosslessSourceFileView::roundTrips(const std::string& source) const
{
    return reconstruct() == source;
}

LosslessSourceView::LosslessSourceView(std::vector<LosslessSourceFileView> files)
    : files_(std::move(files))
{
}

const std::vector<LosslessSourceFileView>& LosslessSourceView::files() const
{
    return files_;
}

const std::vector<LosslessSourceFileView>& LosslessSourceView::sourceFiles() const
{
    return files_;
}

const LosslessSourceFileView& LosslessSourceView::file(SourceFileId sourceId) const
{
    const auto found = std::find_if(
        files_.begin(),
        files_.end(),
        [sourceId](const LosslessSourceFileView& file) {
            return file.sourceId() == sourceId;
        });
    if (found == files_.end()) {
        throw std::out_of_range("lossless source file ID is not present");
    }
    return *found;
}

LosslessSourceFileView buildLosslessSourceFileView(
    const SourceFile& source,
    const std::vector<Token>& tokens)
{
    const SourceFileId sourceId = sourceIdFor(source);
    std::vector<Token> ordered;
    for (const Token& token : tokens) {
        if (token.type == TokenType::EndOfFile) {
            continue;
        }
        const std::optional<SourceRange> range = rangeForToken(token, sourceId);
        if (!range || range->source != sourceId || range->start > range->end
            || range->end > source.text.size()) {
            throw std::invalid_argument("token has invalid lossless source range");
        }
        ordered.push_back(token);
    }

    std::sort(ordered.begin(), ordered.end(), [sourceId](const Token& left, const Token& right) {
        const SourceRange leftRange = *rangeForToken(left, sourceId);
        const SourceRange rightRange = *rangeForToken(right, sourceId);
        if (leftRange.start != rightRange.start) {
            return leftRange.start < rightRange.start;
        }
        return leftRange.end < rightRange.end;
    });

    std::vector<LosslessPiece> pieces;
    std::size_t cursor = 0;
    for (const Token& token : ordered) {
        const SourceRange range = *rangeForToken(token, sourceId);
        if (range.start < cursor) {
            throw std::invalid_argument("overlapping token ranges in lossless source");
        }
        if (range.start > cursor) {
            appendTrivia(pieces, source, sourceId, cursor, range.start);
        }
        pieces.push_back(LosslessPiece{
            LosslessPieceKind::Token,
            range,
            source.text.substr(range.start, range.end - range.start),
            token,
            std::nullopt});
        cursor = range.end;
    }
    if (cursor < source.text.size()) {
        appendTrivia(pieces, source, sourceId, cursor, source.text.size());
    }

    return LosslessSourceFileView(sourceId, std::move(pieces));
}
