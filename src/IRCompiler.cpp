#include "IRCompiler.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

Value literalValue(const std::string& text)
{
    if (text == "nil") {
        return Value::nil();
    }
    if (text == "true") {
        return Value::boolean(true);
    }
    if (text == "false") {
        return Value::boolean(false);
    }
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return Value::string(text.substr(1, text.size() - 2));
    }

    std::size_t parsed = 0;
    const double number = std::stod(text, &parsed);
    if (parsed != text.size()) {
        throw IRCompileError("invalid literal: " + text);
    }
    return Value::number(number);
}

std::string importPathForIR(const Token& token)
{
    if (token.lexeme.size() >= 2 && token.lexeme.front() == '"' && token.lexeme.back() == '"') {
        return token.lexeme.substr(1, token.lexeme.size() - 2);
    }
    return token.lexeme;
}

} // namespace

IRCompileError::IRCompileError(std::string message)
    : DiagnosticError(DiagnosticKind::Compile, std::move(message))
{
}

IRCompiler::SpanScope::SpanScope(IRCompiler& owner, const std::optional<SourceSpan>& span)
    : owner_(owner)
    , previous_(owner.currentSpan_)
{
    owner_.setCurrentSpan(span);
}

IRCompiler::SpanScope::~SpanScope()
{
    owner_.setCurrentSpan(previous_);
}

void IRCompiler::setCurrentSpan(std::optional<SourceSpan> span)
{
    currentSpan_ = std::move(span);
    ir_.setCurrentSpan(currentSpan_);
}

std::optional<SourceSpan> IRCompiler::debugSpan(
    const std::optional<SourceRange>& range,
    const std::optional<SourceSpan>& fallback) const
{
    if (range && isValidSourceRange(*range, ir_.sources())) {
        const SourcePosition start = sourcePositionAt(ir_.sources(), *range);
        SourceSpan result = fallback.value_or(SourceSpan{
            range->source.value,
            start.line,
            start.column,
        });
        result.source = range->source.value;
        result.range = SourceSpanRange{range->start, range->end};
        return result;
    }
    return fallback;
}

void IRCompiler::collectExportedDeclarations(const Program& program)
{
    exportedDeclarations_.clear();
    if (!declarationIndex_) {
        return;
    }

    const auto markExport = [this](const ExportStmt& statement, ScopeId scopeId) {
        if (statement.sourcePath) {
            return;
        }
        for (const Token& name : statement.names) {
            if (const std::optional<DeclarationId> declaration
                = declarationIndex_->lookup(scopeId, name.lexeme)) {
                exportedDeclarations_.insert(*declaration);
            }
        }
    };

    bool hasModules = false;
    for (const auto& statement : program.statements) {
        if (dynamic_cast<const ModuleStmt*>(statement.get())) {
            hasModules = true;
            break;
        }
    }

    if (hasModules) {
        for (const auto& statement : program.statements) {
            const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
            if (!module) {
                continue;
            }
            const DeclarationRecord* moduleDeclaration = declarationIndex_->declaration(*module);
            if (!moduleDeclaration) {
                continue;
            }
            for (const auto& child : module->statements) {
                if (const auto* exportStatement = dynamic_cast<const ExportStmt*>(child.get())) {
                    markExport(*exportStatement, moduleDeclaration->scopeId);
                }
            }
        }
        return;
    }

    std::optional<ScopeId> rootScope;
    for (const ScopeRecord& scope : declarationIndex_->scopes()) {
        if (!scope.parent) {
            rootScope = scope.id;
            break;
        }
    }
    if (!rootScope) {
        return;
    }
    for (const auto& statement : program.statements) {
        if (const auto* exportStatement = dynamic_cast<const ExportStmt*>(statement.get())) {
            markExport(*exportStatement, *rootScope);
        }
    }
}

BindingStorageClass IRCompiler::storageClassFor(
    std::optional<DeclarationId> declarationId) const
{
    if (!declarationIndex_ || !declarationId) {
        return BindingStorageClass::Unknown;
    }
    const DeclarationRecord* declaration = declarationIndex_->declaration(*declarationId);
    if (!declaration) {
        return BindingStorageClass::Unknown;
    }
    if (declarationIndex_->declarationIsCaptured(*declarationId)) {
        return BindingStorageClass::Captured;
    }
    if (exportedDeclarations_.find(*declarationId) != exportedDeclarations_.end()) {
        return BindingStorageClass::Exported;
    }
    return declaration->functionLocal
        ? BindingStorageClass::Local
        : BindingStorageClass::Module;
}

std::optional<BindingId> IRCompiler::registerBinding(
    BindingId bindingId,
    const std::string& resolvedName,
    std::optional<DeclarationId> declarationId,
    std::optional<BindingStorageClass> explicitStorage)
{
    if (!bindingId.valid()) {
        return std::nullopt;
    }
    if (resolvedName.empty()) {
        throw IRCompileError("binding metadata has an empty resolved name");
    }

    const IRBinding candidate{
        bindingId,
        resolvedName,
        explicitStorage ? *explicitStorage : storageClassFor(declarationId)};
    const auto existing = registeredBindings_.find(bindingId);
    if (existing == registeredBindings_.end()) {
        ir_.addBinding(candidate);
        registeredBindings_.emplace(bindingId, candidate);
        bindingIdsByResolvedName_.emplace(resolvedName, bindingId);
    } else if (existing->second.resolvedName != candidate.resolvedName
        || existing->second.storage != candidate.storage) {
        throw IRCompileError("conflicting binding storage metadata for `" + resolvedName + "`");
    }

    if (activeFunctionDepth_ != 0) {
        ir_.addFunctionBinding(candidate);
    }
    return bindingId;
}

std::optional<BindingId> IRCompiler::registerBindingMetadata(
    const BindingMetadataRecord& metadata,
    std::optional<DeclarationId> declarationId)
{
    return registerBinding(metadata.bindingId, metadata.resolvedName, declarationId);
}

std::optional<BindingId> IRCompiler::registerSyntheticBinding(const std::string& resolvedName)
{
    while (nextSyntheticBindingId_ < std::numeric_limits<std::size_t>::max() - 1) {
        const BindingId bindingId{
            std::numeric_limits<std::size_t>::max() - 1 - nextSyntheticBindingId_++};
        if (registeredBindings_.find(bindingId) == registeredBindings_.end()) {
            return registerBinding(
                bindingId,
                resolvedName,
                std::nullopt,
                BindingStorageClass::Synthetic);
        }
    }
    throw IRCompileError("exhausted synthetic binding IDs");
}

void IRCompiler::registerFunctionParameters(
    const FunctionMetadataRecord& metadata,
    const std::vector<DeclarationId>& declarations)
{
    if (metadata.parameterNames.size() != metadata.parameterBindingIds.size()
        || metadata.parameterNames.size() != declarations.size()) {
        throw IRCompileError("function parameter binding metadata mismatch");
    }
    ir_.setFunctionParameterBindingIds(metadata.parameterBindingIds);
    for (std::size_t index = 0; index < metadata.parameterNames.size(); ++index) {
        if (!metadata.parameterBindingIds[index].valid()) {
            throw IRCompileError("function parameter binding metadata is missing");
        }
        registerBinding(
            metadata.parameterBindingIds[index],
            metadata.parameterNames[index],
            declarations[index]);
    }
}

const TypeInfo& IRCompiler::typedExpressionType(
    const Expr& expression,
    const char* context) const
{
    const TypedExpressionRecord* record = declarationIndex_
        ? declarationIndex_->typedExpression(expression)
        : nullptr;
    if (!record) {
        throw IRCompileError(std::string("missing typed metadata for ") + context);
    }
    return record->type;
}

const TypeInfo& IRCompiler::typedExpressionType(
    const Expr& expression,
    StaticType expectedKind,
    const char* context) const
{
    const TypeInfo& type = typedExpressionType(expression, context);
    if (type.kind != expectedKind) {
        throw IRCompileError(std::string("missing typed metadata for ") + context);
    }
    return type;
}

const TypedExpressionRecord* IRCompiler::typedExpressionRecord(const Expr& expression) const
{
    const Expr* current = &expression;
    while (current) {
        if (const auto* grouping = dynamic_cast<const GroupingExpr*>(current)) {
            current = grouping->expression.get();
            continue;
        }
        break;
    }
    return declarationIndex_ ? declarationIndex_->typedExpression(*current) : nullptr;
}

