#include "Parser.hpp"

#include <optional>
#include <sstream>
#include <utility>

namespace {

std::string expectedFunStartMessage(const Token& token)
{
    std::ostringstream message;
    message << "expected function name after `fun` declaration or `(` for function expression, found "
            << tokenTypeName(token.type);
    if (!token.lexeme.empty()) {
        message << " `" << token.lexeme << "`";
    }
    return message.str();
}

std::optional<SourceSpan> spanForToken(const Token& token)
{
    if (!token.source) {
        return std::nullopt;
    }
    SourceSpan span{
        *token.source,
        token.sourceLine.value_or(token.line),
        token.column,
    };
    if (token.range) {
        span.range = SourceSpanRange{token.range->start, token.range->end};
    }
    return span;
}

template <typename Node>
std::unique_ptr<Node> withSpan(std::unique_ptr<Node> node, const Token& token)
{
    node->span = spanForToken(token);
    node->range = token.range;
    return node;
}

template <typename Node>
std::unique_ptr<Node> withSpan(std::unique_ptr<Node> node, const std::optional<SourceSpan>& span)
{
    node->span = span;
    return node;
}

template <typename VariableBuilder, typename IndexBuilder, typename FieldBuilder>
ExprPtr buildAssignmentTarget(
    ExprPtr& expr,
    VariableBuilder variableBuilder,
    IndexBuilder indexBuilder,
    FieldBuilder fieldBuilder)
{
    if (const auto* variable = dynamic_cast<const VariableExpr*>(expr.get())) {
        return variableBuilder(variable->name);
    }

    if (auto* index = dynamic_cast<IndexExpr*>(expr.get())) {
        ExprPtr collection = std::move(index->collection);
        Token bracket = std::move(index->bracket);
        ExprPtr indexExpression = std::move(index->index);
        return indexBuilder(std::move(collection), std::move(bracket), std::move(indexExpression));
    }

    if (auto* field = dynamic_cast<FieldAccessExpr*>(expr.get())) {
        ExprPtr object = std::move(field->object);
        Token name = std::move(field->name);
        return fieldBuilder(std::move(object), std::move(name));
    }

    return nullptr;
}

class ParserStateGuard final {
public:
    ParserStateGuard(int& blockDepth, bool& allowStructConstructors)
        : blockDepth_(blockDepth)
        , allowStructConstructors_(allowStructConstructors)
        , initialBlockDepth_(blockDepth)
        , initialAllowStructConstructors_(allowStructConstructors)
    {
    }

    ParserStateGuard(const ParserStateGuard&) = delete;
    ParserStateGuard& operator=(const ParserStateGuard&) = delete;

    ~ParserStateGuard()
    {
        restore();
    }

    void restore() noexcept
    {
        blockDepth_ = initialBlockDepth_;
        allowStructConstructors_ = initialAllowStructConstructors_;
    }

private:
    int& blockDepth_;
    bool& allowStructConstructors_;
    int initialBlockDepth_;
    bool initialAllowStructConstructors_;
};

} // namespace

ParseError::ParseError(const Token& token, const std::string& message)
    : DiagnosticError(
        DiagnosticKind::Parse,
        SourceLocation{token.line, token.column},
        token.range,
        message)
{
}

ParseErrorList::ParseErrorList(std::vector<ParseError> errors)
    : errors_(std::move(errors))
{
}

const std::vector<ParseError>& ParseErrorList::errors() const
{
    return errors_;
}

const char* ParseErrorList::what() const noexcept
{
    return "parse errors";
}

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens))
{
}

Program Parser::parse()
{
    Program program;
    errors_.clear();

    while (!isAtEnd()) {
        if (std::optional<StmtPtr> statement = parseDeclarationRecovering(false)) {
            program.statements.push_back(std::move(*statement));
        }
    }

    if (!errors_.empty()) {
        throw ParseErrorList(std::move(errors_));
    }

    finalizeSyntaxMetadata(program);
    return program;
}

void Parser::recordParseError(ParseError error)
{
    errors_.push_back(std::move(error));
}

std::optional<StmtPtr> Parser::parseDeclarationRecovering(bool stopAtRightBrace)
{
    ParserStateGuard stateGuard(blockDepth_, allowStructConstructors_);
    try {
        return declaration();
    } catch (const ParseError& error) {
        stateGuard.restore();
        recordParseError(error);
        synchronize(stopAtRightBrace);
        return std::nullopt;
    }
}

std::optional<MethodDecl> Parser::parseMethodDeclarationRecovering()
{
    ParserStateGuard stateGuard(blockDepth_, allowStructConstructors_);
    try {
        return methodDeclaration();
    } catch (const ParseError& error) {
        stateGuard.restore();
        recordParseError(error);
        synchronizeImplMethod();
        return std::nullopt;
    }
}

void Parser::synchronizeImplMethod()
{
    int braceDepth = 0;
    while (!isAtEnd()) {
        if (check(TokenType::LeftBrace)) {
            ++braceDepth;
            advance();
            continue;
        }

        if (check(TokenType::RightBrace)) {
            if (braceDepth == 0) {
                return;
            }
            --braceDepth;
            advance();
            continue;
        }

        if (braceDepth == 0 && (check(TokenType::Fun) || check(TokenType::Operator))) {
            return;
        }
        advance();
    }
}

void Parser::synchronizeDelimitedMember()
{
    int parenthesisDepth = 0;
    int bracketDepth = 0;
    int angleDepth = 0;
    while (!isAtEnd()) {
        if (parenthesisDepth == 0 && bracketDepth == 0 && angleDepth == 0
            && (check(TokenType::Comma) || check(TokenType::RightBrace))) {
            return;
        }

        switch (peek().type) {
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
        case TokenType::Less:
            ++angleDepth;
            break;
        case TokenType::Greater:
            if (angleDepth > 0) {
                --angleDepth;
            }
            break;
        default:
            break;
        }
        advance();
    }
}

void Parser::synchronize(bool stopAtRightBrace)
{
    if (check(TokenType::Semicolon)) {
        advance();
    }

    while (!isAtEnd()) {
        if (check(TokenType::RightBrace)) {
            if (!stopAtRightBrace) {
                advance();
            }
            return;
        }

        switch (peek().type) {
        case TokenType::Let:
        case TokenType::Fun:
        case TokenType::Enum:
        case TokenType::Struct:
        case TokenType::Impl:
        case TokenType::Import:
        case TokenType::Export:
        case TokenType::Print:
        case TokenType::If:
        case TokenType::Match:
        case TokenType::While:
        case TokenType::For:
        case TokenType::Break:
        case TokenType::Continue:
        case TokenType::Return:
        case TokenType::LeftBrace:
            return;
        default:
            advance();
            break;
        }
    }
}

