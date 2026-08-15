#pragma once

#include "ModuleGraph.hpp"
#include "SourceMap.hpp"
#include "Token.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

// Base class for all expression nodes. Each node knows how to print itself
// in a compact prefix representation used by this demo front-end.
struct Expr {
    virtual ~Expr() = default;
    virtual void print(std::ostream& out) const = 0;

    std::optional<SourceSpan> span;
    std::optional<SourceRange> range;
    std::optional<SyntaxNodeId> syntaxNodeId;
};

using ExprPtr = std::unique_ptr<Expr>;

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct TypeAnnotation {
    enum class Kind {
        Simple,
        Qualified,
        Function,
        Array,
        Map,
        Nullable,
        Optional,
    };

    static TypeAnnotation simple(Token token);
    static TypeAnnotation qualified(Token qualifier, Token name);
    static TypeAnnotation function(Token token, std::vector<TypeAnnotation> parameterTypes, TypeAnnotation returnType);
    static TypeAnnotation array(Token token, TypeAnnotation elementType);
    static TypeAnnotation map(Token token, TypeAnnotation keyType, TypeAnnotation valueType);
    static TypeAnnotation nullable(Token token, TypeAnnotation innerType);
    static TypeAnnotation optional(Token token, TypeAnnotation innerType);

    Kind kind = Kind::Simple;
    Token token{TokenType::Identifier, "", 0, 0};
    Token qualifier{TokenType::Identifier, "", 0, 0};
    std::vector<TypeAnnotation> parameterTypes;
    std::vector<TypeAnnotation> typeArguments;
    std::shared_ptr<TypeAnnotation> returnType;
    std::shared_ptr<TypeAnnotation> elementType;
    std::shared_ptr<TypeAnnotation> keyType;
    std::shared_ptr<TypeAnnotation> valueType;
    std::shared_ptr<TypeAnnotation> innerType;
};

struct TypeParameter {
    Token name;
    std::vector<TypeAnnotation> constraints;
};

struct Parameter {
    Token name;
    std::optional<TypeAnnotation> typeName;
};

struct MethodDecl {
    MethodDecl(
        Token name,
        std::vector<TypeParameter> typeParameters,
        std::vector<Parameter> parameters,
        std::optional<TypeAnnotation> returnTypeName,
        std::vector<StmtPtr> body);

    Token name;
    std::vector<TypeParameter> typeParameters;
    std::vector<Parameter> parameters;
    std::optional<TypeAnnotation> returnTypeName;
    std::vector<StmtPtr> body;
    std::optional<SourceRange> range;
    std::optional<SyntaxNodeId> syntaxNodeId;
};

struct LiteralExpr final : Expr {
    explicit LiteralExpr(std::string value);
    void print(std::ostream& out) const override;

    std::string value;
};

struct VariableExpr final : Expr {
    explicit VariableExpr(Token name);
    void print(std::ostream& out) const override;

    Token name;
};

struct AssignExpr final : Expr {
    AssignExpr(Token name, ExprPtr value);
    void print(std::ostream& out) const override;

    Token name;
    ExprPtr value;
};

struct CompoundAssignExpr final : Expr {
    CompoundAssignExpr(Token name, Token op, ExprPtr value);
    void print(std::ostream& out) const override;

    Token name;
    Token op;
    ExprPtr value;
};

struct IndexAssignExpr final : Expr {
    IndexAssignExpr(ExprPtr collection, Token bracket, ExprPtr index, ExprPtr value);
    void print(std::ostream& out) const override;

    ExprPtr collection;
    Token bracket;
    ExprPtr index;
    ExprPtr value;
};

struct IndexCompoundAssignExpr final : Expr {
    IndexCompoundAssignExpr(ExprPtr collection, Token bracket, ExprPtr index, Token op, ExprPtr value);
    void print(std::ostream& out) const override;

    ExprPtr collection;
    Token bracket;
    ExprPtr index;
    Token op;
    ExprPtr value;
};

struct UnaryExpr final : Expr {
    UnaryExpr(Token op, ExprPtr right);
    void print(std::ostream& out) const override;

    Token op;
    ExprPtr right;
};

struct BinaryExpr final : Expr {
    BinaryExpr(ExprPtr left, Token op, ExprPtr right);
    void print(std::ostream& out) const override;

    ExprPtr left;
    Token op;
    ExprPtr right;
};

struct LogicalExpr final : Expr {
    LogicalExpr(ExprPtr left, Token op, ExprPtr right);
    void print(std::ostream& out) const override;