const IndexOperationRecord& IRCompiler::indexOperation(
    const Expr& expression,
    IndexOperationKind kind,
    const char* context) const
{
    const IndexOperationRecord* record = declarationIndex_
        ? declarationIndex_->indexOperation(expression)
        : nullptr;
    if (!record || record->kind != kind) {
        throw IRCompileError(std::string("missing aggregate index metadata for ") + context);
    }
    return *record;
}

const FieldOperationRecord& IRCompiler::fieldOperation(
    const Expr& expression,
    FieldOperationKind kind,
    const char* context) const
{
    const FieldOperationRecord* record = declarationIndex_
        ? declarationIndex_->fieldOperation(expression)
        : nullptr;
    if (!record || record->kind != kind) {
        throw IRCompileError(std::string("missing aggregate field metadata for ") + context);
    }
    return *record;
}

IRProgram IRCompiler::compile(
    const Program& program,
    const DeclarationIndex& declarationIndex)
{
    return compileInternal(program, declarationIndex, std::nullopt);
}

IRProgram IRCompiler::compileModule(
    const Program& program,
    std::size_t moduleId,
    const DeclarationIndex& declarationIndex)
{
    return compileInternal(program, declarationIndex, moduleId);
}

IRProgram IRCompiler::compileInternal(
    const Program& program,
    const DeclarationIndex& declarationIndex,
    std::optional<std::size_t> moduleId)
{
    ir_ = IRProgram();
    ir_.setSources(program.sources);
    currentSpan_ = std::nullopt;
    ir_.setCurrentSpan(std::nullopt);
    declarationIndex_ = &declarationIndex;
    independentModuleId_ = moduleId;
    modules_.clear();
    compiledModules_.clear();
    loopContexts_.clear();
    functionIndices_.clear();
    pendingDirectCalls_.clear();
    registeredBindings_.clear();
    bindingIdsByResolvedName_.clear();
    exportedDeclarations_.clear();
    nextSyntheticName_ = 0;
    nextSyntheticBindingId_ = 0;
    activeFunctionDepth_ = 0;
    collectExportedDeclarations(program);
    for (const auto& statement : program.statements) {
        if (const auto* module = dynamic_cast<const ModuleStmt*>(statement.get())) {
            modules_.emplace(module->moduleId, module);
        }
    }
    const auto recordLayouts = [this](const Stmt& statement) {
        if (const auto* structDecl = dynamic_cast<const StructDeclStmt*>(&statement)) {
            IRStructLayout layout;
            layout.name = structDecl->name.lexeme;
            for (const StructFieldDecl& field : structDecl->fields) {
                layout.fieldNames.push_back(field.name.lexeme);
            }
            ir_.addStructLayout(std::move(layout));
        } else if (const auto* enumDecl = dynamic_cast<const EnumDeclStmt*>(&statement)) {
            IREnumLayout layout;
            layout.name = enumDecl->name.lexeme;
            for (const EnumVariantDecl& variant : enumDecl->variants) {
                layout.variants.push_back(IRVariantLayout{
                    variant.name.lexeme,
                    variant.payloadTypes.size(),
                });
            }
            ir_.addEnumLayout(std::move(layout));
        }
    };
    for (const auto& [moduleId, module] : modules_) {
        (void)moduleId;
        for (const StmtPtr& statement : module->statements) {
            recordLayouts(*statement);
        }
    }
    for (const auto& statement : program.statements) {
        if (!dynamic_cast<const ModuleStmt*>(statement.get())) {
            recordLayouts(*statement);
        }
    }
    if (independentModuleId_) {
        const auto found = modules_.find(*independentModuleId_);
        if (found == modules_.end()) {
            throw IRCompileError("internal error: unresolved module for independent lowering");
        }
        compileModule(*found->second);
    } else {
        for (const auto& statement : program.statements) {
            compileStatement(*statement);
        }
    }
    patchPendingDirectCalls();
    modules_.clear();
    compiledModules_.clear();
    loopContexts_.clear();
    registeredBindings_.clear();
    bindingIdsByResolvedName_.clear();
    exportedDeclarations_.clear();
    activeFunctionDepth_ = 0;
    independentModuleId_.reset();
    declarationIndex_ = nullptr;
    return std::move(ir_);
}

void IRCompiler::patchPendingDirectCalls()
{
    for (const PendingDirectCall& pending : pendingDirectCalls_) {
        const auto found = functionIndices_.find(pending.target);
        if (found == functionIndices_.end()) {
            throw IRCompileError("direct call target has no compiled function");
        }
        if (pending.functionIndex) {
            ir_.patchFunctionCallDirect(
                *pending.functionIndex, pending.instructionIndex, found->second);
        } else {
            ir_.patchMainCallDirect(pending.instructionIndex, found->second);
        }
    }
    pendingDirectCalls_.clear();
}

void IRCompiler::compileStatement(const Stmt& statement)
{
    SpanScope scope(*this, debugSpan(statement.range, statement.span));
    if (const auto* module = dynamic_cast<const ModuleStmt*>(&statement)) {
        if (module->isEntry) {
            compileModule(*module);
        }
        return;
    }

    if (const auto* import = dynamic_cast<const ImportStmt*>(&statement)) {
        if (independentModuleId_) {
            ir_.addModuleDependency(IRModuleDependency{
                import->resolvedModuleId,
                ModuleGraphEdgeKind::Import,
                importPathForIR(import->path),
                ir_.instructionCount()});
            return;
        }
        const auto found = modules_.find(import->resolvedModuleId);
        if (found == modules_.end()) {
            throw IRCompileError("internal error: unresolved import module");
        }
        compileModule(*found->second);
        return;
    }

    if (const auto* exportStmt = dynamic_cast<const ExportStmt*>(&statement)) {
        if (exportStmt->sourcePath) {
            if (independentModuleId_) {
                ir_.addModuleDependency(IRModuleDependency{
                    exportStmt->resolvedModuleId,
                    ModuleGraphEdgeKind::ReExport,
                    importPathForIR(*exportStmt->sourcePath),
                    ir_.instructionCount()});
                return;
            }
            const auto found = modules_.find(exportStmt->resolvedModuleId);
            if (found == modules_.end()) {
                throw IRCompileError("internal error: unresolved re-export module");
            }
            compileModule(*found->second);
        }
        return;
    }

    if (dynamic_cast<const StructDeclStmt*>(&statement)) {
        return;
    }

    if (dynamic_cast<const EnumDeclStmt*>(&statement)) {
        return;
    }

    if (const auto* impl = dynamic_cast<const ImplStmt*>(&statement)) {
        compileImpl(*impl);
        return;
    }

    if (const auto* function = dynamic_cast<const FunctionStmt*>(&statement)) {
        compileFunctionStatement(*function);
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&statement)) {
        compileReturn(*returnStmt);
        return;
    }

    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&statement)) {
        compileBreak(*breakStmt);
        return;
    }

    if (const auto* continueStmt = dynamic_cast<const ContinueStmt*>(&statement)) {
        compileContinue(*continueStmt);
        return;
    }

    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
        for (const auto& child : block->statements) {
            compileStatement(*child);
        }
        return;
    }

    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
        const IRRegister condition = compileExpression(*ifStmt->condition);
        const std::size_t jumpIfFalse = ir_.emitJumpIfFalse(condition);

        compileStatement(*ifStmt->thenBranch);

        if (ifStmt->elseBranch) {
            const std::size_t jumpOverElse = ir_.emitJump();
            ir_.patchJump(jumpIfFalse);
            compileStatement(*ifStmt->elseBranch);
            ir_.patchJump(jumpOverElse);
        } else {
            ir_.patchJump(jumpIfFalse);
        }
        return;
    }

    if (const auto* ifLetStmt = dynamic_cast<const IfLetStmt*>(&statement)) {
        compileIfLet(*ifLetStmt);
        return;
    }

    if (const auto* match = dynamic_cast<const MatchStmt*>(&statement)) {
        compileMatch(*match);
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
        const std::size_t loopStart = ir_.instructionCount();
        const IRRegister condition = compileExpression(*whileStmt->condition);
        const std::size_t exitJump = ir_.emitJumpIfFalse(condition);

        loopContexts_.push_back(LoopContext{whileStmt, loopStart, {}});
        compileStatement(*whileStmt->body);
        LoopContext loop = std::move(loopContexts_.back());
        loopContexts_.pop_back();

        ir_.emitJumpTo(loopStart);
        ir_.patchJump(exitJump);
        for (const std::size_t breakJump : loop.breakJumps) {
            ir_.patchJump(breakJump);
        }
        return;
    }

    if (const auto* whileLetStmt = dynamic_cast<const WhileLetStmt*>(&statement)) {
        compileWhileLet(*whileLetStmt);
        return;
    }

    if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
        compileFor(*forStmt);
        return;
    }

    if (const auto* forInStmt = dynamic_cast<const ForInStmt*>(&statement)) {
        compileForIn(*forInStmt);
        return;
    }

    if (const auto* let = dynamic_cast<const LetStmt*>(&statement)) {
        const IRRegister value = compileExpression(*let->initializer);
        const BindingMetadataRecord* binding = declarationIndex_
            ? declarationIndex_->letBindingMetadata(*let)
            : nullptr;
        if (!binding) {
            throw IRCompileError("missing binding metadata for let declaration");
        }
        const DeclarationRecord* declaration = declarationIndex_->declaration(*let);
        const std::optional<BindingId> bindingId = registerBindingMetadata(
            *binding,
            declaration ? std::optional<DeclarationId>(declaration->declarationId) : std::nullopt);
        ir_.emitStoreVar(binding->resolvedName, value, bindingId);
        return;
    }

    if (const auto* print = dynamic_cast<const PrintStmt*>(&statement)) {
        const IRRegister value = compileExpression(*print->expression);
        ir_.emitPrint(value);
        return;
    }

    if (const auto* expression = dynamic_cast<const ExpressionStmt*>(&statement)) {
        compileExpression(*expression->expression);
        return;
    }

    throw IRCompileError("unsupported statement node");
}