StmtPtr Parser::declaration()
{
    if (match(TokenType::Export)) {
        if (blockDepth_ != 0) {
            throw ParseError(previous(), "`export` is only allowed at top level");
        }
        return exportDeclaration();
    }
    if (match(TokenType::Import)) {
        if (blockDepth_ != 0) {
            throw ParseError(previous(), "`import` is only allowed at top level");
        }
        return importDeclaration();
    }
    if (match(TokenType::Struct)) {
        return structDeclaration();
    }
    if (match(TokenType::Enum)) {
        return enumDeclaration();
    }
    if (match(TokenType::Impl)) {
        if (blockDepth_ != 0) {
            throw ParseError(previous(), "`impl` is only allowed at top level");
        }
        return implDeclaration();
    }
    if (check(TokenType::Fun) && checkNext(TokenType::Identifier)) {
        advance();
        return functionDeclaration();
    }
    if (check(TokenType::Fun)
        && !checkNext(TokenType::LeftParen)
        && !checkNext(TokenType::Less)) {
        advance();
        throw ParseError(peek(), expectedFunStartMessage(peek()));
    }
    if (match(TokenType::Let)) {
        return letDeclaration();
    }
    return statement();
}

StmtPtr Parser::exportDeclaration()
{
    Token keyword = previous();
    std::vector<Token> names;

    names.push_back(consume(TokenType::Identifier, "expected identifier after `export`"));
    while (match(TokenType::Comma)) {
        names.push_back(consume(TokenType::Identifier, "expected identifier after `,` in export list"));
    }

    std::optional<Token> sourcePath;
    if (matchContextualIdentifier("from")) {
        sourcePath = consume(TokenType::String, "expected re-export path string");
        consume(TokenType::Semicolon, "expected `;` after re-export path");
    } else {
        consume(TokenType::Semicolon, "expected `;` after export list");
    }

    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<ExportStmt>(std::move(keyword), std::move(names), std::move(sourcePath)),
        span);
}

StmtPtr Parser::structDeclaration()
{
    Token keyword = previous();
    Token name = consume(TokenType::Identifier, "expected struct name after `struct`");
    std::vector<TypeParameter> parsedTypeParameters = typeParameters();
    consume(TokenType::LeftBrace, "expected `{` after struct name");
    std::vector<StructFieldDecl> fields = structFields();
    consume(TokenType::RightBrace, "expected `}` after struct fields");
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<StructDeclStmt>(
            std::move(name), std::move(parsedTypeParameters), std::move(fields)),
        span);
}

StmtPtr Parser::enumDeclaration()
{
    Token keyword = previous();
    Token name = consume(TokenType::Identifier, "expected enum name after `enum`");
    std::vector<TypeParameter> parsedTypeParameters = typeParameters();
    consume(TokenType::LeftBrace, "expected `{` after enum name");
    std::vector<EnumVariantDecl> variants = enumVariants();
    consume(TokenType::RightBrace, "expected `}` after enum variants");
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<EnumDeclStmt>(
            std::move(name), std::move(parsedTypeParameters), std::move(variants)),
        span);
}

std::vector<EnumVariantDecl> Parser::enumVariants()
{
    std::vector<EnumVariantDecl> variants;
    if (!check(TokenType::RightBrace)) {
        while (true) {
            try {
                variants.push_back(enumVariant());
            } catch (const ParseError& error) {
                recordParseError(error);
                synchronizeDelimitedMember();
            }
            if (!match(TokenType::Comma) || check(TokenType::RightBrace)) {
                break;
            }
        }
    }
    return variants;
}

EnumVariantDecl Parser::enumVariant()
{
    Token name = consume(TokenType::Identifier, "expected enum variant name");
    std::vector<TypeAnnotation> payloadTypes;
    std::vector<std::optional<Token>> payloadNames;
    if (match(TokenType::LeftParen)) {
        if (!check(TokenType::RightParen)) {
            do {
                std::optional<Token> payloadName;
                if (check(TokenType::Identifier) && checkNext(TokenType::Colon)) {
                    payloadName = advance();
                    consume(TokenType::Colon, "expected `:` after enum payload name");
                }
                payloadNames.push_back(std::move(payloadName));
                payloadTypes.push_back(typeAnnotation("expected enum variant payload type"));
            } while (match(TokenType::Comma));
        }
        consume(TokenType::RightParen, "expected `)` after enum variant payload types");
    }
    return EnumVariantDecl{std::move(name), std::move(payloadTypes), std::move(payloadNames)};
}

std::vector<StructFieldDecl> Parser::structFields()
{
    std::vector<StructFieldDecl> fields;
    if (!check(TokenType::RightBrace)) {
        do {
            try {
                fields.push_back(structField());
            } catch (const ParseError& error) {
                recordParseError(error);
                synchronizeDelimitedMember();
            }
        } while (match(TokenType::Comma));
    }
    return fields;
}

StructFieldDecl Parser::structField()
{
    const bool isPrivate = match(TokenType::Private);
    Token name = consume(TokenType::Identifier, "expected struct field name");
    consume(TokenType::Colon, "expected `:` after struct field name");
    TypeAnnotation typeName = typeAnnotation("expected struct field type after `:`");
    return StructFieldDecl{std::move(name), std::move(typeName), isPrivate};
}

StmtPtr Parser::implDeclaration()
{
    Token keyword = previous();
    Token typeName = consume(TokenType::Identifier, "expected struct name after `impl`");
    std::vector<TypeParameter> parsedTypeParameters = typeParameters();
    consume(TokenType::LeftBrace, "expected `{` after impl type name");
    std::vector<MethodDecl> methods;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        if (std::optional<MethodDecl> method = parseMethodDeclarationRecovering()) {
            methods.push_back(std::move(*method));
        }
    }
    consume(TokenType::RightBrace, "expected `}` after impl methods");
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<ImplStmt>(
            std::move(typeName), std::move(parsedTypeParameters), std::move(methods)),
        span);
}

