#pragma once

#include "Ast.hpp"
#include "TypeUtils.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

struct BindingMetadataRecord {
    std::string resolvedName;
    BindingId bindingId;
    ResolvedSymbol symbol;
    std::optional<SourceRange> range;
    // Imported bindings are interface copies without snapshot-local identity;
    // reference metadata for them is skipped by validateMetadata.
    bool imported = false;
};

struct FunctionMetadataRecord {
    std::string resolvedName;
    std::string functionLabel;
    std::vector<std::string> parameterNames;
    // Binding IDs are produced by the checker for every source parameter.
    // The function declaration itself has no binding for lambdas or methods,
    // so bindingId is intentionally optional-by-invalid-value there.
    BindingId bindingId;
    std::vector<BindingId> parameterBindingIds;
};

struct MemberCallMetadataRecord {
    std::string calleeName;
    bool passesReceiver = false;
    bool hasTarget = false;
};

enum class CallTargetKind {
    Direct,
    StructMethod,
};

enum class LoopTargetKind {
    While,
    For,
    ForIn,
};

struct CallTargetRecord {
    CallTargetKind kind = CallTargetKind::Direct;
    ResolvedSymbol target;
};

struct LoopTargetRecord {
    const Stmt* loop = nullptr;
    LoopTargetKind kind = LoopTargetKind::While;
};

enum class IndexOperationKind {
    Read,
    Assign,
    CompoundAssign,
};

struct IndexOperationRecord {
    IndexOperationKind kind = IndexOperationKind::Read;
    TypeInfo collectionType;
    TypeInfo indexType;
    TypeInfo resultType;
};

enum class FieldOperationKind {
    Read,
    Assign,
    CompoundAssign,
};

struct FieldOperationRecord {
    FieldOperationKind kind = FieldOperationKind::Read;
    std::string fieldName;
    TypeInfo fieldType;
    TypeInfo resultType;
    std::optional<std::string> resolvedName;
};

struct StructConstructorRecord {
    TypeInfo type;
    std::vector<std::string> fieldNames;
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
    TypeInfo resultType;
    std::vector<TypeInfo> payloadTypes;
};

struct LiteralPatternRecord {
    std::string literal;
    TypeInfo type;
};

struct VariantPatternRecord {
    std::string enumName;
    std::string variantName;
    TypeInfo enumType;
    std::vector<TypeInfo> payloadTypes;
};

struct RecordPatternRecord {
    TypeInfo structType;
    std::vector<std::string> fieldNames;
    std::vector<TypeInfo> fieldTypes;
};

struct PatternBindingRecord {
    std::string sourceName;
    std::string resolvedName;
    TypeInfo type;
    std::optional<SourceRange> range;
    BindingId bindingId;
    ResolvedSymbol symbol;
};

struct OrPatternRecord {
    std::vector<std::string> bindingNames;
    std::vector<TypeInfo> bindingTypes;
};

struct PatternGuardRecord {
    TypeInfo type;
};

struct MatchCoverageRecord {
    TypeInfo scrutineeType;
    std::vector<std::string> coveredVariants;
    std::vector<std::string> coveredLiterals;
    bool nullable = false;
    bool coversNil = false;
    bool coversStruct = false;
    bool coversAll = false;
    bool exhaustive = false;
};

struct ReturnRecord {
    TypeInfo type;
};

struct CaptureRecord {
    std::vector<ResolvedSymbol> symbols;
};

struct DeclarationSignature {
    std::vector<TypeParameter> typeParameters;
    std::vector<Parameter> parameters;
    std::optional<TypeAnnotation> returnType;
};

struct ResolvedSignatureRecord {
    TypeInfo type;
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
    // True when the declaration is owned by a function body (including a
    // nested function expression).  This is structural metadata used by
    // lowering; it is not inferred from the display name.
    bool functionLocal = false;
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