void IRCompiler::compileModule(const ModuleStmt& module)
{
    if (!module.bodySourceBacked) {
        throw IRCompileError(
            "preloaded module body cannot be lowered; emit module products and link them");
    }
    if (compiledModules_.find(module.moduleId) != compiledModules_.end()) {
        return;
    }
    compiledModules_.insert(module.moduleId);
    for (const auto& child : module.statements) {
        compileStatement(*child);
    }
}

void IRCompiler::compileFunctionStatement(const FunctionStmt& function)
{
    requireFunctionMetadata(function);
    const FunctionMetadataRecord& metadata = *declarationIndex_->functionMetadata(function);
    const std::string& functionName = metadata.resolvedName;
    const DeclarationRecord* declaration = declarationIndex_->declaration(function);
    const std::optional<BindingId> functionBinding = registerBinding(
        metadata.bindingId,
        functionName,
        declaration ? std::optional<DeclarationId>(declaration->declarationId) : std::nullopt);
    IRRegister placeholder = ir_.emitConstant(Value::nil());
    ir_.emitStoreVar(functionName, placeholder, functionBinding);

    std::vector<std::string> parameters = metadata.parameterNames;
    ir_.beginFunction(metadata.functionLabel, std::move(parameters));
    ++activeFunctionDepth_;
    registerFunctionParameters(
        metadata,
        declarationIndex_->functionParameterDeclarations(function));

    std::vector<LoopContext> enclosingLoopContexts = std::move(loopContexts_);
    loopContexts_.clear();
    for (const auto& statement : function.body) {
        compileStatement(*statement);
    }
    IRRegister nilValue = ir_.emitConstant(Value::nil());
    ir_.emitReturn(nilValue);
    loopContexts_ = std::move(enclosingLoopContexts);
    --activeFunctionDepth_;

    const std::size_t functionIndex = ir_.endFunction();
    functionIndices_.emplace(declaration->declarationId, functionIndex);
    IRRegister value = ir_.emitMakeFunction(functionIndex);
    ir_.emitAssignVar(functionName, value, functionBinding);
}

void IRCompiler::compileImpl(const ImplStmt& statement)
{
    for (const MethodDecl& method : statement.methods) {
        compileMethod(method);
    }
}

void IRCompiler::compileMethod(const MethodDecl& method)
{
    requireMethodMetadata(method);
    const FunctionMetadataRecord& metadata = *declarationIndex_->functionMetadata(method);
    const std::string& methodName = metadata.resolvedName;
    const std::optional<BindingId> methodBinding = registerSyntheticBinding(methodName);
    IRRegister placeholder = ir_.emitConstant(Value::nil());
    ir_.emitStoreVar(methodName, placeholder, methodBinding);

    std::vector<std::string> parameters = metadata.parameterNames;
    ir_.beginFunction(metadata.functionLabel, std::move(parameters));
    ++activeFunctionDepth_;
    registerFunctionParameters(
        metadata,
        declarationIndex_->functionParameterDeclarations(method));

    std::vector<LoopContext> enclosingLoopContexts = std::move(loopContexts_);
    loopContexts_.clear();
    for (const auto& statement : method.body) {
        compileStatement(*statement);
    }
    IRRegister nilValue = ir_.emitConstant(Value::nil());
    ir_.emitReturn(nilValue);
    loopContexts_ = std::move(enclosingLoopContexts);
    --activeFunctionDepth_;

    const std::size_t functionIndex = ir_.endFunction();
    const DeclarationRecord* declaration = declarationIndex_->declaration(method);
    if (declaration) {
        functionIndices_.emplace(declaration->declarationId, functionIndex);
    }
    IRRegister value = ir_.emitMakeFunction(functionIndex);
    ir_.emitAssignVar(methodName, value, methodBinding);
}

void IRCompiler::compileReturn(const ReturnStmt& statement)
{
    if (!declarationIndex_ || !declarationIndex_->returnMetadata(statement)) {
        throw IRCompileError("missing return metadata");
    }
    IRRegister value = statement.value ? compileExpression(*statement.value) : ir_.emitConstant(Value::nil());
    ir_.emitReturn(value);
}

void IRCompiler::requireFunctionMetadata(const FunctionStmt& function) const
{
    const DeclarationRecord* declaration = declarationIndex_
        ? declarationIndex_->declaration(function)
        : nullptr;
    const FunctionMetadataRecord* metadata = declarationIndex_
        ? declarationIndex_->functionMetadata(function)
        : nullptr;
    if (!declaration
        || declaration->kind != DeclarationKind::Function
        || !declarationIndex_->resolvedSignature(declaration->declarationId)
        || !metadata
        || !metadata->bindingId.valid()
        || metadata->parameterBindingIds.size() != function.parameters.size()
        || !declarationIndex_->captureMetadata(function)) {
        throw IRCompileError("missing function metadata");
    }
}

void IRCompiler::requireMethodMetadata(const MethodDecl& method) const
{
    const DeclarationRecord* declaration = declarationIndex_
        ? declarationIndex_->declaration(method)
        : nullptr;
    const FunctionMetadataRecord* metadata = declarationIndex_
        ? declarationIndex_->functionMetadata(method)
        : nullptr;
    if (!declaration
        || declaration->kind != DeclarationKind::Method
        || !declarationIndex_->resolvedSignature(declaration->declarationId)
        || !metadata
        || metadata->parameterBindingIds.size() != method.parameters.size() + 1
        || !declarationIndex_->captureMetadata(method)) {
        throw IRCompileError("missing method metadata");
    }
}

void IRCompiler::compileBreak(const BreakStmt& statement)
{
    const LoopTargetRecord* target = declarationIndex_
        ? declarationIndex_->breakTarget(statement)
        : nullptr;
    if (!target) {
        throw IRCompileError("missing break target metadata");
    }
    if (loopContexts_.empty() || loopContexts_.back().statement != target->loop) {
        throw IRCompileError("break target metadata does not match active loop");
    }
    loopContexts_.back().breakJumps.push_back(ir_.emitJump());
}

void IRCompiler::compileContinue(const ContinueStmt& statement)
{
    const LoopTargetRecord* target = declarationIndex_
        ? declarationIndex_->continueTarget(statement)
        : nullptr;
    if (!target) {
        throw IRCompileError("missing continue target metadata");
    }
    if (loopContexts_.empty() || loopContexts_.back().statement != target->loop) {
        throw IRCompileError("continue target metadata does not match active loop");
    }
    ir_.emitJumpTo(loopContexts_.back().continueTarget);
}