MethodDecl Parser::methodDeclaration()
{
    const bool isOperator = match(TokenType::Operator);
    Token name;
    std::vector<TypeParameter> parsedTypeParameters;
    if (isOperator) {
        if (!match(TokenType::Less) && !match(TokenType::LessEqual)
            && !match(TokenType::Greater) && !match(TokenType::GreaterEqual)) {
            throw ParseError(
                peek(),
                "expected `<`, `<=`, `>`, or `>=` after `operator`");
        }
        name = previous();
    } else {
        consume(TokenType::Fun, "expected `fun` or `operator` method declaration in impl block");
        name = consume(TokenType::Identifier, "expected method name after `fun`");
        parsedTypeParameters = typeParameters();
    }
    consume(TokenType::LeftParen, "expected `(` after method name");
    std::vector<Parameter> parsedParameters = parameters();
    consume(TokenType::RightParen, "expected `)` after method parameters");
    std::optional<TypeAnnotation> returnTypeName = optionalReturnType();
    consume(TokenType::LeftBrace, "expected `{` before method body");
    ++blockDepth_;
    std::vector<StmtPtr> body = blockStatements();
    --blockDepth_;
    return MethodDecl(
        std::move(name),
        std::move(parsedTypeParameters),
        std::move(parsedParameters),
        std::move(returnTypeName),
        std::move(body),
        isOperator);
}

StmtPtr Parser::functionDeclaration()
{
    Token keyword = previous();
    Token name = consume(TokenType::Identifier, "expected function name after `fun`");
    std::vector<TypeParameter> parsedTypeParameters = typeParameters();
    consume(TokenType::LeftParen, "expected `(` after function name");

    std::vector<Parameter> parsedParameters = parameters();
    consume(TokenType::RightParen, "expected `)` after function parameters");
    std::optional<TypeAnnotation> returnTypeName = optionalReturnType();
    consume(TokenType::LeftBrace, "expected `{` before function body");
    ++blockDepth_;
    std::vector<StmtPtr> body = blockStatements();
    --blockDepth_;
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<FunctionStmt>(
            std::move(name),
            std::move(parsedTypeParameters),
            std::move(parsedParameters),
            std::move(returnTypeName),
            std::move(body)),
        span);
}

std::vector<TypeParameter> Parser::typeParameters()
{
    std::vector<TypeParameter> parameters;
    if (!match(TokenType::Less)) {
        return parameters;
    }
    do {
        Token name = consume(
            TokenType::Identifier,
            "expected type parameter name after `<` or `,`");
        std::optional<TypeAnnotation> constraint;
        if (match(TokenType::Colon)) {
            constraint = typeAnnotation("expected type constraint after `:`");
        }
        parameters.push_back(TypeParameter{std::move(name), std::move(constraint)});
    } while (match(TokenType::Comma));
    consume(TokenType::Greater, "expected `>` after type parameters");
    return parameters;
}

StmtPtr Parser::letDeclarationNoSemicolon(const std::string& terminatorMessage)
{
    Token keyword = previous();
    Token name = consume(TokenType::Identifier, "expected variable name after `let`");

    std::optional<TypeAnnotation> typeName;
    if (match(TokenType::Colon)) {
        typeName = typeAnnotation("expected type name after `:`");
    }

    consume(TokenType::Equal, "expected `=` after variable declaration");
    ExprPtr initializer = expression();

    if (!terminatorMessage.empty()) {
        consume(TokenType::Semicolon, terminatorMessage);
    }
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<LetStmt>(std::move(name), std::move(typeName), std::move(initializer)),
        span);
}

StmtPtr Parser::letDeclaration()
{
    return letDeclarationNoSemicolon("expected `;` after variable declaration");
}

StmtPtr Parser::importDeclaration()
{
    Token keyword = previous();
    Token path = consume(TokenType::String, "expected import path string");
    std::optional<Token> alias;
    if (match(TokenType::As)) {
        alias = consume(TokenType::Identifier, "expected namespace alias after `as`");
    }
    consume(TokenType::Semicolon, alias ? "expected `;` after import alias" : "expected `;` after import path");
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<ImportStmt>(std::move(keyword), std::move(path), std::move(alias)),
        span);
}

std::vector<Parameter> Parser::parameters()
{
    std::vector<Parameter> parsedParameters;
    if (!check(TokenType::RightParen)) {
        do {
            parsedParameters.push_back(parameter());
        } while (match(TokenType::Comma));
    }
    return parsedParameters;
}

Parameter Parser::parameter()
{
    Token name = consume(TokenType::Identifier, "expected parameter name");
    std::optional<TypeAnnotation> typeName;
    if (match(TokenType::Colon)) {
        typeName = typeAnnotation("expected parameter type after `:`");
    }
    return Parameter{std::move(name), std::move(typeName)};
}

std::optional<TypeAnnotation> Parser::optionalReturnType()
{
    if (!match(TokenType::Colon)) {
        return std::nullopt;
    }
    return typeAnnotation("expected return type after `:`");
}

TypeAnnotation Parser::typeAnnotation(const std::string& simpleTypeMessage)
{
    if (match(TokenType::LeftBracket)) {
        Token bracket = previous();
        TypeAnnotation elementType = typeAnnotation("expected array element type after `[`");
        consume(TokenType::RightBracket, "expected `]` after array element type");
        TypeAnnotation annotation = TypeAnnotation::array(std::move(bracket), std::move(elementType));
        if (match(TokenType::Question)) {
            annotation = TypeAnnotation::nullable(previous(), std::move(annotation));
        }
        return annotation;
    }

    if (checkContextualIdentifier("map") && checkNext(TokenType::Less)) {
        Token mapToken = advance();
        consume(TokenType::Less, "expected `<` after `map` type");
        TypeAnnotation keyType = typeAnnotation("expected map key type after `<`");
        consume(TokenType::Comma, "expected `,` between map key and value types");
        TypeAnnotation valueType = typeAnnotation("expected map value type after `,`");
        consume(TokenType::Greater, "expected `>` after map value type");
        TypeAnnotation annotation = TypeAnnotation::map(
            std::move(mapToken), std::move(keyType), std::move(valueType));
        if (match(TokenType::Question)) {
            annotation = TypeAnnotation::nullable(previous(), std::move(annotation));
        }
        return annotation;
    }

    if (match(TokenType::Fun)) {
        Token keyword = previous();
        consume(TokenType::LeftParen, "expected `(` after `fun` in function type");
        std::vector<TypeAnnotation> parameterTypes = typeArguments();
        consume(TokenType::RightParen, "expected `)` after function type parameters");
        consume(TokenType::Colon, "expected `:` before function type return");
        TypeAnnotation returnType = typeAnnotation();
        return TypeAnnotation::function(std::move(keyword), std::move(parameterTypes), std::move(returnType));
    }

    Token name = consume(TokenType::Identifier, simpleTypeMessage);
    TypeAnnotation annotation;
    if (match(TokenType::Dot)) {
        Token member = consume(TokenType::Identifier, "expected type name after `.`");
        annotation = TypeAnnotation::qualified(std::move(name), std::move(member));
    } else {
        annotation = TypeAnnotation::simple(std::move(name));
    }
    if (match(TokenType::Less)) {
        if (check(TokenType::Greater)) {
            throw ParseError(peek(), "expected type argument after `<`");
        }
        do {
            annotation.typeArguments.push_back(typeAnnotation("expected type argument after `<`"));
        } while (match(TokenType::Comma));
        consume(TokenType::Greater, "expected `>` after type arguments");
    }
    if (match(TokenType::Question)) {
        annotation = TypeAnnotation::nullable(previous(), std::move(annotation));
    }
    return annotation;
}