    ExprPtr left;
    Token op;
    ExprPtr right;
};

struct CoalesceExpr final : Expr {
    CoalesceExpr(ExprPtr left, Token op, ExprPtr right);
    void print(std::ostream& out) const override;

    ExprPtr left;
    Token op;
    ExprPtr right;
};

struct UnwrapOrReturnExpr final : Expr {
    explicit UnwrapOrReturnExpr(ExprPtr value, Token op);
    void print(std::ostream& out) const override;

    ExprPtr value;
    Token op;
};

struct GroupingExpr final : Expr {
    explicit GroupingExpr(ExprPtr expression);
    void print(std::ostream& out) const override;

    ExprPtr expression;
};

struct CallExpr final : Expr {
    CallExpr(
        ExprPtr callee,
        Token paren,
        std::vector<TypeAnnotation> typeArguments,
        std::vector<ExprPtr> arguments);
    void print(std::ostream& out) const override;

    ExprPtr callee;
    Token paren;
    std::vector<TypeAnnotation> typeArguments;
    std::vector<ExprPtr> arguments;
};

struct MemberCallExpr final : Expr {
    MemberCallExpr(
        ExprPtr receiver,
        Token name,
        Token paren,
        std::vector<TypeAnnotation> typeArguments,
        std::vector<ExprPtr> arguments);
    void print(std::ostream& out) const override;

    ExprPtr receiver;
    Token name;
    Token paren;
    std::vector<TypeAnnotation> typeArguments;
    std::vector<ExprPtr> arguments;
};

struct ArrayExpr final : Expr {
    ArrayExpr(Token bracket, std::vector<ExprPtr> elements);
    void print(std::ostream& out) const override;

    Token bracket;
    std::vector<ExprPtr> elements;
};

struct MapEntry {
    ExprPtr key;
    Token colon;
    ExprPtr value;
};

struct MapExpr final : Expr {
    MapExpr(Token brace, std::vector<MapEntry> entries);
    void print(std::ostream& out) const override;

    Token brace;
    std::vector<MapEntry> entries;
};

struct StructField {
    Token name;
    ExprPtr value;
};

struct StructConstructExpr final : Expr {
    StructConstructExpr(
        std::optional<Token> qualifier,
        Token name,
        std::vector<TypeAnnotation> typeArguments,
        std::vector<StructField> fields);
    void print(std::ostream& out) const override;

    std::optional<Token> qualifier;
    Token name;
    std::vector<TypeAnnotation> typeArguments;
    std::vector<StructField> fields;
};

struct StructFieldDecl {
    Token name;
    TypeAnnotation typeName;
    bool isPrivate = false;
};

struct EnumVariantDecl {
    Token name;
    std::vector<TypeAnnotation> payloadTypes;
    std::vector<std::optional<Token>> payloadNames;
};

struct IndexExpr final : Expr {
    IndexExpr(ExprPtr collection, Token bracket, ExprPtr index);
    void print(std::ostream& out) const override;

    ExprPtr collection;
    Token bracket;
    ExprPtr index;
};

struct FieldAccessExpr final : Expr {
    FieldAccessExpr(ExprPtr object, Token name);
    void print(std::ostream& out) const override;

    ExprPtr object;
    Token name;
};

struct FieldAssignExpr final : Expr {
    FieldAssignExpr(ExprPtr object, Token name, ExprPtr value);
    void print(std::ostream& out) const override;

    ExprPtr object;
    Token name;
    ExprPtr value;
};

struct FieldCompoundAssignExpr final : Expr {
    FieldCompoundAssignExpr(ExprPtr object, Token name, Token op, ExprPtr value);
    void print(std::ostream& out) const override;

    ExprPtr object;
    Token name;
    Token op;
    ExprPtr value;
};

struct FunctionExpr final : Expr {
    FunctionExpr(
        Token keyword,
        std::vector<TypeParameter> typeParameters,
        std::vector<Parameter> parameters,
        std::optional<TypeAnnotation> returnTypeName,
        std::vector<StmtPtr> body);
    void print(std::ostream& out) const override;

    Token keyword;
    std::vector<TypeParameter> typeParameters;
    std::vector<Parameter> parameters;
    std::optional<TypeAnnotation> returnTypeName;
    std::vector<StmtPtr> body;
};

struct Stmt {
    virtual ~Stmt() = default;
    virtual void print(std::ostream& out, int indent) const = 0;

