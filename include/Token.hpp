#pragma once

#include "SourceIdentity.hpp"

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>

// TokenType describes every syntactic atom the lexer can emit.
enum class TokenType {
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    LeftBrace,
    RightBrace,
    Colon,
    Semicolon,
    Comma,
    Dot,
    Question,

    Plus,
    Minus,
    Star,
    Slash,
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    Bang,
    BangEqual,
    Equal,
    FatArrow,
    EqualEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AmpersandAmpersand,
    Pipe,
    PipePipe,

    Identifier,
    Number,
    String,

    Break,
    Continue,
    For,
    If,
    Impl,
    Operator,
    Import,
    In,
    As,
    Export,
    Else,
    Enum,
    Fun,
    Let,
    Match,
    Print,
    Private,
    Return,
    Struct,
    While,
    True,
    False,
    Nil,

    EndOfFile,
};

struct Token {
    TokenType type;
    std::string lexeme;
    // 1-based source location for diagnostics.
    int line;
    int column;
    // Typed source identity and source-local half-open byte range, populated
    // by FrontendSession.  Parser diagnostics use the per-file `line`
    // coordinate above.
    std::optional<SourceFileId> sourceId = std::nullopt;
    std::optional<SourceRange> range = std::nullopt;
    // Lexer-owned offsets are source-buffer offsets before FrontendSession
    // attaches a SourceFileId; FrontendSession converts them into `range`.
    std::size_t startOffset = 0;
    std::size_t endOffset = 0;
};

std::string tokenTypeName(TokenType type);
std::ostream& operator<<(std::ostream& out, const Token& token);