std::vector<TypeAnnotation> Parser::typeArguments()
{
    std::vector<TypeAnnotation> arguments;
    if (!check(TokenType::RightParen)) {
        do {
            arguments.push_back(typeAnnotation());
        } while (match(TokenType::Comma));
    }
    return arguments;
}

StmtPtr Parser::statement()
{
    if (match(TokenType::Print)) {
        return printStatement();
    }
    if (match(TokenType::If)) {
        return ifStatement();
    }
    if (match(TokenType::Match)) {
        return matchStatement();
    }
    if (match(TokenType::For)) {
        return forStatement();
    }
    if (match(TokenType::While)) {
        return whileStatement();
    }
    if (match(TokenType::Break)) {
        return breakStatement();
    }
    if (match(TokenType::Continue)) {
        return continueStatement();
    }
    if (match(TokenType::Return)) {
        return returnStatement();
    }
    if (match(TokenType::LeftBrace)) {
        return blockStatement();
    }
    return expressionStatement();
}

StmtPtr Parser::ifStatement()
{
    Token keyword = previous();
    ExprPtr condition = conditionExpression();
    consume(TokenType::LeftBrace, "expected `{` after if condition");
    StmtPtr thenBranch = blockStatement();

    StmtPtr elseBranch;
    if (match(TokenType::Else)) {
        consume(TokenType::LeftBrace, "expected `{` after `else`");
        elseBranch = blockStatement();
    }

    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch)),
        span);
}

StmtPtr Parser::matchStatement()
{
    Token keyword = previous();
    ExprPtr value = conditionExpression();
    consume(TokenType::LeftBrace, "expected `{` after match value");

    std::vector<MatchArm> arms;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        PatternPtr armPattern = pattern();
        ExprPtr guard;
        if (match(TokenType::If)) {
            guard = conditionExpression();
        }
        consume(TokenType::FatArrow, "expected `=>` after match pattern");
        consume(TokenType::LeftBrace, "expected `{` after match arm");
        StmtPtr body = blockStatement();
        arms.push_back(MatchArm{std::move(armPattern), std::move(guard), std::move(body)});
    }
    consume(TokenType::RightBrace, "expected `}` after match arms");

    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(std::make_unique<MatchStmt>(std::move(value), std::move(arms)), span);
}

StmtPtr Parser::forInitializer()
{
    if (check(TokenType::Semicolon)) {
        return nullptr;
    }
    if (match(TokenType::Let)) {
        return letDeclarationNoSemicolon("");
    }
    return expressionStatementNoSemicolon();
}

StmtPtr Parser::forStatement()
{
    Token keyword = previous();

    if (check(TokenType::In)) {
        throw ParseError(peek(), "expected for loop initializer before `in`");
    }
    if (check(TokenType::Identifier) && checkNext(TokenType::In)) {
        Token variable = advance();
        advance();
        return forInStatement(std::move(keyword), std::move(variable));
    }

    StmtPtr initializer = forInitializer();
    consume(TokenType::Semicolon, "expected `;` after for initializer");

    ExprPtr condition;
    if (!check(TokenType::Semicolon)) {
        condition = expression();
    }
    consume(TokenType::Semicolon, "expected `;` after for condition");

    ExprPtr increment;
    if (!check(TokenType::LeftBrace)) {
        increment = expression();
    }
    consume(TokenType::LeftBrace, "expected `{` after for clauses");
    StmtPtr body = blockStatement();

    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<ForStmt>(std::move(keyword), std::move(initializer), std::move(condition), std::move(increment), std::move(body)),
        span);
}

StmtPtr Parser::forInStatement(Token keyword, Token variable)
{
    ExprPtr iterable = conditionExpression();
    consume(TokenType::LeftBrace, "expected `{` after for-in iterable");
    StmtPtr body = blockStatement();
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<ForInStmt>(std::move(keyword), std::move(variable), std::move(iterable), std::move(body)),
        span);
}

StmtPtr Parser::whileStatement()
{
    Token keyword = previous();
    ExprPtr condition = conditionExpression();
    consume(TokenType::LeftBrace, "expected `{` after while condition");
    StmtPtr body = blockStatement();
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(std::make_unique<WhileStmt>(std::move(condition), std::move(body)), span);
}

StmtPtr Parser::breakStatement()
{
    Token keyword = previous();
    consume(TokenType::Semicolon, "expected `;` after break");
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(std::make_unique<BreakStmt>(std::move(keyword)), span);
}

StmtPtr Parser::continueStatement()
{
    Token keyword = previous();
    consume(TokenType::Semicolon, "expected `;` after continue");
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(std::make_unique<ContinueStmt>(std::move(keyword)), span);
}

StmtPtr Parser::blockStatement()
{
    Token brace = previous();
    ++blockDepth_;
    std::vector<StmtPtr> statements = blockStatements();
    --blockDepth_;
    return withSpan(std::make_unique<BlockStmt>(std::move(statements)), brace);
}

std::vector<StmtPtr> Parser::blockStatements()
{
    std::vector<StmtPtr> statements;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        if (std::optional<StmtPtr> statement = parseDeclarationRecovering(true)) {
            statements.push_back(std::move(*statement));
        }
    }
    consume(TokenType::RightBrace, "expected `}` after block");
    return statements;
}

