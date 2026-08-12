#pragma once

#include "Diagnostic.hpp"
#include "Token.hpp"

#include <exception>
#include <string>
#include <vector>

class LexErrorList final : public std::exception {
public:
    explicit LexErrorList(std::vector<DiagnosticError> errors);

    const std::vector<DiagnosticError>& errors() const;
    const char* what() const noexcept override;

private:
    std::vector<DiagnosticError> errors_;
};

class Lexer {
public:
    explicit Lexer(std::string source);

    // Scan the complete source buffer and append a final EndOfFile token.
    std::vector<Token> scanTokens();

private:
    bool isAtEnd() const;
    char advance();
    bool match(char expected);
    char peek() const;
    char peekNext() const;

    void scanToken();
    void addToken(TokenType type);
    void recordError(DiagnosticError error);
    void throwIfErrors();
    void stringLiteral();
    void numberLiteral();
    void identifier();

    std::string source_;
    std::vector<Token> tokens_;
    std::vector<DiagnosticError> errors_;
    // start_ marks the beginning of the lexeme currently being scanned;
    // current_ points to the next character to consume.
    std::size_t start_ = 0;
    std::size_t current_ = 0;
    int line_ = 1;
    int column_ = 1;
    int tokenColumn_ = 1;
};
