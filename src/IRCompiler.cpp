#include "IRCompiler.hpp"

#include <cstdlib>
#include <unordered_map>
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

IRProgram IRCompiler::compile(
    const Program& program,
    const ResolvedNames& resolvedNames,
    const DeclarationIndex& declarationIndex)
{
    ir_ = IRProgram();
    ir_.setSources(program.sources);
    currentSpan_ = std::nullopt;
    ir_.setCurrentSpan(std::nullopt);
    resolvedNames_ = &resolvedNames;
    declarationIndex_ = &declarationIndex;
    modules_.clear();
    compiledModules_.clear();
    loopContexts_.clear();
    for (const auto& statement : program.statements) {
        if (const auto* module = dynamic_cast<const ModuleStmt*>(statement.get())) {
            modules_.emplace(module->moduleId, module);
        }
    }
    for (const auto& statement : program.statements) {
        compileStatement(*statement);
    }
    resolvedNames_ = nullptr;
    modules_.clear();
    compiledModules_.clear();
    loopContexts_.clear();
    declarationIndex_ = nullptr;
    return std::move(ir_);
}

void IRCompiler::compileStatement(const Stmt& statement)
{
    SpanScope scope(*this, statement.span);
    if (const auto* module = dynamic_cast<const ModuleStmt*>(&statement)) {
        if (module->isEntry) {
            compileModule(*module);
        }
        return;
    }

    if (const auto* import = dynamic_cast<const ImportStmt*>(&statement)) {
        const auto found = modules_.find(import->resolvedModuleId);
        if (found == modules_.end()) {
            throw IRCompileError("internal error: unresolved import module");
        }
        compileModule(*found->second);
        return;
    }

    if (const auto* exportStmt = dynamic_cast<const ExportStmt*>(&statement)) {
        if (exportStmt->sourcePath) {
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

    if (const auto* match = dynamic_cast<const MatchStmt*>(&statement)) {
        compileMatch(*match);
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
        const std::size_t loopStart = ir_.instructionCount();
        const IRRegister condition = compileExpression(*whileStmt->condition);
        const std::size_t exitJump = ir_.emitJumpIfFalse(condition);

        loopContexts_.push_back(LoopContext{loopStart, {}});
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
        ir_.emitStoreVar(resolvedNames_->letName(*let), value);
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
    const std::string functionName = resolvedNames_->functionName(function);
    IRRegister placeholder = ir_.emitConstant(Value::nil());
    ir_.emitStoreVar(functionName, placeholder);

    std::vector<std::string> parameters = resolvedNames_->parameterNames(function);
    ir_.beginFunction(function.name.lexeme, std::move(parameters));

    std::vector<LoopContext> enclosingLoopContexts = std::move(loopContexts_);
    loopContexts_.clear();
    for (const auto& statement : function.body) {
        compileStatement(*statement);
    }
    IRRegister nilValue = ir_.emitConstant(Value::nil());
    ir_.emitReturn(nilValue);
    loopContexts_ = std::move(enclosingLoopContexts);

    const std::size_t functionIndex = ir_.endFunction();
    IRRegister value = ir_.emitMakeFunction(functionIndex);
    ir_.emitAssignVar(functionName, value);
}

void IRCompiler::compileImpl(const ImplStmt& statement)
{
    for (const MethodDecl& method : statement.methods) {
        compileMethod(method);
    }
}

void IRCompiler::compileMethod(const MethodDecl& method)
{
    const std::string methodName = resolvedNames_->methodName(method);
    IRRegister placeholder = ir_.emitConstant(Value::nil());
    ir_.emitStoreVar(methodName, placeholder);

    std::vector<std::string> parameters = resolvedNames_->methodParameterNames(method);
    ir_.beginFunction(method.name.lexeme, std::move(parameters));

    std::vector<LoopContext> enclosingLoopContexts = std::move(loopContexts_);
    loopContexts_.clear();
    for (const auto& statement : method.body) {
        compileStatement(*statement);
    }
    IRRegister nilValue = ir_.emitConstant(Value::nil());
    ir_.emitReturn(nilValue);
    loopContexts_ = std::move(enclosingLoopContexts);

    const std::size_t functionIndex = ir_.endFunction();
    IRRegister value = ir_.emitMakeFunction(functionIndex);
    ir_.emitAssignVar(methodName, value);
}

void IRCompiler::compileReturn(const ReturnStmt& statement)
{
    IRRegister value = statement.value ? compileExpression(*statement.value) : ir_.emitConstant(Value::nil());
    ir_.emitReturn(value);
}

void IRCompiler::compileBreak(const BreakStmt&)
{
    if (loopContexts_.empty()) {
        throw IRCompileError("`break` can only be used inside a loop");
    }
    loopContexts_.back().breakJumps.push_back(ir_.emitJump());
}

void IRCompiler::compileContinue(const ContinueStmt&)
{
    if (loopContexts_.empty()) {
        throw IRCompileError("`continue` can only be used inside a loop");
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

    loopContexts_.push_back(LoopContext{incrementStart, {}});
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
    const std::string iterableName = makeSyntheticName("for_in_iter");
    const std::string indexName = makeSyntheticName("for_in_index");
    const std::string lengthName = makeSyntheticName("for_in_len");
    const std::string itemName = resolvedNames_->forInVariableName(statement);

    const IRRegister iterableValue = compileExpression(*statement.iterable);
    const IRRegister arrayValue = ir_.emitAssertArray(iterableValue);
    ir_.emitStoreVar(iterableName, arrayValue);

    const IRRegister zero = ir_.emitConstant(Value::number(0));
    ir_.emitStoreVar(indexName, zero);

    const IRRegister initialItem = ir_.emitConstant(Value::nil());
    ir_.emitStoreVar(itemName, initialItem);

    const IRRegister loadedArrayForLen = ir_.emitLoadVar(iterableName);
    const IRRegister length = ir_.emitLen(loadedArrayForLen);
    ir_.emitStoreVar(lengthName, length);

    const std::size_t loopStart = ir_.instructionCount();
    const IRRegister currentIndex = ir_.emitLoadVar(indexName);
    const IRRegister currentLength = ir_.emitLoadVar(lengthName);
    const IRRegister condition = ir_.emitBinary(IROp::Less, currentIndex, currentLength);
    const std::size_t exitJump = ir_.emitJumpIfFalse(condition);

    const IRRegister arrayForElement = ir_.emitLoadVar(iterableName);
    const IRRegister indexForElement = ir_.emitLoadVar(indexName);
    const IRRegister item = ir_.emitIndex(arrayForElement, indexForElement);
    ir_.emitAssignVar(itemName, item);

    const std::size_t jumpOverIncrement = ir_.emitJump();
    const std::size_t incrementStart = ir_.instructionCount();
    const IRRegister indexBeforeIncrement = ir_.emitLoadVar(indexName);
    const IRRegister one = ir_.emitConstant(Value::number(1));
    const IRRegister nextIndex = ir_.emitBinary(IROp::Add, indexBeforeIncrement, one);
    ir_.emitAssignVar(indexName, nextIndex);
    ir_.emitJumpTo(loopStart);

    ir_.patchJump(jumpOverIncrement);

    loopContexts_.push_back(LoopContext{incrementStart, {}});
    compileStatement(*statement.body);
    LoopContext loop = std::move(loopContexts_.back());
    loopContexts_.pop_back();

    ir_.emitJumpTo(incrementStart);
    ir_.patchJump(exitJump);
    for (const std::size_t breakJump : loop.breakJumps) {
        ir_.patchJump(breakJump);
    }
}

void IRCompiler::compileMatch(const MatchStmt& statement)
{
    const IRRegister value = compileExpression(*statement.value);
    std::vector<std::size_t> endJumps;

    for (const MatchArm& arm : statement.arms) {
        std::vector<std::size_t> failJumps;
        std::vector<std::pair<std::string, IRRegister>> bindings;
        compilePattern(*arm.pattern, value, failJumps, bindings);
        for (const auto& binding : bindings) {
            ir_.emitStoreVar(binding.first, binding.second);
        }
        if (arm.guard) {
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

IRRegister IRCompiler::compileMatchExpression(const MatchExpr& expression)
{
    const IRRegister value = compileExpression(*expression.value);
    const IRRegister result = ir_.makeRegister();
    std::vector<std::size_t> endJumps;

    for (const MatchExprArm& arm : expression.arms) {
        std::vector<std::size_t> failJumps;
        std::vector<std::pair<std::string, IRRegister>> bindings;
        compilePattern(*arm.pattern, value, failJumps, bindings);
        for (const auto& binding : bindings) {
            ir_.emitStoreVar(binding.first, binding.second);
        }
        if (arm.guard) {
            const IRRegister guard = compileExpression(*arm.guard);
            failJumps.push_back(ir_.emitJumpIfFalse(guard));
        }
        const IRRegister armValue = compileExpression(*arm.value);
        ir_.emitCopyTo(result, armValue);
        endJumps.push_back(ir_.emitJump());
        for (const std::size_t jump : failJumps) {
            ir_.patchJump(jump);
        }
    }

    for (const std::size_t jump : endJumps) {
        ir_.patchJump(jump);
    }
    return result;
}

void IRCompiler::compilePattern(
    const Pattern& pattern,
    IRRegister value,
    std::vector<std::size_t>& failJumps,
    std::vector<std::pair<std::string, IRRegister>>& bindings)
{
    if (dynamic_cast<const WildcardPattern*>(&pattern)) {
        return;
    }

    if (const auto* variable = dynamic_cast<const VariablePattern*>(&pattern)) {
        bindings.emplace_back(resolvedNames_->patternVariableName(*variable), value);
        return;
    }

    if (const auto* recordPattern = dynamic_cast<const RecordPattern*>(&pattern)) {
        for (const RecordPatternField& field : recordPattern->fields) {
            const IRRegister fieldValue = ir_.emitField(value, field.name.lexeme);
            compilePattern(*field.pattern, fieldValue, failJumps, bindings);
        }
        return;
    }

    if (const auto* orPattern = dynamic_cast<const OrPattern*>(&pattern)) {
        std::vector<std::size_t> successJumps;
        std::vector<std::size_t> pendingFailJumps;
        std::unordered_map<std::string, IRRegister> sharedBindings;
        std::vector<std::pair<std::string, IRRegister>> mergedBindings;

        for (std::size_t i = 0; i < orPattern->alternatives.size(); ++i) {
            for (const std::size_t jump : pendingFailJumps) {
                ir_.patchJump(jump);
            }
            pendingFailJumps.clear();

            std::vector<std::size_t> alternativeFailJumps;
            std::vector<std::pair<std::string, IRRegister>> alternativeBindings;
            compilePattern(
                *orPattern->alternatives[i],
                value,
                alternativeFailJumps,
                alternativeBindings);

            for (const auto& binding : alternativeBindings) {
                auto shared = sharedBindings.find(binding.first);
                if (shared == sharedBindings.end()) {
                    const IRRegister registerForBinding = ir_.makeRegister();
                    shared = sharedBindings.emplace(binding.first, registerForBinding).first;
                    mergedBindings.emplace_back(binding.first, registerForBinding);
                }
                ir_.emitCopyTo(shared->second, binding.second);
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
        bindings.insert(bindings.end(), mergedBindings.begin(), mergedBindings.end());
        return;
    }

    if (const auto* literal = dynamic_cast<const LiteralPattern*>(&pattern)) {
        const IRRegister expected = ir_.emitConstant(literalValue(literal->value.lexeme));
        const IRRegister equal = ir_.emitBinary(IROp::Equal, value, expected);
        failJumps.push_back(ir_.emitJumpIfFalse(equal));
        return;
    }

    const auto* variant = dynamic_cast<const VariantPattern*>(&pattern);
    if (!variant || !variant->qualifier) {
        throw IRCompileError("unsupported pattern node");
    }

    const IRRegister tag = ir_.emitVariantTag(
        value, resolvedNames_->patternEnumName(*variant), variant->name.lexeme);
    failJumps.push_back(ir_.emitJumpIfFalse(tag));
    const std::vector<std::size_t>& payloadIndices = resolvedNames_->patternPayloadIndices(*variant);
    for (std::size_t i = 0; i < variant->arguments.size(); ++i) {
        const IRRegister field = ir_.emitVariantField(value, payloadIndices[i]);
        compilePattern(*variant->arguments[i], field, failJumps, bindings);
    }
}

IRRegister IRCompiler::compileExpression(const Expr& expression)
{
    SpanScope scope(*this, expression.span);
    if (const auto* literal = dynamic_cast<const LiteralExpr*>(&expression)) {
        return ir_.emitConstant(literalValue(literal->value));
    }

    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
        return ir_.emitLoadVar(resolvedNames_->variableName(*variable));
    }

    if (const auto* assign = dynamic_cast<const AssignExpr*>(&expression)) {
        const IRRegister value = compileExpression(*assign->value);
        ir_.emitAssignVar(resolvedNames_->assignmentName(*assign), value);
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
        return emitUnary(unary->op.type, value);
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        const IRRegister left = compileExpression(*binary->left);
        const IRRegister right = compileExpression(*binary->right);
        return emitBinary(binary->op.type, left, right);
    }

    if (const auto* logical = dynamic_cast<const LogicalExpr*>(&expression)) {
        return emitLogical(*logical);
    }

    if (const auto* match = dynamic_cast<const MatchExpr*>(&expression)) {
        return compileMatchExpression(*match);
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
    std::vector<std::string> parameters = resolvedNames_->parameterNames(expression);
    ir_.beginFunction(resolvedNames_->functionName(expression), std::move(parameters));

    std::vector<LoopContext> enclosingLoopContexts = std::move(loopContexts_);
    loopContexts_.clear();
    for (const auto& statement : expression.body) {
        compileStatement(*statement);
    }
    IRRegister nilValue = ir_.emitConstant(Value::nil());
    ir_.emitReturn(nilValue);
    loopContexts_ = std::move(enclosingLoopContexts);

    const std::size_t functionIndex = ir_.endFunction();
    return ir_.emitMakeFunction(functionIndex);
}

bool IRCompiler::isBuiltinLenCall(const CallExpr& expression) const
{
    const auto* variable = dynamic_cast<const VariableExpr*>(expression.callee.get());
    return variable && variable->name.lexeme == "len" && !resolvedNames_->hasVariable(*variable);
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
    return ir_.emitLen(value);
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
    if (resolvedNames_->hasVariantConstructor(expression)) {
        return emitVariantConstructor(expression);
    }

    if (resolvedNames_->hasMemberCallCallee(expression)) {
        const IRRegister callee = ir_.emitLoadVar(resolvedNames_->memberCallCalleeName(expression));
        std::vector<IRRegister> arguments;
        if (resolvedNames_->memberCallPassesReceiver(expression)) {
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
        return ir_.emitLen(receiver);
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
    std::vector<IRRegister> payload;
    payload.reserve(expression.arguments.size());
    for (const auto& argument : expression.arguments) {
        payload.push_back(compileExpression(*argument));
    }
    return ir_.emitVariant(
        resolvedNames_->variantEnumName(expression),
        resolvedNames_->variantName(expression),
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

IRRegister IRCompiler::emitStructFields(const std::vector<StructField>& fields, std::optional<std::string> typeName)
{
    std::vector<std::size_t> names;
    std::vector<IRRegister> values;
    names.reserve(fields.size());
    values.reserve(fields.size());

    std::optional<std::size_t> typeNameOperand;
    if (typeName) {
        typeNameOperand = ir_.addName(std::move(*typeName));
    }

    for (const StructField& field : fields) {
        names.push_back(ir_.addName(field.name.lexeme));
        values.push_back(compileExpression(*field.value));
    }
    return ir_.emitStruct(std::move(names), std::move(values), typeNameOperand);
}

IRRegister IRCompiler::emitStructConstructor(const StructConstructExpr& expression)
{
    const TypeInfo& type = typedExpressionType(
        expression, StaticType::Struct, "struct constructor");
    if (!type.structName) {
        throw IRCompileError("typed struct constructor is missing a type name");
    }
    return emitStructFields(expression.fields, *type.structName);
}

IRRegister IRCompiler::emitIndex(const IndexExpr& expression)
{
    IRRegister collection = compileExpression(*expression.collection);
    IRRegister index = compileExpression(*expression.index);
    return ir_.emitIndex(collection, index);
}

IROp IRCompiler::compoundAssignmentOp(TokenType op) const
{
    switch (op) {
    case TokenType::PlusEqual:
        return IROp::Add;
    case TokenType::MinusEqual:
        return IROp::Subtract;
    case TokenType::StarEqual:
        return IROp::Multiply;
    case TokenType::SlashEqual:
        return IROp::Divide;
    default:
        throw IRCompileError("unsupported compound assignment operator: " + tokenTypeName(op));
    }
}

IRRegister IRCompiler::emitCompoundAssign(const CompoundAssignExpr& expression)
{
    const std::string& name = resolvedNames_->compoundAssignmentName(expression);
    const IRRegister oldValue = ir_.emitLoadVar(name);
    const IRRegister result = emitCompoundAssignmentResult(
        expression.op, oldValue, *expression.value, "`" + expression.op.lexeme + "` expects number variable");
    ir_.emitAssignVar(name, result);
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
    IRRegister collection = compileExpression(*expression.collection);
    IRRegister index = compileExpression(*expression.index);
    IRRegister value = compileExpression(*expression.value);
    return ir_.emitAssignIndex(collection, index, value);
}

IRRegister IRCompiler::emitIndexCompoundAssign(const IndexCompoundAssignExpr& expression)
{
    IRRegister collection = compileExpression(*expression.collection);
    IRRegister index = compileExpression(*expression.index);
    IRRegister oldValue = ir_.emitIndex(collection, index);
    IRRegister result = emitCompoundAssignmentResult(
        expression.op, oldValue, *expression.value, "`" + expression.op.lexeme + "` expects number target");
    ir_.emitAssignIndex(collection, index, result);
    return result;
}

IRRegister IRCompiler::emitFieldAccess(const FieldAccessExpr& expression)
{
    if (resolvedNames_->hasFieldAccess(expression)) {
        return ir_.emitLoadVar(resolvedNames_->fieldAccessName(expression));
    }
    IRRegister object = compileExpression(*expression.object);
    return ir_.emitField(object, expression.name.lexeme);
}

IRRegister IRCompiler::emitFieldAssign(const FieldAssignExpr& expression)
{
    typedExpressionType(expression, "field assignment");
    IRRegister object = compileExpression(*expression.object);
    IRRegister value = compileExpression(*expression.value);
    return ir_.emitAssignField(object, expression.name.lexeme, value);
}

IRRegister IRCompiler::emitFieldCompoundAssign(const FieldCompoundAssignExpr& expression)
{
    typedExpressionType(expression, StaticType::Number, "field compound assignment");
    IRRegister object = compileExpression(*expression.object);
    IRRegister oldValue = ir_.emitField(object, expression.name.lexeme);
    IRRegister result = emitCompoundAssignmentResult(
        expression.op, oldValue, *expression.value, "`" + expression.op.lexeme + "` expects number target");
    ir_.emitAssignField(object, expression.name.lexeme, result);
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

IRRegister IRCompiler::emitBinary(TokenType op, IRRegister left, IRRegister right)
{
    switch (op) {
    case TokenType::Plus:
        return ir_.emitBinary(IROp::Add, left, right);
    case TokenType::Minus:
        return ir_.emitBinary(IROp::Subtract, left, right);
    case TokenType::Star:
        return ir_.emitBinary(IROp::Multiply, left, right);
    case TokenType::Slash:
        return ir_.emitBinary(IROp::Divide, left, right);
    case TokenType::EqualEqual:
        return ir_.emitBinary(IROp::Equal, left, right);
    case TokenType::BangEqual:
        return ir_.emitBinary(IROp::NotEqual, left, right);
    case TokenType::Greater:
        return ir_.emitBinary(IROp::Greater, left, right);
    case TokenType::GreaterEqual:
        return ir_.emitBinary(IROp::GreaterEqual, left, right);
    case TokenType::Less:
        return ir_.emitBinary(IROp::Less, left, right);
    case TokenType::LessEqual:
        return ir_.emitBinary(IROp::LessEqual, left, right);
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