StmtPtr Parser::printStatement()
{
    Token keyword = previous();
    ExprPtr value = expression();
    consume(TokenType::Semicolon, "expected `;` after print value");
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(std::make_unique<PrintStmt>(std::move(value)), span);
}

StmtPtr Parser::returnStatement()
{
    Token keyword = previous();
    ExprPtr value;
    if (!check(TokenType::Semicolon)) {
        value = expression();
    }
    consume(TokenType::Semicolon, "expected `;` after return value");
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(std::make_unique<ReturnStmt>(std::move(keyword), std::move(value)), span);
}

StmtPtr Parser::expressionStatementNoSemicolon()
{
    ExprPtr value = expression();
    std::optional<SourceSpan> span = value ? value->span : std::nullopt;
    return withSpan(std::make_unique<ExpressionStmt>(std::move(value)), span);
}

StmtPtr Parser::expressionStatement()
{
    StmtPtr statement = expressionStatementNoSemicolon();
    consume(TokenType::Semicolon, "expected `;` after expression");
    return statement;
}

ExprPtr Parser::expression()
{
    return assignment();
}

ExprPtr Parser::conditionExpression()
{
    const bool previousAllowStructConstructors = allowStructConstructors_;
    allowStructConstructors_ = false;
    try {
        ExprPtr result = expression();
        allowStructConstructors_ = previousAllowStructConstructors;
        return result;
    } catch (...) {
        allowStructConstructors_ = previousAllowStructConstructors;
        throw;
    }
}

ExprPtr Parser::assignment()
{
    ExprPtr expr = logicalOr();

    if (match(TokenType::Equal)) {
        Token equals = previous();
        ExprPtr value = assignment();

        const std::optional<SourceSpan> span = expr ? expr->span : std::nullopt;
        ExprPtr assignment = buildAssignmentTarget(
            expr,
            [&](Token name) -> ExprPtr {
                return std::make_unique<AssignExpr>(std::move(name), std::move(value));
            },
            [&](ExprPtr collection, Token bracket, ExprPtr indexExpression) -> ExprPtr {
                return std::make_unique<IndexAssignExpr>(
                    std::move(collection), std::move(bracket), std::move(indexExpression), std::move(value));
            },
            [&](ExprPtr object, Token name) -> ExprPtr {
                return std::make_unique<FieldAssignExpr>(std::move(object), std::move(name), std::move(value));
            });
        if (assignment) {
            assignment->span = span;
            return assignment;
        }

        throw ParseError(equals, "invalid assignment target");
    }

    if (matchCompoundAssignment()) {
        Token op = previous();
        ExprPtr value = assignment();

        const std::optional<SourceSpan> span = expr ? expr->span : std::nullopt;
        ExprPtr assignment = buildAssignmentTarget(
            expr,
            [&](Token name) -> ExprPtr {
                return std::make_unique<CompoundAssignExpr>(std::move(name), std::move(op), std::move(value));
            },
            [&](ExprPtr collection, Token bracket, ExprPtr indexExpression) -> ExprPtr {
                return std::make_unique<IndexCompoundAssignExpr>(
                    std::move(collection), std::move(bracket), std::move(indexExpression), std::move(op), std::move(value));
            },
            [&](ExprPtr object, Token name) -> ExprPtr {
                return std::make_unique<FieldCompoundAssignExpr>(std::move(object), std::move(name), std::move(op), std::move(value));
            });
        if (assignment) {
            assignment->span = span;
            return assignment;
        }

        throw ParseError(op, "invalid compound assignment target");
    }

    return expr;
}

bool Parser::matchCompoundAssignment()
{
    return match(TokenType::PlusEqual)
        || match(TokenType::MinusEqual)
        || match(TokenType::StarEqual)
        || match(TokenType::SlashEqual);
}

ExprPtr Parser::logicalOr()
{
    ExprPtr expr = logicalAnd();
    while (match(TokenType::PipePipe)) {
        Token op = previous();
        ExprPtr right = logicalAnd();
        const std::optional<SourceSpan> span = expr ? expr->span : std::nullopt;
        expr = withSpan(
            std::make_unique<LogicalExpr>(std::move(expr), std::move(op), std::move(right)),
            span);
    }
    return expr;
}

ExprPtr Parser::logicalAnd()
{
    ExprPtr expr = equality();
    while (match(TokenType::AmpersandAmpersand)) {
        Token op = previous();
        ExprPtr right = equality();
        const std::optional<SourceSpan> span = expr ? expr->span : std::nullopt;
        expr = withSpan(
            std::make_unique<LogicalExpr>(std::move(expr), std::move(op), std::move(right)),
            span);
    }
    return expr;
}

ExprPtr Parser::equality()
{
    ExprPtr expr = comparison();
    // Left-associative binary operators are folded as they are encountered:
    // `a == b != c` becomes `(!= (== a b) c)`.
    while (match(TokenType::BangEqual) || match(TokenType::EqualEqual)) {
        Token op = previous();
        ExprPtr right = comparison();
        const std::optional<SourceSpan> span = expr ? expr->span : std::nullopt;
        expr = withSpan(
            std::make_unique<BinaryExpr>(std::move(expr), std::move(op), std::move(right)),
            span);
    }
    return expr;
}

ExprPtr Parser::comparison()
{
    ExprPtr expr = term();
    while (match(TokenType::Greater) || match(TokenType::GreaterEqual)
        || match(TokenType::Less) || match(TokenType::LessEqual)) {
        Token op = previous();
        ExprPtr right = term();
        const std::optional<SourceSpan> span = expr ? expr->span : std::nullopt;
        expr = withSpan(
            std::make_unique<BinaryExpr>(std::move(expr), std::move(op), std::move(right)),
            span);
    }
    return expr;
}

ExprPtr Parser::term()
{
    ExprPtr expr = factor();
    while (match(TokenType::Minus) || match(TokenType::Plus)) {
        Token op = previous();
        ExprPtr right = factor();
        const std::optional<SourceSpan> span = expr ? expr->span : std::nullopt;
        expr = withSpan(
            std::make_unique<BinaryExpr>(std::move(expr), std::move(op), std::move(right)),
            span);
    }
    return expr;
}

ExprPtr Parser::factor()
{
    ExprPtr expr = unary();
    while (match(TokenType::Slash) || match(TokenType::Star)) {
        Token op = previous();
        ExprPtr right = unary();
        const std::optional<SourceSpan> span = expr ? expr->span : std::nullopt;
        expr = withSpan(
            std::make_unique<BinaryExpr>(std::move(expr), std::move(op), std::move(right)),
            span);
    }
    return expr;
}

