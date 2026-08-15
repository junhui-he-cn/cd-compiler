#pragma once

#include "Ast.hpp"
#include "DeclarationIndex.hpp"
#include "Diagnostic.hpp"
#include "IR.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class IRCompileError final : public DiagnosticError {
public:
    explicit IRCompileError(std::string message);
};

class IRCompiler {
public:
    IRProgram compile(
        const Program& program,
        const DeclarationIndex& declarationIndex);
    IRProgram compileModule(
        const Program& program,
        std::size_t moduleId,
        const DeclarationIndex& declarationIndex);
    std::optional<std::size_t> moduleInitFunction() const;

private:
    class SpanScope {
    public:
        SpanScope(IRCompiler& owner, const std::optional<SourceSpan>& span);
        ~SpanScope();

    private:
        IRCompiler& owner_;
        std::optional<SourceSpan> previous_;
    };

    void setCurrentSpan(std::optional<SourceSpan> span);
    std::optional<SourceSpan> debugSpan(
        const std::optional<SourceRange>& range,
        const std::optional<SourceSpan>& fallback) const;
    void collectExportedDeclarations(const Program& program);
    BindingStorageClass storageClassFor(std::optional<DeclarationId> declarationId) const;
    std::optional<BindingId> registerBinding(
        BindingId bindingId,
        const std::string& resolvedName,
        std::optional<DeclarationId> declarationId,
        std::optional<BindingStorageClass> explicitStorage = std::nullopt);
    std::optional<BindingId> registerBindingMetadata(
        const BindingMetadataRecord& metadata,
        std::optional<DeclarationId> declarationId);
    std::optional<BindingId> registerSyntheticBinding(const std::string& resolvedName);
    std::optional<BindingId> registerSyntheticBinding(
        const std::string& resolvedName,
        BindingStorageClass storage);
    void registerFunctionParameters(
        const FunctionMetadataRecord& metadata,
        const std::vector<DeclarationId>& declarations);
    IRProgram compileInternal(
        const Program& program,
        const DeclarationIndex& declarationIndex,
        std::optional<std::size_t> moduleId);
    const TypeInfo& typedExpressionType(
        const Expr& expression,
        const char* context) const;
    const TypeInfo& typedExpressionType(
        const Expr& expression,
        StaticType expectedKind,
        const char* context) const;
    const IndexOperationRecord& indexOperation(
        const Expr& expression,
        IndexOperationKind kind,
        const char* context) const;
    const FieldOperationRecord& fieldOperation(
        const Expr& expression,
        FieldOperationKind kind,
        const char* context) const;

    void compileStatement(const Stmt& statement);
    void compileModule(const ModuleStmt& module);
    void compileFunctionStatement(const FunctionStmt& function);
    void compileImpl(const ImplStmt& statement);
    void compileMethod(const MethodDecl& method);
    void compileIfLet(const IfLetStmt& statement);
    void compileWhileLet(const WhileLetStmt& statement);
    void compileReturn(const ReturnStmt& statement);
    void requireFunctionMetadata(const FunctionStmt& function) const;
    void requireMethodMetadata(const MethodDecl& method) const;
    IRRegister compileExpression(const Expr& expression);
    IRRegister compileCoalesce(const CoalesceExpr& expression);
    IRRegister compileUnwrapOrReturn(const UnwrapOrReturnExpr& expression);
    IRRegister emitCall(const CallExpr& expression);
    IRRegister emitMemberCall(const MemberCallExpr& expression);
    bool isBuiltinLenCall(const CallExpr& expression) const;
    bool hasNativeCallMetadata(const CallExpr& expression) const;
    void compileBreak(const BreakStmt& statement);
    void compileContinue(const ContinueStmt& statement);
    void compileFor(const ForStmt& statement);
    void compileForIn(const ForInStmt& statement);
    void compileMatch(const MatchStmt& statement);
    std::string makeSyntheticName(const std::string& prefix);
    IRRegister emitLenCall(const CallExpr& expression);
    IRRegister emitNativeStdlibCall(const CallExpr& expression);
    IRRegister emitFunctionExpr(const FunctionExpr& expression);
    IRRegister emitArray(const ArrayExpr& expression);
    IRRegister emitMap(const MapExpr& expression);
    IRRegister emitStructFields(
        const std::vector<StructField>& fields,
        const std::vector<std::string>& fieldNames,
        std::optional<std::string> typeName = std::nullopt);
    IRRegister emitStructConstructor(const StructConstructExpr& expression);
    IRRegister emitVariantConstructor(const MemberCallExpr& expression);
    struct CompiledPatternBinding {
        std::string sourceName;
        std::string resolvedName;
        IRRegister value;
        std::optional<BindingId> bindingId;
    };
    void compilePattern(
        const Pattern& pattern,
        IRRegister value,
        std::vector<std::size_t>& failJumps,
        std::vector<CompiledPatternBinding>& bindings);
    IRRegister emitIndex(const IndexExpr& expression);
    IRRegister emitCompoundAssign(const CompoundAssignExpr& expression);
    IRRegister emitCompoundAssignmentResult(
        const Token& op,
        IRRegister oldValue,
        const Expr& value,
        const std::string& targetMessage);
    IROp compoundAssignmentOp(TokenType op) const;
    IRRegister emitIndexAssign(const IndexAssignExpr& expression);
    IRRegister emitIndexCompoundAssign(const IndexCompoundAssignExpr& expression);
    IRRegister emitFieldAccess(const FieldAccessExpr& expression);
    IRRegister emitFieldAssign(const FieldAssignExpr& expression);
    IRRegister emitFieldCompoundAssign(const FieldCompoundAssignExpr& expression);
    IRRegister emitUnary(TokenType op, IRRegister value);
    IRRegister emitBinary(
        TokenType op,
        IRRegister left,
        IRRegister right,
        const TypeInfo* operandType = nullptr,
        const TypeInfo* resultType = nullptr);
    const TypedExpressionRecord* typedExpressionRecord(const Expr& expression) const;
    IRRegister emitLenTyped(IRRegister value, const Expr& operand);
    IRRegister emitLogical(const LogicalExpr& expression);
    void patchPendingDirectCalls();

    struct LoopContext {
        const Stmt* statement = nullptr;
        std::size_t continueTarget = 0;
        std::vector<std::size_t> breakJumps;
    };

    struct PendingDirectCall {
        std::optional<std::size_t> functionId;
        std::size_t instructionIndex = 0;
        DeclarationId target;
    };

    IRProgram ir_;
    const DeclarationIndex* declarationIndex_ = nullptr;
    std::unordered_map<std::size_t, const ModuleStmt*> modules_;
    std::unordered_set<std::size_t> compiledModules_;
    std::optional<std::size_t> independentModuleId_;
    std::vector<LoopContext> loopContexts_;
    std::unordered_map<DeclarationId, std::size_t, SnapshotIdHash<DeclarationIdTag>> functionIndices_;
    std::vector<PendingDirectCall> pendingDirectCalls_;
    std::optional<std::size_t> moduleInitFunction_;
    std::size_t nextSyntheticName_ = 0;
    std::size_t nextSyntheticBindingId_ = 0;
    std::size_t activeFunctionDepth_ = 0;
    std::unordered_set<DeclarationId, SnapshotIdHash<DeclarationIdTag>> exportedDeclarations_;
    std::unordered_map<BindingId, IRBinding, SnapshotIdHash<BindingIdTag>> registeredBindings_;
    std::unordered_map<std::string, BindingId> bindingIdsByResolvedName_;
    std::optional<SourceSpan> currentSpan_;
};
