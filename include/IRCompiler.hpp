#pragma once

#include "Ast.hpp"
#include "Diagnostic.hpp"
#include "IR.hpp"
#include "TypeChecker.hpp"

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
        const ResolvedNames& resolvedNames,
        const DeclarationIndex& declarationIndex);

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
    void compileReturn(const ReturnStmt& statement);
    void requireFunctionMetadata(const FunctionStmt& function) const;
    void requireMethodMetadata(const MethodDecl& method) const;
    IRRegister compileExpression(const Expr& expression);
    IRRegister emitCall(const CallExpr& expression);
    IRRegister emitMemberCall(const MemberCallExpr& expression);
    bool isBuiltinLenCall(const CallExpr& expression) const;
    bool hasNativeCallMetadata(const CallExpr& expression) const;
    void compileBreak(const BreakStmt& statement);
    void compileContinue(const ContinueStmt& statement);
    void compileFor(const ForStmt& statement);
    void compileForIn(const ForInStmt& statement);
    void compileMatch(const MatchStmt& statement);
    IRRegister compileMatchExpression(const MatchExpr& expression);
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
    IRRegister emitBinary(TokenType op, IRRegister left, IRRegister right);
    IRRegister emitLogical(const LogicalExpr& expression);

    struct LoopContext {
        const Stmt* statement = nullptr;
        std::size_t continueTarget = 0;
        std::vector<std::size_t> breakJumps;
    };

    IRProgram ir_;
    const ResolvedNames* resolvedNames_ = nullptr;
    const DeclarationIndex* declarationIndex_ = nullptr;
    std::unordered_map<std::size_t, const ModuleStmt*> modules_;
    std::unordered_set<std::size_t> compiledModules_;
    std::vector<LoopContext> loopContexts_;
    std::size_t nextSyntheticName_ = 0;
    std::optional<SourceSpan> currentSpan_;
};