    std::optional<SourceSpan> span;
    std::optional<SourceRange> range;
    std::optional<SyntaxNodeId> syntaxNodeId;
};

struct Pattern {
    virtual ~Pattern() = default;
    virtual void print(std::ostream& out) const = 0;

    std::optional<SourceSpan> span;
    std::optional<SourceRange> range;
    std::optional<SyntaxNodeId> syntaxNodeId;
};

using PatternPtr = std::unique_ptr<Pattern>;

struct WildcardPattern final : Pattern {
    explicit WildcardPattern(Token name);
    void print(std::ostream& out) const override;

    Token name;
};

struct VariablePattern final : Pattern {
    explicit VariablePattern(Token name);
    void print(std::ostream& out) const override;

    Token name;
};

struct LiteralPattern final : Pattern {
    explicit LiteralPattern(Token value);
    void print(std::ostream& out) const override;

    Token value;
};

struct OrPattern final : Pattern {
    OrPattern(Token pipe, std::vector<PatternPtr> alternatives);
    void print(std::ostream& out) const override;

    Token pipe;
    std::vector<PatternPtr> alternatives;
};

struct RecordPatternField {
    Token name;
    PatternPtr pattern;
};

struct RecordPattern final : Pattern {
    RecordPattern(
        std::optional<Token> qualifier,
        Token name,
        std::vector<RecordPatternField> fields);
    void print(std::ostream& out) const override;

    std::optional<Token> qualifier;
    Token name;
    std::vector<RecordPatternField> fields;
};

struct VariantPattern final : Pattern {
    VariantPattern(
        std::optional<Token> qualifier,
        Token name,
        std::vector<PatternPtr> arguments);
    void print(std::ostream& out) const override;

    std::optional<Token> qualifier;
    Token name;
    std::vector<PatternPtr> arguments;
};

struct EnumDeclStmt final : Stmt {
    EnumDeclStmt(Token name, std::vector<TypeParameter> typeParameters, std::vector<EnumVariantDecl> variants);
    void print(std::ostream& out, int indent) const override;

    Token name;
    std::vector<TypeParameter> typeParameters;
    std::vector<EnumVariantDecl> variants;
};

struct MatchArm {
    PatternPtr pattern;
    ExprPtr guard;
    StmtPtr body;
    ExprPtr expression;
};

struct MatchExpr final : Expr {
    MatchExpr(ExprPtr value, std::vector<MatchArm> arms);
    void print(std::ostream& out) const override;

    ExprPtr value;
    std::vector<MatchArm> arms;
};

struct StructDeclStmt final : Stmt {
    StructDeclStmt(Token name, std::vector<TypeParameter> typeParameters, std::vector<StructFieldDecl> fields);
    void print(std::ostream& out, int indent) const override;

    Token name;
    std::vector<TypeParameter> typeParameters;
    std::vector<StructFieldDecl> fields;
};

struct ImplStmt final : Stmt {
    ImplStmt(Token typeName, std::vector<TypeParameter> typeParameters, std::vector<MethodDecl> methods);
    void print(std::ostream& out, int indent) const override;

    Token typeName;
    std::vector<TypeParameter> typeParameters;
    std::vector<MethodDecl> methods;
};

struct ImportStmt final : Stmt {
    ImportStmt(Token keyword, Token path, std::optional<Token> alias);
    void print(std::ostream& out, int indent) const override;

    Token keyword;
    Token path;
    std::optional<Token> alias;
    std::size_t resolvedModuleId = static_cast<std::size_t>(-1);
};

struct ExportStmt final : Stmt {
    ExportStmt(Token keyword, std::vector<Token> names, std::optional<Token> sourcePath = std::nullopt);
    void print(std::ostream& out, int indent) const override;

    Token keyword;
    std::vector<Token> names;
    std::optional<Token> sourcePath;
    std::size_t resolvedModuleId = static_cast<std::size_t>(-1);
};

struct ModuleStmt final : Stmt {
    ModuleStmt(
        std::size_t moduleId,
        std::string path,
        std::string source,
        std::vector<StmtPtr> statements,
        bool isEntry,
        SourceFileId sourceId = SourceFileId{});
    void print(std::ostream& out, int indent) const override;

    std::size_t moduleId;
    SourceFileId sourceId;
    std::string path;
    std::string source;
    // Stable source digest used by independently loaded module sidecars.  It
    // is a build product field, not a snapshot-local identity.
    std::string sourceHash;
    std::vector<StmtPtr> statements;
    // False when the body was intentionally omitted because a validated
    // serialized interface is serving as the dependency semantic boundary.
    // Such a module remains a graph transport node and is not directly
    // lowerable by the source AST path.
    bool bodySourceBacked = true;
    bool isEntry = false;
};

