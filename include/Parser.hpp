#pragma once

#include "Ast.hpp"
#include "Diagnostic.hpp"
#include "Token.hpp"

#include <exception>
#include <optional>
#include <string>
#include <vector>

class ParseError final : public DiagnosticError {
public:
    explicit ParseError(const Token& token, const std::string& message);
};

class ParseErrorList final : public std::exception {
public:
    explicit ParseErrorList(std::vector<ParseError> errors);

    const std::vector<ParseError>& errors() const;
    const char* what() const noexcept override;

private:
    std::vector<ParseError> errors_;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // Parse all tokens into a Program AST.
    Program parse();

private:
    void recordParseError(ParseError error);
    void synchronize(bool stopAtRightBrace);
    std::optional<StmtPtr> parseDeclarationRecovering(bool stopAtRightBrace);

    StmtPtr declaration();
    StmtPtr exportDeclaration();
    StmtPtr enumDeclaration();
    std::vector<EnumVariantDecl> enumVariants();
    EnumVariantDecl enumVariant();
    StmtPtr structDeclaration();
    StmtPtr implDeclaration();
    MethodDecl methodDeclaration();
    std::optional<MethodDecl> parseMethodDeclarationRecovering();
    void synchronizeImplMethod();
    void synchronizeDelimitedMember();
    std::vector<StructFieldDecl> structFields();
    StructFieldDecl structField();
    StmtPtr functionDeclaration();
    StmtPtr letDeclaration();
    StmtPtr importDeclaration();
    std::vector<TypeParameter> typeParameters();
    std::vector<Parameter> parameters();
    Parameter parameter();
    TypeAnnotation typeAnnotation(const std::string& simpleTypeMessage = "expected type name");
    std::vector<TypeAnnotation> typeArguments();
    std::optional<TypeAnnotation> optionalReturnType();
    StmtPtr statement();
    StmtPtr ifStatement();
    StmtPtr matchStatement();
    ExprPtr parseMatchExpression();
    StmtPtr forStatement();
    StmtPtr forInStatement(Token keyword, Token variable);
    StmtPtr forInitializer();
    StmtPtr whileStatement();
    StmtPtr breakStatement();
    StmtPtr continueStatement();
    StmtPtr blockStatement();
    std::vector<StmtPtr> blockStatements();
    StmtPtr returnStatement();
    StmtPtr letDeclarationNoSemicolon(const std::string& terminatorMessage);
    StmtPtr expressionStatementNoSemicolon();
    StmtPtr expressionStatement();

    // Recursive descent expression grammar, ordered from lowest to highest precedence.
    ExprPtr expression();
    ExprPtr conditionExpression();
    ExprPtr assignment();
    ExprPtr coalesce();
    bool matchCompoundAssignment();
    ExprPtr logicalOr();
    ExprPtr logicalAnd();
    ExprPtr equality();
    ExprPtr comparison();
    ExprPtr term();
    ExprPtr factor();
    ExprPtr unary();
    ExprPtr call();
    bool isExplicitTypeArgumentCall(const Expr& callee) const;
    std::vector<TypeAnnotation> explicitTypeArguments();
    ExprPtr finishCall(ExprPtr callee, std::vector<TypeAnnotation> typeArguments);
    ExprPtr finishIndex(ExprPtr collection);
    ExprPtr finishFieldAccess(ExprPtr object);
    ExprPtr arrayLiteral(Token bracket);
    ExprPtr mapLiteral(Token brace);
    std::vector<StructField> structLiteralFields();
    bool isStructConstructorStart() const;
    ExprPtr structConstructor();
    ExprPtr qualifiedStructConstructor();
    bool isQualifiedStructConstructorStart() const;
    ExprPtr functionExpression();
    ExprPtr primary();
    PatternPtr pattern();
    PatternPtr patternAtom();
    PatternPtr recordPattern(std::optional<Token> qualifier, Token name);
    bool isQualifiedRecordPatternStart() const;
    PatternPtr variantPattern(Token qualifier, Token name);

    bool match(TokenType type);
    bool matchContextualIdentifier(const std::string& lexeme);
    bool check(TokenType type) const;
    bool checkContextualIdentifier(const std::string& lexeme) const;
    bool checkNext(TokenType type) const;
    Token advance();
    bool isAtEnd() const;
    const Token& peek() const;
    const Token& previous() const;
    Token consume(TokenType type, const std::string& message);

    std::vector<Token> tokens_;
    std::size_t current_ = 0;
    std::vector<ParseError> errors_;
    bool allowStructConstructors_ = true;
    int blockDepth_ = 0;
};