ExprPtr Parser::unary()
{
    if (match(TokenType::Bang) || match(TokenType::Minus)) {
        Token op = previous();
        ExprPtr right = unary();
        const std::optional<SourceSpan> span = spanForToken(op);
        return withSpan(std::make_unique<UnaryExpr>(std::move(op), std::move(right)), span);
    }

    return call();
}

ExprPtr Parser::call()
{
    ExprPtr expr = primary();
    while (true) {
        if (isExplicitTypeArgumentCall(*expr)) {
            std::vector<TypeAnnotation> typeArguments = explicitTypeArguments();
            consume(TokenType::LeftParen, "expected `(` after type arguments");
            expr = finishCall(std::move(expr), std::move(typeArguments));
        } else if (match(TokenType::LeftParen)) {
            expr = finishCall(std::move(expr), {});
        } else if (match(TokenType::LeftBracket)) {
            expr = finishIndex(std::move(expr));
        } else if (match(TokenType::Dot)) {
            expr = finishFieldAccess(std::move(expr));
        } else {
            break;
        }
    }
    return expr;
}

bool Parser::isExplicitTypeArgumentCall(const Expr& callee) const
{
    const Token* calleeName = nullptr;
    if (const auto* variable = dynamic_cast<const VariableExpr*>(&callee)) {
        calleeName = &variable->name;
    } else if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&callee)) {
        calleeName = &field->name;
    }

    if (!calleeName || !check(TokenType::Less)) {
        return false;
    }

    const Token& less = peek();
    if (less.line != calleeName->line
        || less.column != calleeName->column + static_cast<int>(calleeName->lexeme.size())) {
        return false;
    }

    int angleDepth = 0;
    for (std::size_t index = current_; index < tokens_.size(); ++index) {
        if (tokens_[index].type == TokenType::Less) {
            ++angleDepth;
        } else if (tokens_[index].type == TokenType::Greater) {
            --angleDepth;
            if (angleDepth == 0) {
                return index + 1 < tokens_.size()
                    && tokens_[index + 1].type == TokenType::LeftParen;
            }
        } else if (tokens_[index].type == TokenType::EndOfFile) {
            return false;
        }
    }
    return false;
}

std::vector<TypeAnnotation> Parser::explicitTypeArguments()
{
    consume(TokenType::Less, "expected `<` before type arguments");
    std::vector<TypeAnnotation> typeArguments;
    if (check(TokenType::Greater)) {
        throw ParseError(peek(), "expected type argument after `<`");
    }
    do {
        typeArguments.push_back(typeAnnotation("expected type argument after `<`"));
    } while (match(TokenType::Comma));
    consume(TokenType::Greater, "expected `>` after type arguments");
    return typeArguments;
}

ExprPtr Parser::finishCall(ExprPtr callee, std::vector<TypeAnnotation> typeArguments)
{
    const std::optional<SourceSpan> span = callee ? callee->span : std::nullopt;
    std::vector<ExprPtr> arguments;
    if (!check(TokenType::RightParen)) {
        do {
            arguments.push_back(expression());
        } while (match(TokenType::Comma));
    }
    Token paren = consume(TokenType::RightParen, "expected `)` after arguments");

    if (auto* field = dynamic_cast<FieldAccessExpr*>(callee.get())) {
        ExprPtr receiver = std::move(field->object);
        Token name = std::move(field->name);
        return withSpan(
            std::make_unique<MemberCallExpr>(
                std::move(receiver),
                std::move(name),
                std::move(paren),
                std::move(typeArguments),
                std::move(arguments)),
            span);
    }

    return withSpan(
        std::make_unique<CallExpr>(
            std::move(callee),
            std::move(paren),
            std::move(typeArguments),
            std::move(arguments)),
        span);
}

ExprPtr Parser::finishIndex(ExprPtr collection)
{
    const std::optional<SourceSpan> span = collection ? collection->span : std::nullopt;
    Token bracket = previous();
    ExprPtr index = expression();
    consume(TokenType::RightBracket, "expected `]` after index");
    return withSpan(
        std::make_unique<IndexExpr>(std::move(collection), std::move(bracket), std::move(index)),
        span);
}

ExprPtr Parser::finishFieldAccess(ExprPtr object)
{
    const std::optional<SourceSpan> span = object ? object->span : std::nullopt;
    Token name = consume(TokenType::Identifier, "expected field name after `.`");
    return withSpan(std::make_unique<FieldAccessExpr>(std::move(object), std::move(name)), span);
}

ExprPtr Parser::arrayLiteral(Token bracket)
{
    const std::optional<SourceSpan> span = spanForToken(bracket);
    std::vector<ExprPtr> elements;
    if (!check(TokenType::RightBracket)) {
        do {
            elements.push_back(expression());
        } while (match(TokenType::Comma));
    }
    consume(TokenType::RightBracket, "expected `]` after array elements");
    return withSpan(std::make_unique<ArrayExpr>(std::move(bracket), std::move(elements)), span);
}

ExprPtr Parser::mapLiteral(Token brace)
{
    const std::optional<SourceSpan> span = spanForToken(brace);
    std::vector<MapEntry> entries;
    if (!check(TokenType::RightBrace)) {
        do {
            ExprPtr key = expression();
            Token colon = consume(TokenType::Colon, "expected `:` after map key");
            ExprPtr value = expression();
            entries.push_back(MapEntry{std::move(key), std::move(colon), std::move(value)});
        } while (match(TokenType::Comma));
    }
    consume(TokenType::RightBrace, "expected `}` after map entries");
    return withSpan(std::make_unique<MapExpr>(std::move(brace), std::move(entries)), span);
}

std::vector<StructField> Parser::structLiteralFields()
{
    std::vector<StructField> fields;
    if (!check(TokenType::RightBrace)) {
        do {
            Token name = consume(TokenType::Identifier, "expected struct field name");
            consume(TokenType::Colon, "expected `:` after struct field name");
            ExprPtr value = expression();
            fields.push_back(StructField{std::move(name), std::move(value)});
        } while (match(TokenType::Comma));
    }
    return fields;
}

