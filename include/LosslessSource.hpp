#pragma once

#include "SourceMap.hpp"
#include "Token.hpp"

#include <optional>
#include <string>
#include <vector>

enum class LosslessPieceKind {
    Token,
    Trivia,
};

enum class TriviaKind {
    Whitespace,
    LineComment,
    BlockComment,
    Other,
};

// A lossless source piece owns the exact bytes in its source range. Token
// pieces retain the existing lexer token; trivia pieces are derived only from
// the gaps between those tokens and never reinterpret parser grammar.
struct LosslessPiece {
    LosslessPieceKind kind = LosslessPieceKind::Trivia;
    SourceRange range;
    std::string text;
    std::optional<Token> token;
    std::optional<TriviaKind> triviaKind;

    bool isToken() const
    {
        return kind == LosslessPieceKind::Token;
    }

    bool isTrivia() const
    {
        return kind == LosslessPieceKind::Trivia;
    }
};

class LosslessSourceFileView {
public:
    LosslessSourceFileView(SourceFileId sourceId, std::vector<LosslessPiece> pieces);

    SourceFileId sourceId() const;
    const std::vector<LosslessPiece>& pieces() const;
    std::string reconstruct() const;
    bool roundTrips(const std::string& source) const;

private:
    SourceFileId sourceId_;
    std::vector<LosslessPiece> pieces_;
};

class LosslessSourceView {
public:
    explicit LosslessSourceView(std::vector<LosslessSourceFileView> files = {});

    const std::vector<LosslessSourceFileView>& files() const;
    const std::vector<LosslessSourceFileView>& sourceFiles() const;
    const LosslessSourceFileView& file(SourceFileId sourceId) const;

private:
    std::vector<LosslessSourceFileView> files_;
};

// Construct a view from the production token stream and the original source
// bytes. The lexer remains the only token producer.
LosslessSourceFileView buildLosslessSourceFileView(
    const SourceFile& source,
    const std::vector<Token>& tokens);