struct LetStmt final : Stmt {
    LetStmt(Token name, std::optional<TypeAnnotation> typeName, ExprPtr initializer);
    void print(std::ostream& out, int indent) const override;

    Token name;
    std::optional<TypeAnnotation> typeName;
    ExprPtr initializer;
};

struct PrintStmt final : Stmt {
    explicit PrintStmt(ExprPtr expression);
    void print(std::ostream& out, int indent) const override;

    ExprPtr expression;
};

struct ExpressionStmt final : Stmt {
    explicit ExpressionStmt(ExprPtr expression);
    void print(std::ostream& out, int indent) const override;

    ExprPtr expression;
};

struct BlockStmt final : Stmt {
    explicit BlockStmt(std::vector<StmtPtr> statements);
    void print(std::ostream& out, int indent) const override;

    std::vector<StmtPtr> statements;
};

struct IfStmt final : Stmt {
    IfStmt(ExprPtr condition, StmtPtr thenBranch, StmtPtr elseBranch);
    void print(std::ostream& out, int indent) const override;

    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch;
};

struct IfLetStmt final : Stmt {
    IfLetStmt(
        Token keyword,
        Token variable,
        ExprPtr value,
        StmtPtr thenBranch,
        StmtPtr elseBranch);
    void print(std::ostream& out, int indent) const override;

    Token keyword;
    Token variable;
    ExprPtr value;
    StmtPtr thenBranch;
    StmtPtr elseBranch;
};

struct WhileStmt final : Stmt {
    WhileStmt(ExprPtr condition, StmtPtr body);
    void print(std::ostream& out, int indent) const override;

    ExprPtr condition;
    StmtPtr body;
};

struct WhileLetStmt final : Stmt {
    WhileLetStmt(Token keyword, Token variable, ExprPtr value, StmtPtr body);
    void print(std::ostream& out, int indent) const override;

    Token keyword;
    Token variable;
    ExprPtr value;
    StmtPtr body;
};

struct ForStmt final : Stmt {
    ForStmt(Token keyword, StmtPtr initializer, ExprPtr condition, ExprPtr increment, StmtPtr body);
    void print(std::ostream& out, int indent) const override;

    Token keyword;
    StmtPtr initializer;
    ExprPtr condition;
    ExprPtr increment;
    StmtPtr body;
};

struct ForInStmt final : Stmt {
    ForInStmt(Token keyword, Token variable, ExprPtr iterable, StmtPtr body);
    void print(std::ostream& out, int indent) const override;

    Token keyword;
    Token variable;
    ExprPtr iterable;
    StmtPtr body;
};

struct BreakStmt final : Stmt {
    explicit BreakStmt(Token keyword);
    void print(std::ostream& out, int indent) const override;

    Token keyword;
};

struct ContinueStmt final : Stmt {
    explicit ContinueStmt(Token keyword);
    void print(std::ostream& out, int indent) const override;

    Token keyword;
};

struct FunctionStmt final : Stmt {
    FunctionStmt(Token name, std::vector<TypeParameter> typeParameters, std::vector<Parameter> parameters, std::optional<TypeAnnotation> returnTypeName, std::vector<StmtPtr> body);
    void print(std::ostream& out, int indent) const override;

    Token name;
    std::vector<TypeParameter> typeParameters;
    std::vector<Parameter> parameters;
    std::optional<TypeAnnotation> returnTypeName;
    std::vector<StmtPtr> body;
};

struct ReturnStmt final : Stmt {
    ReturnStmt(Token keyword, ExprPtr value);
    void print(std::ostream& out, int indent) const override;

    Token keyword;
    ExprPtr value;
};

struct Program {
    std::vector<StmtPtr> statements;
    std::vector<SourceFile> sources;
    std::optional<ModuleGraph> moduleGraph;

    // Emit a readable tree view of the parsed program.
    void print(std::ostream& out) const;
};

// Complete the source metadata produced by the parser.  IDs are allocated in
// traversal order and are therefore stable for the lifetime of one Program
// snapshot.  FrontendSession calls this once after assembling all source
// units, so IDs do not collide across modules or imports.
void populateSyntaxRanges(Program& program);
void assignSyntaxNodeIds(Program& program);
void finalizeSyntaxMetadata(Program& program);