ExprPtr Parser::structConstructor()
{
    Token name = consume(TokenType::Identifier, "expected struct constructor name");
    const std::optional<SourceSpan> span = spanForToken(name);
    std::vector<TypeAnnotation> typeArguments;
    if (check(TokenType::Less)) {
        typeArguments = explicitTypeArguments();
    }
    consume(TokenType::LeftBrace, "expected `{` after struct constructor name");
    std::vector<StructField> fields = structLiteralFields();
    consume(TokenType::RightBrace, "expected `}` after struct fields");
    return withSpan(
        std::make_unique<StructConstructExpr>(
            std::nullopt, std::move(name), std::move(typeArguments), std::move(fields)),
        span);
}

bool Parser::isStructConstructorStart() const
{
    if (!check(TokenType::Identifier)) {
        return false;
    }
    std::size_t index = current_ + 1;
    if (index < tokens_.size() && tokens_[index].type == TokenType::LeftBrace) {
        return true;
    }
    if (index >= tokens_.size() || tokens_[index].type != TokenType::Less) {
        return false;
    }
    int depth = 0;
    for (; index < tokens_.size(); ++index) {
        if (tokens_[index].type == TokenType::Less) {
            ++depth;
        } else if (tokens_[index].type == TokenType::Greater) {
            --depth;
            if (depth == 0) {
                return index + 1 < tokens_.size()
                    && tokens_[index + 1].type == TokenType::LeftBrace;
            }
        } else if (tokens_[index].type == TokenType::EndOfFile) {
            return false;
        }
    }
    return false;
}

bool Parser::isQualifiedStructConstructorStart() const
{
    if (!check(TokenType::Identifier)
        || current_ + 2 >= tokens_.size()
        || tokens_[current_ + 1].type != TokenType::Dot
        || tokens_[current_ + 2].type != TokenType::Identifier) {
        return false;
    }
    std::size_t index = current_ + 3;
    if (index < tokens_.size() && tokens_[index].type == TokenType::LeftBrace) {
        return true;
    }
    if (index >= tokens_.size() || tokens_[index].type != TokenType::Less) {
        return false;
    }
    int depth = 0;
    for (; index < tokens_.size(); ++index) {
        if (tokens_[index].type == TokenType::Less) {
            ++depth;
        } else if (tokens_[index].type == TokenType::Greater) {
            --depth;
            if (depth == 0) {
                return index + 1 < tokens_.size()
                    && tokens_[index + 1].type == TokenType::LeftBrace;
            }
        } else if (tokens_[index].type == TokenType::EndOfFile) {
            return false;
        }
    }
    return false;
}

ExprPtr Parser::qualifiedStructConstructor()
{
    Token qualifier = consume(TokenType::Identifier, "expected namespace alias before `.`");
    const std::optional<SourceSpan> span = spanForToken(qualifier);
    consume(TokenType::Dot, "expected `.` after namespace alias");
    Token name = consume(TokenType::Identifier, "expected struct constructor name after `.`");
    std::vector<TypeAnnotation> typeArguments;
    if (check(TokenType::Less)) {
        typeArguments = explicitTypeArguments();
    }
    consume(TokenType::LeftBrace, "expected `{` after struct constructor name");
    std::vector<StructField> fields = structLiteralFields();
    consume(TokenType::RightBrace, "expected `}` after struct fields");
    return withSpan(
        std::make_unique<StructConstructExpr>(
            std::move(qualifier), std::move(name), std::move(typeArguments), std::move(fields)),
        span);
}

ExprPtr Parser::functionExpression()
{
    Token keyword = previous();
    std::vector<TypeParameter> parsedTypeParameters = typeParameters();
    consume(TokenType::LeftParen, "expected `(` after `fun`");

    std::vector<Parameter> parsedParameters = parameters();
    consume(TokenType::RightParen, "expected `)` after function parameters");
    std::optional<TypeAnnotation> returnTypeName = optionalReturnType();
    consume(TokenType::LeftBrace, "expected `{` before function body");
    ++blockDepth_;
    std::vector<StmtPtr> body = blockStatements();
    --blockDepth_;
    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<FunctionExpr>(
            std::move(keyword),
            std::move(parsedTypeParameters),
            std::move(parsedParameters),
            std::move(returnTypeName),
            std::move(body)),
        span);
}

ExprPtr Parser::primary()
{
    if (match(TokenType::Fun)) {
        return functionExpression();
    }
    if (match(TokenType::Match)) {
        return matchExpression();
    }
    if (match(TokenType::False)) {
        return withSpan(std::make_unique<LiteralExpr>("false"), previous());
    }
    if (match(TokenType::True)) {
        return withSpan(std::make_unique<LiteralExpr>("true"), previous());
    }
    if (match(TokenType::Nil)) {
        return withSpan(std::make_unique<LiteralExpr>("nil"), previous());
    }
    if (match(TokenType::Number) || match(TokenType::String)) {
        Token token = previous();
        return withSpan(std::make_unique<LiteralExpr>(token.lexeme), token);
    }
    if (match(TokenType::LeftBracket)) {
        Token bracket = previous();
        return arrayLiteral(std::move(bracket));
    }
    if (match(TokenType::LeftBrace)) {
        Token brace = previous();
        return mapLiteral(std::move(brace));
    }
    if (allowStructConstructors_ && isQualifiedStructConstructorStart()) {
        return qualifiedStructConstructor();
    }
    if (allowStructConstructors_ && isStructConstructorStart()) {
        return structConstructor();
    }
    if (match(TokenType::Identifier)) {
        Token name = previous();
        return withSpan(std::make_unique<VariableExpr>(name), name);
    }
    if (match(TokenType::LeftParen)) {
        Token paren = previous();
        ExprPtr expr = expression();
        consume(TokenType::RightParen, "expected `)` after expression");
        return withSpan(std::make_unique<GroupingExpr>(std::move(expr)), paren);
    }

    throw ParseError(peek(), "expected expression");
}

ExprPtr Parser::matchExpression()
{
    Token keyword = previous();
    ExprPtr value = conditionExpression();
    consume(TokenType::LeftBrace, "expected `{` after match value");

    std::vector<MatchExprArm> arms;
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        PatternPtr armPattern = pattern();
        ExprPtr guard;
        if (match(TokenType::If)) {
            guard = conditionExpression();
        }
        Token arrow = consume(TokenType::FatArrow, "expected `=>` after match pattern or guard");
        ExprPtr armValue = expression();
        arms.push_back(MatchExprArm{std::move(arrow), std::move(armPattern), std::move(guard), std::move(armValue)});

        if (!match(TokenType::Comma) && !check(TokenType::RightBrace)) {
            throw ParseError(peek(), "expected `,` after match expression arm");
        }
    }
    consume(TokenType::RightBrace, "expected `}` after match expression arms");

    const std::optional<SourceSpan> span = spanForToken(keyword);
    return withSpan(
        std::make_unique<MatchExpr>(std::move(keyword), std::move(value), std::move(arms)),
        span);
}

