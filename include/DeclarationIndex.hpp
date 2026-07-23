#pragma once

#include "Ast.hpp"
#include "TypeUtils.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ResolvedNames;
class TypeChecker;
class DeclarationIndexCollector;

enum class DeclarationKind {
    Module,
    Variable,
    Function,
    Parameter,
    ForInVariable,
    Struct,
    Enum,
    Method,
    NamespaceAlias,
};

std::string declarationKindName(DeclarationKind kind);

struct ResolvedSymbol {
    DeclarationId declarationId;
    SymbolId symbolId;
};

enum class CallTargetKind {
    Direct,
    StructMethod,
};

struct CallTargetRecord {
    CallTargetKind kind = CallTargetKind::Direct;
    ResolvedSymbol target;
};

struct TypedExpressionRecord {
    TypeInfo type;
};

struct NativeCallRecord {
    std::string name;
};

struct VariantConstructorRecord {
    std::string enumName;
    std::string variantName;
};

struct DeclarationSignature {
    std::vector<TypeParameter> typeParameters;
    std::vector<Parameter> parameters;
    std::optional<TypeAnnotation> returnType;
};

struct DeclarationShape {
    std::vector<StructFieldDecl> structFields;
    std::vector<EnumVariantDecl> enumVariants;
};

struct DeclarationRecord {
    DeclarationId declarationId;
    SymbolId symbolId;
    DeclarationKind kind = DeclarationKind::Variable;
    std::string name;
    ScopeId scopeId;
    std::optional<SourceRange> range;
    std::optional<SyntaxNodeId> syntaxNodeId;
    const Stmt* statement = nullptr;
    const MethodDecl* method = nullptr;
    const Parameter* parameter = nullptr;
    std::string ownerType;
    std::vector<TypeParameter> typeParameters;
    std::vector<Parameter> parameters;
    std::optional<TypeAnnotation> returnType;
};

struct ScopeRecord {
    ScopeId id;
    std::optional<ScopeId> parent;
    std::optional<SyntaxNodeId> ownerSyntaxNode;
    std::unordered_map<std::string, DeclarationId> declarations;
};

struct ImportRecord {
    const ImportStmt* statement = nullptr;
    std::size_t resolvedModuleId = static_cast<std::size_t>(-1);
    std::optional<std::string> alias;
};

struct ExportRecord {
    const ExportStmt* statement = nullptr;
    std::vector<std::string> names;
    std::size_t resolvedModuleId = static_cast<std::size_t>(-1);
    std::optional<std::string> sourcePath;
};

// A snapshot-local declaration/symbol index collected from the existing AST.
// It is intentionally independent of TypeChecker's type decisions: the
// migration slices record declarations, scopes, signatures, lexical
// references, and typed expression results while the old checker remains the
// behavior oracle.
class DeclarationIndex {
public:
    static DeclarationIndex collect(const Program& program);

    const std::vector<DeclarationRecord>& declarations() const;
    const std::vector<ScopeRecord>& scopes() const;
    const std::vector<ImportRecord>& imports() const;
    const std::vector<ExportRecord>& exports() const;

    const DeclarationRecord* declaration(DeclarationId id) const;
    const DeclarationRecord* declaration(const Stmt& statement) const;
    const DeclarationRecord* declaration(const MethodDecl& method) const;
    const DeclarationRecord* declaration(const Parameter& parameter) const;
    const DeclarationRecord* declaration(const VariablePattern& pattern) const;
    std::optional<DeclarationSignature> signature(DeclarationId id) const;
    std::optional<DeclarationShape> shape(DeclarationId id) const;
    const ScopeRecord* scope(ScopeId id) const;
    std::optional<ScopeId> scopeFor(const Stmt& statement) const;
    std::optional<ResolvedSymbol> forInBinding(const ForInStmt& statement) const;
    std::optional<ResolvedSymbol> patternBinding(const VariablePattern& pattern) const;
    const CallTargetRecord* callTarget(const CallExpr& expression) const;
    const CallTargetRecord* callTarget(const MemberCallExpr& expression) const;
    const TypedExpressionRecord* typedExpression(const Expr& expression) const;
    const NativeCallRecord* nativeCall(const Expr& expression) const;
    const VariantConstructorRecord* variantConstructor(const MemberCallExpr& expression) const;

    std::optional<DeclarationId> lookup(ScopeId scopeId, const std::string& name) const;
    std::optional<ResolvedSymbol> variableReference(const VariableExpr& expression) const;
    std::optional<ResolvedSymbol> assignmentReference(const AssignExpr& expression) const;
    std::optional<ResolvedSymbol> compoundAssignmentReference(const CompoundAssignExpr& expression) const;

    // Compares the collected declaration/reference shape with the legacy
    // ResolvedNames table. IDs are owned by different migration snapshots, so
    // comparison uses declaration kind/name/range rather than raw integers.
    std::size_t compareResolvedNames(const ResolvedNames& resolved);

private:
    friend class DeclarationIndexCollector;
    friend class TypeChecker;

    void recordTypedExpression(const Expr& expression, TypeInfo type);
    void recordNativeCall(const Expr& expression, std::string name);
    void recordVariantConstructor(
        const MemberCallExpr& expression,
        std::string enumName,
        std::string variantName);

    std::vector<DeclarationRecord> declarations_;
    std::vector<ScopeRecord> scopes_;
    std::vector<ImportRecord> imports_;
    std::vector<ExportRecord> exports_;
    std::unordered_map<const Stmt*, DeclarationId> statementDeclarations_;
    std::unordered_map<const MethodDecl*, DeclarationId> methodDeclarations_;
    std::unordered_map<const Parameter*, DeclarationId> parameterDeclarations_;
    std::unordered_map<const VariablePattern*, DeclarationId> patternDeclarations_;
    std::unordered_map<const Stmt*, ScopeId> statementScopes_;
    std::unordered_map<const CallExpr*, const VariableExpr*> directCallCallees_;
    std::unordered_map<const CallExpr*, CallTargetRecord> callTargets_;
    std::unordered_map<const MemberCallExpr*, std::string> memberCallCandidates_;
    std::unordered_map<const MemberCallExpr*, CallTargetRecord> memberCallTargets_;
    std::unordered_map<const VariableExpr*, ResolvedSymbol> variableReferences_;
    std::unordered_map<const AssignExpr*, ResolvedSymbol> assignmentReferences_;
    std::unordered_map<const CompoundAssignExpr*, ResolvedSymbol> compoundAssignmentReferences_;
    std::unordered_set<const FieldAccessExpr*> fieldAccesses_;
    std::unordered_set<const FieldAssignExpr*> fieldAssignments_;
    std::unordered_set<const FieldCompoundAssignExpr*> fieldCompoundAssignments_;
    std::unordered_set<const IndexExpr*> indexExpressions_;
    std::unordered_set<const IndexAssignExpr*> indexAssignments_;
    std::unordered_set<const IndexCompoundAssignExpr*> indexCompoundAssignments_;
    std::unordered_set<const ArrayExpr*> arrayExpressions_;
    std::unordered_set<const MapExpr*> mapExpressions_;
    std::unordered_set<const StructConstructExpr*> structConstructors_;
    std::unordered_map<const Expr*, std::string> nativeCallCandidates_;
    std::unordered_map<const Expr*, TypedExpressionRecord> typedExpressions_;
    std::unordered_map<const Expr*, NativeCallRecord> nativeCalls_;
    std::unordered_map<const MemberCallExpr*, VariantConstructorRecord> variantConstructors_;
};