    const DeclarationRecord* declaration(DeclarationId id) const;
    const DeclarationRecord* declaration(const Stmt& statement) const;
    const DeclarationRecord* declaration(const MethodDecl& method) const;
    const DeclarationRecord* declaration(const Parameter& parameter) const;
    const DeclarationRecord* declaration(const VariablePattern& pattern) const;
    const BindingMetadataRecord* letBindingMetadata(const LetStmt& statement) const;
    const BindingMetadataRecord* variableBindingMetadata(const VariableExpr& expression) const;
    const BindingMetadataRecord* assignmentBindingMetadata(const AssignExpr& expression) const;
    const BindingMetadataRecord* compoundAssignmentBindingMetadata(
        const CompoundAssignExpr& expression) const;
    const BindingMetadataRecord* forInBindingMetadata(const ForInStmt& statement) const;
    const FunctionMetadataRecord* functionMetadata(const FunctionStmt& statement) const;
    const FunctionMetadataRecord* functionMetadata(const FunctionExpr& expression) const;
    const FunctionMetadataRecord* functionMetadata(const MethodDecl& method) const;
    std::vector<DeclarationId> functionParameterDeclarations(const FunctionStmt& statement) const;
    std::vector<DeclarationId> functionParameterDeclarations(const FunctionExpr& expression) const;
    std::vector<DeclarationId> functionParameterDeclarations(const MethodDecl& method) const;
    const MemberCallMetadataRecord* memberCallMetadata(const MemberCallExpr& expression) const;
    std::optional<DeclarationSignature> signature(DeclarationId id) const;
    const ResolvedSignatureRecord* resolvedSignature(DeclarationId id) const;
    std::optional<DeclarationShape> shape(DeclarationId id) const;
    const ScopeRecord* scope(ScopeId id) const;
    std::optional<ScopeId> scopeFor(const Stmt& statement) const;
    std::optional<ResolvedSymbol> forInBinding(const ForInStmt& statement) const;
    std::optional<ResolvedSymbol> patternBinding(const VariablePattern& pattern) const;
    const CallTargetRecord* callTarget(const CallExpr& expression) const;
    const CallTargetRecord* callTarget(const MemberCallExpr& expression) const;
    const CallTargetRecord* callTarget(const BinaryExpr& expression) const;
    const TypedExpressionRecord* typedExpression(const Expr& expression) const;
    const NativeCallRecord* nativeCall(const Expr& expression) const;
    const VariantConstructorRecord* variantConstructor(const MemberCallExpr& expression) const;
    const LiteralPatternRecord* literalPattern(const LiteralPattern& pattern) const;
    const VariantPatternRecord* variantPattern(const VariantPattern& pattern) const;
    const RecordPatternRecord* recordPattern(const RecordPattern& pattern) const;
    const PatternBindingRecord* patternBindingMetadata(const VariablePattern& pattern) const;
    const OrPatternRecord* orPattern(const OrPattern& pattern) const;
    const PatternGuardRecord* patternGuard(const Expr& guard) const;
    const MatchCoverageRecord* matchCoverage(const MatchStmt& match) const;
    const IndexOperationRecord* indexOperation(const Expr& expression) const;
    const FieldOperationRecord* fieldOperation(const Expr& expression) const;
    const StructConstructorRecord* structConstructor(const StructConstructExpr& expression) const;
    const ReturnRecord* returnMetadata(const ReturnStmt& statement) const;
    const CaptureRecord* captureMetadata(const FunctionStmt& statement) const;
    const CaptureRecord* captureMetadata(const FunctionExpr& expression) const;
    const CaptureRecord* captureMetadata(const MethodDecl& method) const;
    bool declarationIsCaptured(DeclarationId id) const;
    const LoopTargetRecord* breakTarget(const BreakStmt& statement) const;
    const LoopTargetRecord* continueTarget(const ContinueStmt& statement) const;

    std::optional<DeclarationId> lookup(ScopeId scopeId, const std::string& name) const;
    std::optional<ResolvedSymbol> variableReference(const VariableExpr& expression) const;
    std::optional<ResolvedSymbol> assignmentReference(const AssignExpr& expression) const;
    std::optional<ResolvedSymbol> compoundAssignmentReference(const CompoundAssignExpr& expression) const;

    // Validates that the collected and checker-produced semantic records are
    // complete and source-consistent. Checker bindings source their ids from
    // this index, so declaration/symbol ids must match exactly.
    std::size_t validateMetadata() const;

private:
    friend class DeclarationIndexCollector;
    friend class TypeChecker;

    void recordTypedExpression(const Expr& expression, TypeInfo type);
    void recordNativeCall(const Expr& expression, std::string name);
    void recordVariantConstructor(
        const MemberCallExpr& expression,
        std::string enumName,
        std::string variantName,
        TypeInfo resultType,
        std::vector<TypeInfo> payloadTypes);
    void recordLiteralPattern(const LiteralPattern& pattern, LiteralPatternRecord record);
    void recordVariantPattern(const VariantPattern& pattern, VariantPatternRecord record);
    void recordRecordPattern(const RecordPattern& pattern, RecordPatternRecord record);
    void recordPatternBinding(const VariablePattern& pattern, PatternBindingRecord record);
    void recordOrPattern(const OrPattern& pattern, OrPatternRecord record);
    void recordPatternGuard(const Expr& guard, PatternGuardRecord record);
    void recordMatchCoverage(const MatchStmt& match, MatchCoverageRecord record);
    void recordIndexOperation(const Expr& expression, IndexOperationRecord record);
    void recordFieldOperation(const Expr& expression, FieldOperationRecord record);
    void recordStructConstructor(const StructConstructExpr& expression, StructConstructorRecord record);
    void recordLetBinding(const LetStmt& statement, BindingMetadataRecord record);
    void recordVariableBinding(const VariableExpr& expression, BindingMetadataRecord record);
    void recordAssignmentBinding(const AssignExpr& expression, BindingMetadataRecord record);
    void recordCompoundAssignmentBinding(
        const CompoundAssignExpr& expression,
        BindingMetadataRecord record);
    void recordForInBinding(const ForInStmt& statement, BindingMetadataRecord record);
    void recordFunctionMetadata(const FunctionStmt& statement, FunctionMetadataRecord record);
    void recordFunctionMetadata(const FunctionExpr& expression, FunctionMetadataRecord record);
    void recordFunctionMetadata(const MethodDecl& method, FunctionMetadataRecord record);
    void recordMemberCallMetadata(
        const MemberCallExpr& expression,
        MemberCallMetadataRecord record);
    void recordMemberCallTarget(
        const MemberCallExpr& expression,
        CallTargetRecord record);
    void recordReturn(const ReturnStmt& statement, TypeInfo type);
    void recordResolvedSignature(DeclarationId id, TypeInfo type);