void IRCompiler::compileFor(const ForStmt& statement)
{
    if (statement.initializer) {
        compileStatement(*statement.initializer);
    }

    const std::size_t loopStart = ir_.instructionCount();
    std::size_t exitJump = static_cast<std::size_t>(-1);
    if (statement.condition) {
        const IRRegister condition = compileExpression(*statement.condition);
        exitJump = ir_.emitJumpIfFalse(condition);
    }

    const std::size_t jumpOverIncrement = ir_.emitJump();
    const std::size_t incrementStart = ir_.instructionCount();
    if (statement.increment) {
        compileExpression(*statement.increment);
    }
    ir_.emitJumpTo(loopStart);

    ir_.patchJump(jumpOverIncrement);

    loopContexts_.push_back(LoopContext{&statement, incrementStart, {}});
    compileStatement(*statement.body);
    LoopContext loop = std::move(loopContexts_.back());
    loopContexts_.pop_back();

    ir_.emitJumpTo(incrementStart);
    if (exitJump != static_cast<std::size_t>(-1)) {
        ir_.patchJump(exitJump);
    }
    for (const std::size_t breakJump : loop.breakJumps) {
        ir_.patchJump(breakJump);
    }
}

std::string IRCompiler::makeSyntheticName(const std::string& prefix)
{
    return "__" + prefix + "#" + std::to_string(nextSyntheticName_++);
}

void IRCompiler::compileForIn(const ForInStmt& statement)
{
    const BindingMetadataRecord* itemBinding = declarationIndex_
        ? declarationIndex_->forInBindingMetadata(statement)
        : nullptr;
    if (!itemBinding) {
        throw IRCompileError("missing binding metadata for for-in variable");
    }
    const std::string& itemName = itemBinding->resolvedName;
    const DeclarationRecord* itemDeclaration = declarationIndex_->declaration(statement);
    const std::optional<BindingId> itemBindingId = registerBindingMetadata(
        *itemBinding,
        itemDeclaration
            ? std::optional<DeclarationId>(itemDeclaration->declarationId)
            : std::nullopt);

    const IRRegister iterableValue = compileExpression(*statement.iterable);
    const IRRegister iterator = ir_.emitIterInit(iterableValue);

    const IRRegister initialItem = ir_.emitConstant(Value::nil());
    ir_.emitStoreVar(itemName, initialItem, itemBindingId);

    const std::size_t loopStart = ir_.instructionCount();
    const IRRegister hasNext = ir_.emitIterHas(iterator);
    const std::size_t exitJump = ir_.emitJumpIfFalse(hasNext);

    const IRRegister item = ir_.emitIterNext(iterator);
    ir_.emitAssignVar(itemName, item, itemBindingId);

    loopContexts_.push_back(LoopContext{&statement, loopStart, {}});
    compileStatement(*statement.body);
    LoopContext loop = std::move(loopContexts_.back());
    loopContexts_.pop_back();

    ir_.emitJumpTo(loopStart);
    ir_.patchJump(exitJump);
    for (const std::size_t breakJump : loop.breakJumps) {
        ir_.patchJump(breakJump);
    }
}

void IRCompiler::compileIfLet(const IfLetStmt& statement)
{
    const BindingMetadataRecord* binding = declarationIndex_
        ? declarationIndex_->ifLetBindingMetadata(statement)
        : nullptr;
    if (!binding) {
        throw IRCompileError("missing if-let binding metadata");
    }

    const IRRegister value = compileExpression(*statement.value);
    const IRRegister nilValue = ir_.emitConstant(Value::nil());
    const IRRegister notNil = ir_.emitBinary(IROp::NotEqual, value, nilValue);
    const std::size_t failJump = ir_.emitJumpIfFalse(notNil);

    const DeclarationRecord* declaration = declarationIndex_
        ? declarationIndex_->declaration(statement)
        : nullptr;
    const std::optional<BindingId> bindingId = registerBindingMetadata(
        *binding,
        declaration ? std::optional<DeclarationId>(declaration->declarationId) : std::nullopt);
    ir_.emitStoreVar(binding->resolvedName, value, bindingId);

    compileStatement(*statement.thenBranch);
    if (statement.elseBranch) {
        const std::size_t jumpOverElse = ir_.emitJump();
        ir_.patchJump(failJump);
        compileStatement(*statement.elseBranch);
        ir_.patchJump(jumpOverElse);
    } else {
        ir_.patchJump(failJump);
    }
}

void IRCompiler::compileWhileLet(const WhileLetStmt& statement)
{
    const BindingMetadataRecord* binding = declarationIndex_
        ? declarationIndex_->whileLetBindingMetadata(statement)
        : nullptr;
    if (!binding) {
        throw IRCompileError("missing while-let binding metadata");
    }

    const DeclarationRecord* declaration = declarationIndex_
        ? declarationIndex_->declaration(statement)
        : nullptr;
    const std::optional<BindingId> bindingId = registerBindingMetadata(
        *binding,
        declaration ? std::optional<DeclarationId>(declaration->declarationId) : std::nullopt);

    const std::size_t loopStart = ir_.instructionCount();
    const IRRegister value = compileExpression(*statement.value);
    const IRRegister nilValue = ir_.emitConstant(Value::nil());
    const IRRegister notNil = ir_.emitBinary(IROp::NotEqual, value, nilValue);
    const std::size_t exitJump = ir_.emitJumpIfFalse(notNil);

    ir_.emitStoreVar(binding->resolvedName, value, bindingId);

    loopContexts_.push_back(LoopContext{&statement, loopStart, {}});
    compileStatement(*statement.body);
    LoopContext loop = std::move(loopContexts_.back());
    loopContexts_.pop_back();

    ir_.emitJumpTo(loopStart);
    ir_.patchJump(exitJump);
    for (const std::size_t breakJump : loop.breakJumps) {
        ir_.patchJump(breakJump);
    }
}

IRRegister IRCompiler::compileCoalesce(const CoalesceExpr& expression)
{
    const IRRegister left = compileExpression(*expression.left);
    const IRRegister result = ir_.makeRegister();
    const IRRegister nilValue = ir_.emitConstant(Value::nil());
    const IRRegister notNil = ir_.emitBinary(IROp::NotEqual, left, nilValue);
    const std::size_t nilJump = ir_.emitJumpIfFalse(notNil);

    ir_.emitCopyTo(result, left);
    const std::size_t endJump = ir_.emitJump();

    ir_.patchJump(nilJump);
    const IRRegister right = compileExpression(*expression.right);
    ir_.emitCopyTo(result, right);
    ir_.patchJump(endJump);
    return result;
}

IRRegister IRCompiler::compileUnwrapOrReturn(const UnwrapOrReturnExpr& expression)
{
    const IRRegister value = compileExpression(*expression.value);
    const IRRegister nilValue = ir_.emitConstant(Value::nil());
    const IRRegister notNil = ir_.emitBinary(IROp::NotEqual, value, nilValue);
    const std::size_t nilJump = ir_.emitJumpIfFalse(notNil);
    const std::size_t doneJump = ir_.emitJump();

    ir_.patchJump(nilJump);
    ir_.emitReturn(nilValue);
    ir_.patchJump(doneJump);
    return value;
}

void IRCompiler::compileMatch(const MatchStmt& statement)
{
    const MatchCoverageRecord* coverage = declarationIndex_
        ? declarationIndex_->matchCoverage(statement)
        : nullptr;
    if (!coverage || !coverage->exhaustive) {
        throw IRCompileError("missing match coverage metadata");
    }
    const IRRegister value = compileExpression(*statement.value);
    std::vector<std::size_t> endJumps;

    for (const MatchArm& arm : statement.arms) {
        std::vector<std::size_t> failJumps;
        std::vector<CompiledPatternBinding> bindings;
        compilePattern(*arm.pattern, value, failJumps, bindings);
        for (const auto& binding : bindings) {
            ir_.emitStoreVar(binding.resolvedName, binding.value, binding.bindingId);
        }
        if (arm.guard) {
            const PatternGuardRecord* guardRecord = declarationIndex_
                ? declarationIndex_->patternGuard(*arm.guard)
                : nullptr;
            if (!guardRecord) {
                throw IRCompileError("missing pattern guard metadata");
            }
            const IRRegister guard = compileExpression(*arm.guard);
            failJumps.push_back(ir_.emitJumpIfFalse(guard));
        }
        compileStatement(*arm.body);
        endJumps.push_back(ir_.emitJump());
        for (const std::size_t jump : failJumps) {
            ir_.patchJump(jump);
        }
    }

    for (const std::size_t jump : endJumps) {
        ir_.patchJump(jump);
    }
}