PatternPtr Parser::pattern()
{
    PatternPtr first = patternAtom();
    if (!match(TokenType::Pipe)) {
        return first;
    }

    Token pipe = previous();
    const std::optional<SourceSpan> span = first ? first->span : spanForToken(pipe);
    std::vector<PatternPtr> alternatives;
    alternatives.push_back(std::move(first));
    do {
        alternatives.push_back(patternAtom());
    } while (match(TokenType::Pipe));

    return withSpan(
        std::make_unique<OrPattern>(std::move(pipe), std::move(alternatives)),
        span);
}

PatternPtr Parser::patternAtom()
{
    if (check(TokenType::Identifier) && checkNext(TokenType::LeftBrace)) {
        Token name = advance();
        return recordPattern(std::nullopt, std::move(name));
    }
    if (isQualifiedRecordPatternStart()) {
        Token qualifier = advance();
        consume(TokenType::Dot, "expected `.` after record pattern qualifier");
        Token name = consume(TokenType::Identifier, "expected record pattern name after `.`");
        return recordPattern(std::move(qualifier), std::move(name));
    }

    if (match(TokenType::Nil) || match(TokenType::True) || match(TokenType::False)
        || match(TokenType::Number) || match(TokenType::String)) {
        Token value = previous();
        return withSpan(std::make_unique<LiteralPattern>(value), value);
    }

    Token qualifierOrName = consume(TokenType::Identifier, "expected match pattern");
    if (match(TokenType::Dot)) {
        Token name = consume(TokenType::Identifier, "expected variant name after `.`");
        if (match(TokenType::Dot)) {
            Token variant = consume(TokenType::Identifier, "expected variant name after enum type");
            qualifierOrName.lexeme += "." + name.lexeme;
            return variantPattern(std::move(qualifierOrName), std::move(variant));
        }
        return variantPattern(std::move(qualifierOrName), std::move(name));
    }

    if (qualifierOrName.lexeme == "_") {
        return withSpan(std::make_unique<WildcardPattern>(qualifierOrName), qualifierOrName);
    }
    return withSpan(std::make_unique<VariablePattern>(qualifierOrName), qualifierOrName);
}

PatternPtr Parser::recordPattern(std::optional<Token> qualifier, Token name)
{
    const Token spanToken = qualifier ? *qualifier : name;
    const std::optional<SourceSpan> span = spanForToken(spanToken);
    consume(TokenType::LeftBrace, "expected `{` after record pattern name");

    std::vector<RecordPatternField> fields;
    if (!check(TokenType::RightBrace)) {
        do {
            Token fieldName = consume(TokenType::Identifier, "expected record pattern field name");
            consume(TokenType::Colon, "expected `:` after record pattern field name");
            fields.push_back(RecordPatternField{std::move(fieldName), pattern()});
        } while (match(TokenType::Comma));
    }
    consume(TokenType::RightBrace, "expected `}` after record pattern fields");
    return withSpan(
        std::make_unique<RecordPattern>(std::move(qualifier), std::move(name), std::move(fields)),
        span);
}

bool Parser::isQualifiedRecordPatternStart() const
{
    return check(TokenType::Identifier)
        && current_ + 3 < tokens_.size()
        && tokens_[current_ + 1].type == TokenType::Dot
        && tokens_[current_ + 2].type == TokenType::Identifier
        && tokens_[current_ + 3].type == TokenType::LeftBrace;
}

PatternPtr Parser::variantPattern(Token qualifier, Token name)
{
    const std::optional<SourceSpan> span = spanForToken(qualifier);
    std::vector<PatternPtr> arguments;
    std::vector<std::optional<Token>> argumentNames;
    if (match(TokenType::LeftParen)) {
        if (!check(TokenType::RightParen)) {
            do {
                std::optional<Token> argumentName;
                if (check(TokenType::Identifier) && checkNext(TokenType::Colon)) {
                    argumentName = advance();
                    consume(TokenType::Colon, "expected `:` after pattern payload name");
                }
                argumentNames.push_back(std::move(argumentName));
                arguments.push_back(pattern());
            } while (match(TokenType::Comma));
        }
        consume(TokenType::RightParen, "expected `)` after variant pattern");
    }
    return withSpan(
        std::make_unique<VariantPattern>(
            std::move(qualifier), std::move(name), std::move(arguments), std::move(argumentNames)),
        span);
}

bool Parser::match(TokenType type)
{
    if (!check(type)) {
        return false;
    }
    advance();
    return true;
}

bool Parser::matchContextualIdentifier(const std::string& lexeme)
{
    if (!checkContextualIdentifier(lexeme)) {
        return false;
    }
    advance();
    return true;
}

bool Parser::check(TokenType type) const
{
    if (isAtEnd()) {
        return type == TokenType::EndOfFile;
    }
    return peek().type == type;
}

bool Parser::checkContextualIdentifier(const std::string& lexeme) const
{
    return check(TokenType::Identifier) && peek().lexeme == lexeme;
}

bool Parser::checkNext(TokenType type) const
{
    if (current_ + 1 >= tokens_.size()) {
        return false;
    }
    return tokens_[current_ + 1].type == type;
}

Token Parser::advance()
{
    if (!isAtEnd()) {
        current_++;
    }
    return previous();
}

bool Parser::isAtEnd() const
{
    return peek().type == TokenType::EndOfFile;
}

const Token& Parser::peek() const
{
    return tokens_[current_];
}

const Token& Parser::previous() const
{
    return tokens_[current_ - 1];
}

Token Parser::consume(TokenType type, const std::string& message)
{
    if (check(type)) {
        return advance();
    }

    // Include both the expected construct and the actual token to make syntax
    // errors actionable for users of the command-line demo.
    std::ostringstream fullMessage;
    fullMessage << message << ", found " << tokenTypeName(peek().type);
    if (!peek().lexeme.empty()) {
        fullMessage << " `" << peek().lexeme << "`";
    }
    throw ParseError(peek(), fullMessage.str());
}