    std::vector<DeclarationRecord> declarations_;
    std::vector<ScopeRecord> scopes_;
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
    std::unordered_map<const LetStmt*, BindingMetadataRecord> letBindingMetadata_;
    std::unordered_map<const VariableExpr*, BindingMetadataRecord> variableBindingMetadata_;
    std::unordered_map<const AssignExpr*, BindingMetadataRecord> assignmentBindingMetadata_;
    std::unordered_map<const CompoundAssignExpr*, BindingMetadataRecord>
        compoundAssignmentBindingMetadata_;
    std::unordered_map<const ForInStmt*, BindingMetadataRecord> forInBindingMetadata_;
    std::unordered_map<const FunctionStmt*, FunctionMetadataRecord> functionMetadata_;
    std::unordered_map<const FunctionExpr*, FunctionMetadataRecord> functionExpressionMetadata_;
    std::unordered_map<const MethodDecl*, FunctionMetadataRecord> methodMetadata_;
    std::unordered_map<const FunctionStmt*, std::vector<DeclarationId>> functionParameterDeclarations_;
    std::unordered_map<const FunctionExpr*, std::vector<DeclarationId>> functionExpressionParameterDeclarations_;
    std::unordered_map<const MethodDecl*, std::vector<DeclarationId>> methodParameterDeclarations_;
    std::unordered_map<const MemberCallExpr*, MemberCallMetadataRecord> memberCallMetadata_;
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
    std::unordered_map<const LiteralPattern*, LiteralPatternRecord> literalPatterns_;
    std::unordered_map<const VariantPattern*, VariantPatternRecord> variantPatterns_;
    std::unordered_map<const RecordPattern*, RecordPatternRecord> recordPatterns_;
    std::unordered_map<const VariablePattern*, PatternBindingRecord> patternBindingMetadata_;
    std::unordered_map<const OrPattern*, OrPatternRecord> orPatterns_;
    std::unordered_map<const Expr*, PatternGuardRecord> patternGuards_;
    std::unordered_map<const MatchStmt*, MatchCoverageRecord> matchStatementCoverage_;
    std::unordered_map<const Expr*, IndexOperationRecord> indexOperations_;
    std::unordered_map<const Expr*, FieldOperationRecord> fieldOperations_;
    std::unordered_map<const StructConstructExpr*, StructConstructorRecord> structConstructorsMetadata_;
    std::unordered_set<const FunctionExpr*> functionExpressions_;
    std::unordered_set<const ReturnStmt*> returnStatements_;
    std::unordered_map<const ReturnStmt*, ReturnRecord> returnMetadata_;
    std::unordered_map<const FunctionStmt*, CaptureRecord> functionCaptures_;
    std::unordered_map<const FunctionExpr*, CaptureRecord> functionExpressionCaptures_;
    std::unordered_map<const MethodDecl*, CaptureRecord> methodCaptures_;
    std::unordered_set<const BreakStmt*> breakStatements_;
    std::unordered_set<const ContinueStmt*> continueStatements_;
    std::unordered_set<const LiteralPattern*> literalPatternNodes_;
    std::unordered_set<const VariantPattern*> variantPatternNodes_;
    std::unordered_set<const RecordPattern*> recordPatternNodes_;
    std::unordered_set<const OrPattern*> orPatternNodes_;
    std::unordered_set<const Expr*> patternGuardNodes_;
    std::unordered_set<const MatchStmt*> matchStatementNodes_;
    std::unordered_map<const BreakStmt*, LoopTargetRecord> breakTargets_;
    std::unordered_map<const ContinueStmt*, LoopTargetRecord> continueTargets_;
    std::unordered_map<
        DeclarationId,
        ResolvedSignatureRecord,
        SnapshotIdHash<DeclarationIdTag>> resolvedSignatures_;
};