void IRCompiler::compilePattern(
    const Pattern& pattern,
    IRRegister value,
    std::vector<std::size_t>& failJumps,
    std::vector<CompiledPatternBinding>& bindings)
{
    if (dynamic_cast<const WildcardPattern*>(&pattern)) {
        return;
    }

    if (const auto* variable = dynamic_cast<const VariablePattern*>(&pattern)) {
        const PatternBindingRecord* record = declarationIndex_
            ? declarationIndex_->patternBindingMetadata(*variable)
            : nullptr;
        if (!record || record->resolvedName.empty()) {
            throw IRCompileError("missing pattern binding metadata");
        }
        const DeclarationRecord* declaration = declarationIndex_->declaration(*variable);
        const std::optional<BindingId> bindingId = registerBinding(
            record->bindingId,
            record->resolvedName,
            declaration ? std::optional<DeclarationId>(declaration->declarationId) : std::nullopt);
        bindings.push_back(
            CompiledPatternBinding{record->sourceName, record->resolvedName, value, bindingId});
        return;
    }

    if (const auto* recordPattern = dynamic_cast<const RecordPattern*>(&pattern)) {
        const RecordPatternRecord* record = declarationIndex_
            ? declarationIndex_->recordPattern(*recordPattern)
            : nullptr;
        if (!record || record->structType.kind != StaticType::Struct
            || record->fieldNames.size() != recordPattern->fields.size()
            || record->fieldTypes.size() != recordPattern->fields.size()) {
            throw IRCompileError("missing record pattern metadata");
        }
        for (std::size_t i = 0; i < recordPattern->fields.size(); ++i) {
            const IRRegister fieldValue = ir_.emitField(
                value, record->fieldNames[i], record->structType.structName);
            compilePattern(*recordPattern->fields[i].pattern, fieldValue, failJumps, bindings);
        }
        return;
    }

    if (const auto* orPattern = dynamic_cast<const OrPattern*>(&pattern)) {
        const OrPatternRecord* record = declarationIndex_
            ? declarationIndex_->orPattern(*orPattern)
            : nullptr;
        if (!record || record->bindingNames.size() != record->bindingTypes.size()) {
            throw IRCompileError("missing OR pattern metadata");
        }
        const std::unordered_set<std::string> expectedBindingNames(
            record->bindingNames.begin(),
            record->bindingNames.end());
        std::vector<std::size_t> successJumps;
        std::vector<std::size_t> pendingFailJumps;
        std::unordered_map<std::string, IRRegister> sharedBindings;
        std::unordered_map<std::string, std::string> resolvedNamesByBinding;
        std::unordered_map<std::string, std::size_t> mergedBindingIndices;
        std::vector<CompiledPatternBinding> mergedBindings;

        for (std::size_t i = 0; i < orPattern->alternatives.size(); ++i) {
            for (const std::size_t jump : pendingFailJumps) {
                ir_.patchJump(jump);
            }
            pendingFailJumps.clear();

            std::vector<std::size_t> alternativeFailJumps;
            std::vector<CompiledPatternBinding> alternativeBindings;
            compilePattern(
                *orPattern->alternatives[i],
                value,
                alternativeFailJumps,
                alternativeBindings);

            std::unordered_set<std::string> alternativeBindingNames;
            for (const auto& binding : alternativeBindings) {
                if (expectedBindingNames.find(binding.sourceName) == expectedBindingNames.end()
                    || !alternativeBindingNames.insert(binding.sourceName).second) {
                    throw IRCompileError("OR pattern binding metadata mismatch");
                }
                const auto resolvedName = resolvedNamesByBinding.find(binding.sourceName);
                if (resolvedName != resolvedNamesByBinding.end()
                    && resolvedName->second != binding.resolvedName) {
                    throw IRCompileError("OR pattern binding metadata mismatch");
                }
                const auto mergedIndex = mergedBindingIndices.find(binding.sourceName);
                if (mergedIndex != mergedBindingIndices.end()
                    && mergedBindings[mergedIndex->second].bindingId != binding.bindingId) {
                    throw IRCompileError("OR pattern binding metadata mismatch");
                }
                resolvedNamesByBinding.insert_or_assign(
                    binding.sourceName,
                    binding.resolvedName);
                auto shared = sharedBindings.find(binding.resolvedName);
                if (shared == sharedBindings.end()) {
                    const IRRegister registerForBinding = ir_.makeRegister();
                    shared = sharedBindings.emplace(binding.resolvedName, registerForBinding).first;
                    mergedBindingIndices.emplace(binding.sourceName, mergedBindings.size());
                    mergedBindings.push_back(CompiledPatternBinding{
                        binding.sourceName,
                        binding.resolvedName,
                        registerForBinding,
                        binding.bindingId});
                }
                ir_.emitCopyTo(shared->second, binding.value);
            }
            if (alternativeBindingNames.size() != expectedBindingNames.size()) {
                throw IRCompileError("OR pattern binding metadata mismatch");
            }

            if (i + 1 < orPattern->alternatives.size()) {
                successJumps.push_back(ir_.emitJump());
                pendingFailJumps = std::move(alternativeFailJumps);
            } else {
                failJumps.insert(
                    failJumps.end(),
                    alternativeFailJumps.begin(),
                    alternativeFailJumps.end());
            }
        }

        for (const std::size_t jump : successJumps) {
            ir_.patchJump(jump);
        }
        if (sharedBindings.size() != expectedBindingNames.size()) {
            throw IRCompileError("OR pattern binding metadata mismatch");
        }
        bindings.insert(bindings.end(), mergedBindings.begin(), mergedBindings.end());
        return;
    }

    if (const auto* literal = dynamic_cast<const LiteralPattern*>(&pattern)) {
        const LiteralPatternRecord* record = declarationIndex_
            ? declarationIndex_->literalPattern(*literal)
            : nullptr;
        if (!record) {
            throw IRCompileError("missing literal pattern metadata");
        }
        const IRRegister expected = ir_.emitConstant(literalValue(record->literal));
        const IRRegister equal = ir_.emitBinary(IROp::Equal, value, expected);
        failJumps.push_back(ir_.emitJumpIfFalse(equal));
        return;
    }

    const auto* variant = dynamic_cast<const VariantPattern*>(&pattern);
    if (!variant) {
        throw IRCompileError("unsupported pattern node");
    }
    const VariantPatternRecord* record = declarationIndex_
        ? declarationIndex_->variantPattern(*variant)
        : nullptr;
    if (!record || record->enumType.kind != StaticType::Enum
        || record->payloadTypes.size() != variant->arguments.size()) {
        throw IRCompileError("missing variant pattern metadata");
    }

    const IRRegister tag = ir_.emitVariantTag(
        value, record->enumName, record->variantName);
    failJumps.push_back(ir_.emitJumpIfFalse(tag));
    for (std::size_t i = 0; i < variant->arguments.size(); ++i) {
        const IRRegister field = ir_.emitVariantField(
            value, i, record->enumName, record->variantName);
        compilePattern(*variant->arguments[i], field, failJumps, bindings);
    }
}

IRRegister IRCompiler::compileExpression(const Expr& expression)
{
    SpanScope scope(*this, debugSpan(expression.range, expression.span));
    if (const auto* literal = dynamic_cast<const LiteralExpr*>(&expression)) {
        return ir_.emitConstant(literalValue(literal->value));
    }

    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
        typedExpressionType(*variable, "variable read");
        const BindingMetadataRecord* binding = declarationIndex_
            ? declarationIndex_->variableBindingMetadata(*variable)
            : nullptr;
        if (!binding) {
            throw IRCompileError("missing binding metadata for variable read");
        }
        const std::optional<ResolvedSymbol> resolved = declarationIndex_->variableReference(*variable);
        const DeclarationRecord* declaration = resolved
            ? declarationIndex_->declaration(resolved->declarationId)
            : nullptr;
        const std::optional<BindingId> bindingId = registerBindingMetadata(
            *binding,
            declaration ? std::optional<DeclarationId>(declaration->declarationId) : std::nullopt);
        return ir_.emitLoadVar(binding->resolvedName, bindingId);
    }

    if (const auto* assign = dynamic_cast<const AssignExpr*>(&expression)) {
        typedExpressionType(*assign, "assignment");
        const IRRegister value = compileExpression(*assign->value);
        const BindingMetadataRecord* binding = declarationIndex_
            ? declarationIndex_->assignmentBindingMetadata(*assign)
            : nullptr;
        if (!binding) {
            throw IRCompileError("missing binding metadata for assignment");
        }
        const std::optional<ResolvedSymbol> resolved = declarationIndex_->assignmentReference(*assign);
        const DeclarationRecord* declaration = resolved
            ? declarationIndex_->declaration(resolved->declarationId)
            : nullptr;
        const std::optional<BindingId> bindingId = registerBindingMetadata(
            *binding,
            declaration ? std::optional<DeclarationId>(declaration->declarationId) : std::nullopt);
        ir_.emitAssignVar(binding->resolvedName, value, bindingId);
        return value;
    }

    if (const auto* compound = dynamic_cast<const CompoundAssignExpr*>(&expression)) {
        return emitCompoundAssign(*compound);
    }

    if (const auto* indexCompound = dynamic_cast<const IndexCompoundAssignExpr*>(&expression)) {
        return emitIndexCompoundAssign(*indexCompound);
    }

    if (const auto* fieldCompound = dynamic_cast<const FieldCompoundAssignExpr*>(&expression)) {
        return emitFieldCompoundAssign(*fieldCompound);
    }

    if (const auto* indexAssign = dynamic_cast<const IndexAssignExpr*>(&expression)) {
        return emitIndexAssign(*indexAssign);
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignExpr*>(&expression)) {
        return emitFieldAssign(*fieldAssign);
    }

    if (const auto* grouping = dynamic_cast<const GroupingExpr*>(&expression)) {
        return compileExpression(*grouping->expression);
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        const IRRegister value = compileExpression(*unary->right);
        if (unary->op.type == TokenType::Minus) {
            const TypedExpressionRecord* record = typedExpressionRecord(*unary);
            if (record && record->type.kind == StaticType::Number) {
                return ir_.emitUnary(IROp::NegNum, value);
            }
        }
        return emitUnary(unary->op.type, value);
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        const IRRegister left = compileExpression(*binary->left);
        const IRRegister right = compileExpression(*binary->right);
        const TypedExpressionRecord* binaryRecord = declarationIndex_
            ? declarationIndex_->typedExpression(*binary)
            : nullptr;
        const TypeInfo* resultType = binaryRecord ? &binaryRecord->type : nullptr;
        const TypedExpressionRecord* leftOperand = typedExpressionRecord(*binary->left);
        const TypeInfo* operandType = leftOperand ? &leftOperand->type : nullptr;
        return emitBinary(binary->op.type, left, right, operandType, resultType);
    }

    if (const auto* logical = dynamic_cast<const LogicalExpr*>(&expression)) {
        return emitLogical(*logical);
    }

    if (const auto* coalesce = dynamic_cast<const CoalesceExpr*>(&expression)) {
        return compileCoalesce(*coalesce);
    }

    if (const auto* unwrap = dynamic_cast<const UnwrapOrReturnExpr*>(&expression)) {
        return compileUnwrapOrReturn(*unwrap);
    }

    if (const auto* function = dynamic_cast<const FunctionExpr*>(&expression)) {
        return emitFunctionExpr(*function);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
        return emitCall(*call);
    }

    if (const auto* memberCall = dynamic_cast<const MemberCallExpr*>(&expression)) {
        return emitMemberCall(*memberCall);
    }

    if (const auto* array = dynamic_cast<const ArrayExpr*>(&expression)) {
        return emitArray(*array);
    }

    if (const auto* map = dynamic_cast<const MapExpr*>(&expression)) {
        return emitMap(*map);
    }

    if (const auto* construct = dynamic_cast<const StructConstructExpr*>(&expression)) {
        return emitStructConstructor(*construct);
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
        return emitIndex(*index);
    }

    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expression)) {
        return emitFieldAccess(*field);
    }

    throw IRCompileError("unsupported expression node");
}

IRRegister IRCompiler::emitFunctionExpr(const FunctionExpr& expression)
{
    typedExpressionType(expression, StaticType::Function, "function expression");
    if (!declarationIndex_ || !declarationIndex_->captureMetadata(expression)) {
        throw IRCompileError("missing function expression metadata");
    }
    const FunctionMetadataRecord* metadata = declarationIndex_->functionMetadata(expression);
    if (!metadata) {
        throw IRCompileError("missing function expression metadata");
    }
    std::vector<std::string> parameters = metadata->parameterNames;
    ir_.beginFunction(metadata->functionLabel, std::move(parameters));
    ++activeFunctionDepth_;
    registerFunctionParameters(
        *metadata,
        declarationIndex_->functionParameterDeclarations(expression));

    std::vector<LoopContext> enclosingLoopContexts = std::move(loopContexts_);
    loopContexts_.clear();
    for (const auto& statement : expression.body) {
        compileStatement(*statement);
    }
    IRRegister nilValue = ir_.emitConstant(Value::nil());
    ir_.emitReturn(nilValue);
    loopContexts_ = std::move(enclosingLoopContexts);
    --activeFunctionDepth_;

    const std::size_t functionIndex = ir_.endFunction();
    return ir_.emitMakeFunction(functionIndex);
}

bool IRCompiler::isBuiltinLenCall(const CallExpr& expression) const
{
    const auto* variable = dynamic_cast<const VariableExpr*>(expression.callee.get());
    return variable
        && variable->name.lexeme == "len"
        && (!declarationIndex_ || !declarationIndex_->variableBindingMetadata(*variable));
}

bool IRCompiler::hasNativeCallMetadata(const CallExpr& expression) const
{
    return declarationIndex_ && declarationIndex_->nativeCall(expression) != nullptr;
}

IRRegister IRCompiler::emitLenCall(const CallExpr& expression)
{
    if (expression.arguments.size() != 1) {
        throw IRCompileError("len expects exactly one argument");
    }
    const IRRegister value = compileExpression(*expression.arguments.front());
    return emitLenTyped(value, *expression.arguments.front());
}

IRRegister IRCompiler::emitLenTyped(IRRegister value, const Expr& operand)
{
    const TypedExpressionRecord* record = typedExpressionRecord(operand);
    if (!record) {
        return ir_.emitLen(value);
    }
    switch (record->type.kind) {
    case StaticType::Array:
        return ir_.emitLenArray(value);
    case StaticType::Map:
        return ir_.emitLenMap(value);
    case StaticType::Range:
        return ir_.emitLenRange(value);
    case StaticType::String:
        return ir_.emitLenStr(value);
    default:
        return ir_.emitLen(value);
    }
}

IRRegister IRCompiler::emitNativeStdlibCall(const CallExpr& expression)
{
    const NativeCallRecord* nativeCall = declarationIndex_
        ? declarationIndex_->nativeCall(expression)
        : nullptr;
    if (!nativeCall) {
        throw IRCompileError("native stdlib call missing variable callee");
    }

    std::vector<IRRegister> arguments;
    for (const auto& argument : expression.arguments) {
        arguments.push_back(compileExpression(*argument));
    }
    return ir_.emitNativeCall(nativeCall->name, std::move(arguments));
}

IRRegister IRCompiler::emitMemberCall(const MemberCallExpr& expression)
{
    if (declarationIndex_ && declarationIndex_->variantConstructor(expression)) {
        return emitVariantConstructor(expression);
    }

    const MemberCallMetadataRecord* memberCall = declarationIndex_
        ? declarationIndex_->memberCallMetadata(expression)
        : nullptr;
    if (memberCall) {
        typedExpressionType(expression, "member call");
        if (memberCall->hasTarget) {
            const CallTargetRecord* target = declarationIndex_->callTarget(expression);
            if (!target || target->kind != CallTargetKind::StructMethod) {
                throw IRCompileError("missing struct method call target metadata");
            }
        }
        const auto binding = bindingIdsByResolvedName_.find(memberCall->calleeName);
        const std::optional<BindingId> bindingId = binding == bindingIdsByResolvedName_.end()
            ? std::nullopt
            : std::optional<BindingId>(binding->second);
        const IRRegister callee = ir_.emitLoadVar(memberCall->calleeName, bindingId);
        std::vector<IRRegister> arguments;
        if (memberCall->passesReceiver) {
            arguments.push_back(compileExpression(*expression.receiver));
        }
        for (const auto& argument : expression.arguments) {
            arguments.push_back(compileExpression(*argument));
        }
        return ir_.emitCall(callee, std::move(arguments));
    }

    const IRRegister receiver = compileExpression(*expression.receiver);

    if (expression.name.lexeme == "len") {
        if (!expression.arguments.empty()) {
            throw IRCompileError("len member call expects no arguments");
        }
        return emitLenTyped(receiver, *expression.receiver);
    }

    const NativeCallRecord* nativeCall = declarationIndex_
        ? declarationIndex_->nativeCall(expression)
        : nullptr;
    if (nativeCall) {
        std::vector<IRRegister> arguments;
        arguments.push_back(receiver);
        for (const auto& argument : expression.arguments) {
            arguments.push_back(compileExpression(*argument));
        }
        return ir_.emitNativeCall(nativeCall->name, std::move(arguments));
    }

    throw IRCompileError("unknown member call `" + expression.name.lexeme + "`");
}

IRRegister IRCompiler::emitVariantConstructor(const MemberCallExpr& expression)
{
    const VariantConstructorRecord* variant = declarationIndex_
        ? declarationIndex_->variantConstructor(expression)
        : nullptr;
    if (!variant || variant->resultType.kind != StaticType::Enum
        || variant->payloadTypes.size() != expression.arguments.size()) {
        throw IRCompileError("missing variant constructor metadata");
    }
    std::vector<IRRegister> payload;
    payload.reserve(expression.arguments.size());
    for (const auto& argument : expression.arguments) {
        payload.push_back(compileExpression(*argument));
    }
    return ir_.emitVariant(
        variant->enumName,
        variant->variantName,
        std::move(payload));
}

IRRegister IRCompiler::emitCall(const CallExpr& expression)
{
    if (isBuiltinLenCall(expression)) {
        return emitLenCall(expression);
    }

    if (hasNativeCallMetadata(expression)) {
        return emitNativeStdlibCall(expression);
    }

    typedExpressionType(expression, "call");
    const auto directCallee = [](const Expr* callee) -> const VariableExpr* {
        while (const auto* grouping = dynamic_cast<const GroupingExpr*>(callee)) {
            callee = grouping->expression.get();
        }
        return dynamic_cast<const VariableExpr*>(callee);
    };
    if (const VariableExpr* callee = directCallee(expression.callee.get())) {
        if (declarationIndex_->variableReference(*callee).has_value()) {
            const CallTargetRecord* target = declarationIndex_->callTarget(expression);
            if (!target || target->kind != CallTargetKind::Direct) {
                throw IRCompileError("missing direct call target metadata");
            }
            const DeclarationRecord* declaration
                = declarationIndex_->declaration(target->target.declarationId);
            const BindingMetadataRecord* binding
                = declarationIndex_->variableBindingMetadata(*callee);
            const bool imported = binding && binding->imported;
            const CaptureRecord* captures = nullptr;
            if (declaration && declaration->statement) {
                if (const auto* function
                    = dynamic_cast<const FunctionStmt*>(declaration->statement)) {
                    captures = declarationIndex_->captureMetadata(*function);
                }
            }
            if (!imported && declaration && captures && captures->symbols.empty()) {
                std::vector<IRRegister> directArguments;
                directArguments.reserve(expression.arguments.size());
                for (const auto& argument : expression.arguments) {
                    directArguments.push_back(compileExpression(*argument));
                }
                const std::size_t instructionIndex = ir_.instructionCount();
                const IRRegister result = ir_.emitCallDirect(0, std::move(directArguments));
                pendingDirectCalls_.push_back(PendingDirectCall{
                    activeFunctionDepth_ != 0
                        ? std::optional<std::size_t>(ir_.functionCount())
                        : std::nullopt,
                    instructionIndex,
                    target->target.declarationId});
                return result;
            }
        }
    }

    IRRegister callee = compileExpression(*expression.callee);
    std::vector<IRRegister> arguments;
    for (const auto& argument : expression.arguments) {
        arguments.push_back(compileExpression(*argument));
    }
    return ir_.emitCall(callee, std::move(arguments));
}

IRRegister IRCompiler::emitArray(const ArrayExpr& expression)
{
    typedExpressionType(expression, StaticType::Array, "array literal");
    std::vector<IRRegister> elements;
    for (const auto& element : expression.elements) {
        elements.push_back(compileExpression(*element));
    }
    return ir_.emitArray(std::move(elements));
}

IRRegister IRCompiler::emitMap(const MapExpr& expression)
{
    typedExpressionType(expression, StaticType::Map, "map literal");
    std::vector<IRRegister> keyValueRegisters;
    keyValueRegisters.reserve(expression.entries.size() * 2);
    for (const MapEntry& entry : expression.entries) {
        keyValueRegisters.push_back(compileExpression(*entry.key));
        keyValueRegisters.push_back(compileExpression(*entry.value));
    }
    return ir_.emitMap(std::move(keyValueRegisters));
}

IRRegister IRCompiler::emitStructFields(
    const std::vector<StructField>& fields,
    const std::vector<std::string>& fieldNames,
    std::optional<std::string> typeName)
{
    if (fields.size() != fieldNames.size()) {
        throw IRCompileError("struct constructor metadata field count mismatch");
    }
    std::vector<std::size_t> names;
    std::vector<IRRegister> values;
    names.reserve(fields.size());
    values.reserve(fields.size());

    std::optional<std::size_t> typeNameOperand;
    if (typeName) {
        typeNameOperand = ir_.addName(std::move(*typeName));
    }

    for (std::size_t i = 0; i < fields.size(); ++i) {
        names.push_back(ir_.addName(fieldNames[i]));
        const StructField& field = fields[i];
        values.push_back(compileExpression(*field.value));
    }
    return ir_.emitStruct(std::move(names), std::move(values), typeNameOperand);
}

IRRegister IRCompiler::emitStructConstructor(const StructConstructExpr& expression)
{
    const StructConstructorRecord* constructor = declarationIndex_
        ? declarationIndex_->structConstructor(expression)
        : nullptr;
    if (!constructor || constructor->type.kind != StaticType::Struct || !constructor->type.structName) {
        throw IRCompileError("typed struct constructor is missing a type name");
    }
    return emitStructFields(
        expression.fields,
        constructor->fieldNames,
        *constructor->type.structName);
}

IRRegister IRCompiler::emitIndex(const IndexExpr& expression)
{
    const IndexOperationRecord& operation
        = indexOperation(expression, IndexOperationKind::Read, "index expression");
    IRRegister collection = compileExpression(*expression.collection);
    IRRegister index = compileExpression(*expression.index);
    switch (operation.collectionType.kind) {
    case StaticType::Array:
        return ir_.emitArrayGet(collection, index);
    case StaticType::Map:
        return ir_.emitMapGet(collection, index);
    case StaticType::Range:
        return ir_.emitRangeGet(collection, index);
    default:
        break;
    }
    return ir_.emitIndex(collection, index);
}

IROp IRCompiler::compoundAssignmentOp(TokenType op) const
{
    switch (op) {
    case TokenType::PlusEqual:
        return IROp::AddNum;
    case TokenType::MinusEqual:
        return IROp::SubNum;
    case TokenType::StarEqual:
        return IROp::MulNum;
    case TokenType::SlashEqual:
        return IROp::DivNum;
    default:
        throw IRCompileError("unsupported compound assignment operator: " + tokenTypeName(op));
    }
}

IRRegister IRCompiler::emitCompoundAssign(const CompoundAssignExpr& expression)
{
    typedExpressionType(expression, StaticType::Number, "compound assignment");
    const BindingMetadataRecord* binding = declarationIndex_
        ? declarationIndex_->compoundAssignmentBindingMetadata(expression)
        : nullptr;
    if (!binding) {
        throw IRCompileError("missing binding metadata for compound assignment");
    }
    const std::string& name = binding->resolvedName;
    const std::optional<ResolvedSymbol> resolved = declarationIndex_->compoundAssignmentReference(expression);
    const DeclarationRecord* declaration = resolved
        ? declarationIndex_->declaration(resolved->declarationId)
        : nullptr;
    const std::optional<BindingId> bindingId = registerBindingMetadata(
        *binding,
        declaration ? std::optional<DeclarationId>(declaration->declarationId) : std::nullopt);
    const IRRegister oldValue = ir_.emitLoadVar(name, bindingId);
    const IRRegister result = emitCompoundAssignmentResult(
        expression.op, oldValue, *expression.value, "`" + expression.op.lexeme + "` expects number variable");
    ir_.emitAssignVar(name, result, bindingId);
    return result;
}

IRRegister IRCompiler::emitCompoundAssignmentResult(
    const Token& op,
    IRRegister oldValue,
    const Expr& valueExpression,
    const std::string& targetMessage)
{
    const IRRegister checkedOldValue = ir_.emitAssertNumber(oldValue, targetMessage);
    const IRRegister value = compileExpression(valueExpression);
    const IRRegister checkedValue = ir_.emitAssertNumber(
        value, "`" + op.lexeme + "` expects number value");
    return ir_.emitBinary(compoundAssignmentOp(op.type), checkedOldValue, checkedValue);
}

IRRegister IRCompiler::emitIndexAssign(const IndexAssignExpr& expression)
{
    const IndexOperationRecord& operation
        = indexOperation(expression, IndexOperationKind::Assign, "index assignment");
    IRRegister collection = compileExpression(*expression.collection);
    IRRegister index = compileExpression(*expression.index);
    IRRegister value = compileExpression(*expression.value);
    if (operation.collectionType.kind == StaticType::Array) {
        return ir_.emitArraySet(collection, index, value);
    }
    if (operation.collectionType.kind == StaticType::Map) {
        return ir_.emitMapSet(collection, index, value);
    }
    return ir_.emitAssignIndex(collection, index, value);
}

IRRegister IRCompiler::emitIndexCompoundAssign(const IndexCompoundAssignExpr& expression)
{
    const IndexOperationRecord& operation = indexOperation(
        expression, IndexOperationKind::CompoundAssign, "index compound assignment");
    if (operation.resultType.kind != StaticType::Number) {
        throw IRCompileError("missing aggregate index metadata for index compound assignment");
    }
    IRRegister collection = compileExpression(*expression.collection);
    IRRegister index = compileExpression(*expression.index);
    IRRegister oldValue = ir_.emitArrayGet(collection, index);
    IRRegister result = emitCompoundAssignmentResult(
        expression.op, oldValue, *expression.value, "`" + expression.op.lexeme + "` expects number target");
    ir_.emitArraySet(collection, index, result);
    return result;
}

IRRegister IRCompiler::emitFieldAccess(const FieldAccessExpr& expression)
{
    const FieldOperationRecord& operation = fieldOperation(
        expression, FieldOperationKind::Read, "field access");
    if (operation.resolvedName) {
        return ir_.emitLoadVar(*operation.resolvedName);
    }
    const TypedExpressionRecord* objectRecord = declarationIndex_
        ? declarationIndex_->typedExpression(*expression.object)
        : nullptr;
    const std::optional<std::string> objectStructName = objectRecord
        ? objectRecord->type.structName
        : std::nullopt;
    IRRegister object = compileExpression(*expression.object);
    return ir_.emitField(
        object, operation.fieldName, objectStructName);
}

IRRegister IRCompiler::emitFieldAssign(const FieldAssignExpr& expression)
{
    const FieldOperationRecord& operation = fieldOperation(
        expression, FieldOperationKind::Assign, "field assignment");
    const TypedExpressionRecord* objectRecord = declarationIndex_
        ? declarationIndex_->typedExpression(*expression.object)
        : nullptr;
    const std::optional<std::string> objectStructName = objectRecord
        ? objectRecord->type.structName
        : std::nullopt;
    IRRegister object = compileExpression(*expression.object);
    IRRegister value = compileExpression(*expression.value);
    return ir_.emitAssignField(
        object, operation.fieldName, objectStructName, value);
}

IRRegister IRCompiler::emitFieldCompoundAssign(const FieldCompoundAssignExpr& expression)
{
    const FieldOperationRecord& operation = fieldOperation(
        expression, FieldOperationKind::CompoundAssign, "field compound assignment");
    if (operation.resultType.kind != StaticType::Number) {
        throw IRCompileError("missing aggregate field metadata for field compound assignment");
    }
    const TypedExpressionRecord* objectRecord = declarationIndex_
        ? declarationIndex_->typedExpression(*expression.object)
        : nullptr;
    const std::optional<std::string> objectStructName = objectRecord
        ? objectRecord->type.structName
        : std::nullopt;
    IRRegister object = compileExpression(*expression.object);
    IRRegister oldValue = ir_.emitField(object, operation.fieldName, objectStructName);
    IRRegister result = emitCompoundAssignmentResult(
        expression.op, oldValue, *expression.value, "`" + expression.op.lexeme + "` expects number target");
    ir_.emitAssignField(object, operation.fieldName, objectStructName, result);
    return result;
}

IRRegister IRCompiler::emitUnary(TokenType op, IRRegister value)
{
    switch (op) {
    case TokenType::Bang:
        return ir_.emitUnary(IROp::Not, value);
    case TokenType::Minus:
        return ir_.emitUnary(IROp::Negate, value);
    default:
        throw IRCompileError("unsupported unary operator: " + tokenTypeName(op));
    }
}

IRRegister IRCompiler::emitBinary(
    TokenType op,
    IRRegister left,
    IRRegister right,
    const TypeInfo* operandType,
    const TypeInfo* resultType)
{
    const bool knownNumber = operandType && operandType->kind == StaticType::Number;
    const bool knownString = operandType && operandType->kind == StaticType::String;
    const bool numericResult = resultType && resultType->kind == StaticType::Number;
    const bool stringResult = resultType && resultType->kind == StaticType::String;
    switch (op) {
    case TokenType::Plus:
        if (numericResult) {
            return ir_.emitBinary(IROp::AddNum, left, right);
        }
        if (stringResult) {
            return ir_.emitBinary(IROp::ConcatStr, left, right);
        }
        return ir_.emitBinary(IROp::Add, left, right);
    case TokenType::Minus:
        return ir_.emitBinary(numericResult ? IROp::SubNum : IROp::Subtract, left, right);
    case TokenType::Star:
        return ir_.emitBinary(numericResult ? IROp::MulNum : IROp::Multiply, left, right);
    case TokenType::Slash:
        return ir_.emitBinary(numericResult ? IROp::DivNum : IROp::Divide, left, right);
    case TokenType::EqualEqual:
        return ir_.emitBinary(IROp::Equal, left, right);
    case TokenType::BangEqual:
        return ir_.emitBinary(IROp::NotEqual, left, right);
    case TokenType::Greater:
        return ir_.emitBinary(
            knownNumber ? IROp::GreaterNum : knownString ? IROp::GreaterStr : IROp::Greater,
            left, right);
    case TokenType::GreaterEqual:
        return ir_.emitBinary(
            knownNumber ? IROp::GreaterEqualNum
                        : knownString ? IROp::GreaterEqualStr : IROp::GreaterEqual,
            left, right);
    case TokenType::Less:
        return ir_.emitBinary(
            knownNumber ? IROp::LessNum : knownString ? IROp::LessStr : IROp::Less,
            left, right);
    case TokenType::LessEqual:
        return ir_.emitBinary(
            knownNumber ? IROp::LessEqualNum
                        : knownString ? IROp::LessEqualStr : IROp::LessEqual,
            left, right);
    default:
        throw IRCompileError("unsupported binary operator: " + tokenTypeName(op));
    }
}

IRRegister IRCompiler::emitLogical(const LogicalExpr& expression)
{
    const IRRegister left = compileExpression(*expression.left);
    const IRRegister result = ir_.emitCopy(left);

    std::size_t jump = 0;
    switch (expression.op.type) {
    case TokenType::PipePipe:
        jump = ir_.emitJumpIfTrue(result);
        break;
    case TokenType::AmpersandAmpersand:
        jump = ir_.emitJumpIfFalse(result);
        break;
    default:
        throw IRCompileError("unsupported logical operator: " + tokenTypeName(expression.op.type));
    }

    const IRRegister right = compileExpression(*expression.right);
    ir_.emitCopyTo(result, right);
    ir_.patchJump(jump);
    return result;
}
