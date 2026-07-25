#include "DeclarationIndex.hpp"

#include "NativeStdlib.hpp"
#include "TypeChecker.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {

bool sameRange(const std::optional<SourceRange>& left, const std::optional<SourceRange>& right)
{
    if (!left || !right) {
        return !left && !right;
    }
    return left->source == right->source
        && left->start == right->start
        && left->end == right->end;
}

std::optional<SourceRange> tokenRange(const Token& token)
{
    return token.range;
}

} // namespace

std::string declarationKindName(DeclarationKind kind)
{
    switch (kind) {
    case DeclarationKind::Module:
        return "module";
    case DeclarationKind::Variable:
        return "variable";
    case DeclarationKind::Function:
        return "function";
    case DeclarationKind::Parameter:
        return "parameter";
    case DeclarationKind::ForInVariable:
        return "for-in variable";
    case DeclarationKind::Struct:
        return "struct";
    case DeclarationKind::Enum:
        return "enum";
    case DeclarationKind::Method:
        return "method";
    case DeclarationKind::NamespaceAlias:
        return "namespace alias";
    }
    return "unknown";
}

class DeclarationIndexCollector {
public:
    explicit DeclarationIndexCollector(DeclarationIndex& index)
        : index_(index)
    {
    }

    void collect(const Program& program)
    {
        bool hasModules = false;
        for (const StmtPtr& statement : program.statements) {
            if (dynamic_cast<const ModuleStmt*>(statement.get())) {
                hasModules = true;
                break;
            }
        }

        if (hasModules) {
            for (const StmtPtr& statement : program.statements) {
                if (const auto* module = dynamic_cast<const ModuleStmt*>(statement.get())) {
                    collectModule(*module);
                }
            }
            return;
        }

        beginScope(nullptr);
        collectStatementList(program.statements);
        endScope();
    }

private:
    ScopeRecord& currentScope()
    {
        if (scopeStack_.empty()) {
            throw std::logic_error("declaration collector scope stack is empty");
        }
        return index_.scopes_.at(scopeStack_.back().value);
    }

    const ScopeRecord& currentScope() const
    {
        if (scopeStack_.empty()) {
            throw std::logic_error("declaration collector scope stack is empty");
        }
        return index_.scopes_.at(scopeStack_.back().value);
    }

    void beginScope(const Stmt* owner)
    {
        ScopeRecord scope;
        scope.id = ScopeId{index_.scopes_.size()};
        if (!scopeStack_.empty()) {
            scope.parent = scopeStack_.back();
        }
        if (owner) {
            scope.ownerSyntaxNode = owner->syntaxNodeId;
            index_.statementScopes_.emplace(owner, scope.id);
        }
        index_.scopes_.push_back(std::move(scope));
        scopeStack_.push_back(index_.scopes_.back().id);
    }

    void endScope()
    {
        if (scopeStack_.empty()) {
            throw std::logic_error("declaration collector scope stack is empty");
        }
        scopeStack_.pop_back();
    }

    struct LoopContext {
        const Stmt* statement = nullptr;
        LoopTargetKind kind = LoopTargetKind::While;
    };

    struct FunctionContext {
        ScopeId scopeId;
        const FunctionStmt* statement = nullptr;
        const FunctionExpr* expression = nullptr;
        const MethodDecl* method = nullptr;
        CaptureRecord captures;
        std::vector<LoopContext> enclosingLoops;
    };

    void beginFunctionContext(
        ScopeId scopeId,
        const FunctionStmt* statement = nullptr,
        const FunctionExpr* expression = nullptr,
        const MethodDecl* method = nullptr)
    {
        FunctionContext context;
        context.scopeId = scopeId;
        context.statement = statement;
        context.expression = expression;
        context.method = method;
        context.enclosingLoops = std::move(loopStack_);
        loopStack_.clear();
        functionStack_.push_back(std::move(context));
    }

    void endFunctionContext()
    {
        if (functionStack_.empty()) {
            throw std::logic_error("declaration collector function stack is empty");
        }
        FunctionContext context = std::move(functionStack_.back());
        functionStack_.pop_back();
        if (context.statement) {
            index_.functionCaptures_.emplace(context.statement, std::move(context.captures));
        } else if (context.expression) {
            index_.functionExpressionCaptures_.emplace(
                context.expression,
                std::move(context.captures));
        } else if (context.method) {
            index_.methodCaptures_.emplace(context.method, std::move(context.captures));
        } else {
            throw std::logic_error("declaration collector function context has no owner");
        }
        loopStack_ = std::move(context.enclosingLoops);
    }

    DeclarationRecord& addDeclaration(
        DeclarationKind kind,
        std::string name,
        std::optional<SourceRange> range,
        std::optional<SyntaxNodeId> syntaxNodeId,
        const Stmt* statement = nullptr,
        const MethodDecl* method = nullptr,
        const Parameter* parameter = nullptr,
        std::string ownerType = {},
        std::vector<TypeParameter> typeParameters = {},
        std::vector<Parameter> parameters = {},
        std::optional<TypeAnnotation> returnType = {},
        bool addToScope = true)
    {
        DeclarationRecord record;
        record.declarationId = DeclarationId{index_.declarations_.size()};
        record.symbolId = SymbolId{index_.declarations_.size()};
        record.kind = kind;
        record.name = std::move(name);
        record.scopeId = currentScope().id;
        record.range = std::move(range);
        record.syntaxNodeId = syntaxNodeId;
        record.statement = statement;
        record.method = method;
        record.parameter = parameter;
        record.ownerType = std::move(ownerType);
        record.typeParameters = std::move(typeParameters);
        record.parameters = std::move(parameters);
        record.returnType = std::move(returnType);

        index_.declarations_.push_back(std::move(record));
        DeclarationRecord& stored = index_.declarations_.back();
        if (statement) {
            index_.statementDeclarations_.emplace(statement, stored.declarationId);
        }
        if (method) {
            index_.methodDeclarations_.emplace(method, stored.declarationId);
        }
        if (parameter) {
            index_.parameterDeclarations_.emplace(parameter, stored.declarationId);
        }
        if (addToScope && !stored.name.empty()) {
            currentScope().declarations[stored.name] = stored.declarationId;
        }
        return stored;
    }

    bool hasStatementDeclaration(const Stmt& statement) const
    {
        return index_.statementDeclarations_.find(&statement)
            != index_.statementDeclarations_.end();
    }

    void predeclareTypes(const std::vector<StmtPtr>& statements)
    {
        for (const StmtPtr& statement : statements) {
            if (const auto* structDecl = dynamic_cast<const StructDeclStmt*>(statement.get())) {
                if (!hasStatementDeclaration(*structDecl)) {
                    addDeclaration(
                        DeclarationKind::Struct,
                        structDecl->name.lexeme,
                        tokenRange(structDecl->name),
                        structDecl->syntaxNodeId,
                        structDecl,
                        nullptr,
                        nullptr,
                        {},
                        structDecl->typeParameters);
                }
            } else if (const auto* enumDecl = dynamic_cast<const EnumDeclStmt*>(statement.get())) {
                if (!hasStatementDeclaration(*enumDecl)) {
                    addDeclaration(
                        DeclarationKind::Enum,
                        enumDecl->name.lexeme,
                        tokenRange(enumDecl->name),
                        enumDecl->syntaxNodeId,
                        enumDecl,
                        nullptr,
                        nullptr,
                        {},
                        enumDecl->typeParameters);
                }
            }
        }
    }

    void collectModule(const ModuleStmt& module)
    {
        beginScope(&module);
        addDeclaration(
            DeclarationKind::Module,
            module.path,
            SourceRange{module.sourceId, 0, module.source.size()},
            module.syntaxNodeId,
            &module,
            nullptr,
            nullptr,
            {},
            {},
            {},
            {},
            false);
        collectStatementList(module.statements);
        endScope();
    }

    void collectStatementList(const std::vector<StmtPtr>& statements)
    {
        predeclareTypes(statements);
        for (const StmtPtr& statement : statements) {
            if (statement) {
                collectStatement(*statement);
            }
        }
    }

    void collectStatement(const Stmt* statement)
    {
        if (statement) {
            collectStatement(*statement);
        }
    }

    void collectStatement(const Stmt& statement)
    {
        if (const auto* module = dynamic_cast<const ModuleStmt*>(&statement)) {
            collectModule(*module);
            return;
        }
        if (dynamic_cast<const StructDeclStmt*>(&statement)) {
            return;
        }
        if (dynamic_cast<const EnumDeclStmt*>(&statement)) {
            return;
        }
        if (const auto* function = dynamic_cast<const FunctionStmt*>(&statement)) {
            if (!hasStatementDeclaration(*function)) {
                addDeclaration(
                    DeclarationKind::Function,
                    function->name.lexeme,
                    tokenRange(function->name),
                    function->syntaxNodeId,
                    function,
                    nullptr,
                    nullptr,
                    {},
                    function->typeParameters,
                    function->parameters,
                    function->returnTypeName);
            }
            beginScope(function);
            beginFunctionContext(scopeStack_.back(), function);
            for (const Parameter& parameter : function->parameters) {
                addDeclaration(
                    DeclarationKind::Parameter,
                    parameter.name.lexeme,
                    tokenRange(parameter.name),
                    std::nullopt,
                    nullptr,
                    nullptr,
                    &parameter,
                    {},
                    {},
                    {},
                    parameter.typeName);
            }
            collectStatementList(function->body);
            endFunctionContext();
            endScope();
            return;
        }
        if (const auto* let = dynamic_cast<const LetStmt*>(&statement)) {
            collectExpression(let->initializer.get());
            addDeclaration(
                DeclarationKind::Variable,
                let->name.lexeme,
                tokenRange(let->name),
                let->syntaxNodeId,
                let);
            return;
        }
        if (const auto* import = dynamic_cast<const ImportStmt*>(&statement)) {
            index_.imports_.push_back(ImportRecord{
                import,
                import->resolvedModuleId,
                import->alias ? std::optional<std::string>(import->alias->lexeme) : std::nullopt});
            if (import->alias) {
                addDeclaration(
                    DeclarationKind::NamespaceAlias,
                    import->alias->lexeme,
                    tokenRange(*import->alias),
                    import->syntaxNodeId,
                    import);
            }
            return;
        }
        if (const auto* exportStmt = dynamic_cast<const ExportStmt*>(&statement)) {
            std::vector<std::string> names;
            for (const Token& name : exportStmt->names) {
                names.push_back(name.lexeme);
            }
            index_.exports_.push_back(ExportRecord{
                exportStmt,
                std::move(names),
                exportStmt->resolvedModuleId,
                exportStmt->sourcePath
                    ? std::optional<std::string>(exportStmt->sourcePath->lexeme)
                    : std::nullopt});
            return;
        }
        if (const auto* impl = dynamic_cast<const ImplStmt*>(&statement)) {
            for (const MethodDecl& method : impl->methods) {
                collectMethod(*impl, method);
            }
            return;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
            beginScope(block);
            collectStatementList(block->statements);
            endScope();
            return;
        }
        if (const auto* print = dynamic_cast<const PrintStmt*>(&statement)) {
            collectExpression(print->expression.get());
            return;
        }
        if (const auto* expression = dynamic_cast<const ExpressionStmt*>(&statement)) {
            collectExpression(expression->expression.get());
            return;
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
            collectExpression(ifStmt->condition.get());
            collectStatement(ifStmt->thenBranch.get());
            collectStatement(ifStmt->elseBranch.get());
            return;
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
            collectExpression(whileStmt->condition.get());
            loopStack_.push_back(LoopContext{whileStmt, LoopTargetKind::While});
            collectStatement(whileStmt->body.get());
            loopStack_.pop_back();
            return;
        }
        if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
            beginScope(forStmt);
            collectStatement(forStmt->initializer.get());
            collectExpression(forStmt->condition.get());
            collectExpression(forStmt->increment.get());
            loopStack_.push_back(LoopContext{forStmt, LoopTargetKind::For});
            collectStatement(forStmt->body.get());
            loopStack_.pop_back();
            endScope();
            return;
        }
        if (const auto* forIn = dynamic_cast<const ForInStmt*>(&statement)) {
            collectExpression(forIn->iterable.get());
            beginScope(forIn);
            addDeclaration(
                DeclarationKind::ForInVariable,
                forIn->variable.lexeme,
                tokenRange(forIn->variable),
                forIn->syntaxNodeId,
                forIn);
            if (const auto* body = dynamic_cast<const BlockStmt*>(forIn->body.get())) {
                index_.statementScopes_.emplace(body, currentScope().id);
                loopStack_.push_back(LoopContext{forIn, LoopTargetKind::ForIn});
                collectStatementList(body->statements);
                loopStack_.pop_back();
            } else {
                loopStack_.push_back(LoopContext{forIn, LoopTargetKind::ForIn});
                collectStatement(forIn->body.get());
                loopStack_.pop_back();
            }
            endScope();
            return;
        }
        if (const auto* match = dynamic_cast<const MatchStmt*>(&statement)) {
            index_.matchStatementNodes_.insert(match);
            collectExpression(match->value.get());
            for (const MatchArm& arm : match->arms) {
                beginScope(nullptr);
                std::unordered_map<std::string, DeclarationId> bindings;
                collectPattern(arm.pattern.get(), bindings);
                if (arm.guard) {
                    index_.patternGuardNodes_.insert(arm.guard.get());
                }
                collectExpression(arm.guard.get());
                collectStatement(arm.body.get());
                endScope();
            }
            return;
        }
        if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&statement)) {
            collectExpression(returnStmt->value.get());
            index_.returnStatements_.insert(returnStmt);
            return;
        }
        if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&statement)) {
            index_.breakStatements_.insert(breakStmt);
            if (!loopStack_.empty()) {
                const LoopContext& loop = loopStack_.back();
                index_.breakTargets_.emplace(
                    breakStmt,
                    LoopTargetRecord{loop.statement, loop.kind});
            }
            return;
        }
        if (const auto* continueStmt = dynamic_cast<const ContinueStmt*>(&statement)) {
            index_.continueStatements_.insert(continueStmt);
            if (!loopStack_.empty()) {
                const LoopContext& loop = loopStack_.back();
                index_.continueTargets_.emplace(
                    continueStmt,
                    LoopTargetRecord{loop.statement, loop.kind});
            }
            return;
        }
    }

    void collectMethod(const ImplStmt& impl, const MethodDecl& method)
    {
        addDeclaration(
            DeclarationKind::Method,
            method.name.lexeme,
            tokenRange(method.name),
            method.syntaxNodeId,
            nullptr,
            &method,
            nullptr,
            impl.typeName.lexeme,
            method.typeParameters,
            method.parameters,
            method.returnTypeName,
            false);
        beginScope(nullptr);
        addDeclaration(
            DeclarationKind::Parameter,
            "this",
            std::nullopt,
            std::nullopt,
            nullptr,
            nullptr,
            nullptr,
            {},
            {},
            {},
            {},
            true);
        beginFunctionContext(scopeStack_.back(), nullptr, nullptr, &method);
        for (const Parameter& parameter : method.parameters) {
            addDeclaration(
                DeclarationKind::Parameter,
                parameter.name.lexeme,
                tokenRange(parameter.name),
                std::nullopt,
                nullptr,
                nullptr,
                &parameter,
                {},
                {},
                {},
                parameter.typeName);
        }
        collectStatementList(method.body);
        endFunctionContext();
        endScope();
    }

    void collectPattern(
        const Pattern* pattern,
        std::unordered_map<std::string, DeclarationId>& bindings)
    {
        if (!pattern) {
            return;
        }
        if (const auto* literal = dynamic_cast<const LiteralPattern*>(pattern)) {
            index_.literalPatternNodes_.insert(literal);
            return;
        }
        if (const auto* variable = dynamic_cast<const VariablePattern*>(pattern)) {
            DeclarationId declarationId;
            const auto found = bindings.find(variable->name.lexeme);
            if (found == bindings.end()) {
                DeclarationRecord& record = addDeclaration(
                    DeclarationKind::Variable,
                    variable->name.lexeme,
                    tokenRange(variable->name),
                    variable->syntaxNodeId,
                    nullptr);
                declarationId = record.declarationId;
                bindings.emplace(variable->name.lexeme, declarationId);
            } else {
                declarationId = found->second;
            }
            index_.patternDeclarations_.emplace(variable, declarationId);
            return;
        }
        if (const auto* orPattern = dynamic_cast<const OrPattern*>(pattern)) {
            index_.orPatternNodes_.insert(orPattern);
            for (const PatternPtr& alternative : orPattern->alternatives) {
                collectPattern(alternative.get(), bindings);
            }
            return;
        }
        if (const auto* record = dynamic_cast<const RecordPattern*>(pattern)) {
            index_.recordPatternNodes_.insert(record);
            for (const RecordPatternField& field : record->fields) {
                collectPattern(field.pattern.get(), bindings);
            }
            return;
        }
        if (const auto* variant = dynamic_cast<const VariantPattern*>(pattern)) {
            index_.variantPatternNodes_.insert(variant);
            for (const PatternPtr& argument : variant->arguments) {
                collectPattern(argument.get(), bindings);
            }
        }
    }

    void collectExpression(const Expr* expression)
    {
        if (!expression) {
            return;
        }
        if (const auto* assign = dynamic_cast<const AssignExpr*>(expression)) {
            collectExpression(assign->value.get());
            recordAssignmentReference(*assign);
            return;
        }
        if (const auto* compound = dynamic_cast<const CompoundAssignExpr*>(expression)) {
            collectExpression(compound->value.get());
            recordCompoundAssignmentReference(*compound);
            return;
        }
        if (const auto* indexAssign = dynamic_cast<const IndexAssignExpr*>(expression)) {
            collectExpression(indexAssign->collection.get());
            collectExpression(indexAssign->index.get());
            collectExpression(indexAssign->value.get());
            index_.indexAssignments_.insert(indexAssign);
            return;
        }
        if (const auto* indexCompound = dynamic_cast<const IndexCompoundAssignExpr*>(expression)) {
            collectExpression(indexCompound->collection.get());
            collectExpression(indexCompound->index.get());
            collectExpression(indexCompound->value.get());
            index_.indexCompoundAssignments_.insert(indexCompound);
            return;
        }
        if (const auto* variable = dynamic_cast<const VariableExpr*>(expression)) {
            recordVariableReference(*variable);
            return;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression)) {
            collectExpression(unary->right.get());
            return;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression)) {
            collectExpression(binary->left.get());
            collectExpression(binary->right.get());
            return;
        }
        if (const auto* logical = dynamic_cast<const LogicalExpr*>(expression)) {
            collectExpression(logical->left.get());
            collectExpression(logical->right.get());
            return;
        }
        if (const auto* grouping = dynamic_cast<const GroupingExpr*>(expression)) {
            collectExpression(grouping->expression.get());
            return;
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
            collectExpression(call->callee.get());
            for (const ExprPtr& argument : call->arguments) {
                collectExpression(argument.get());
            }
            if (const VariableExpr* callee = directCallee(call->callee.get())) {
                index_.directCallCallees_.emplace(call, callee);
                if (isNativeStdlibName(callee->name.lexeme)) {
                    index_.nativeCallCandidates_.emplace(call, callee->name.lexeme);
                }
                if (const std::optional<ResolvedSymbol> resolved = lookupReference(callee->name.lexeme)) {
                    const DeclarationRecord* target = index_.declaration(resolved->declarationId);
                    if (target && isValueDeclaration(target->kind)) {
                        index_.callTargets_.emplace(
                            call,
                            CallTargetRecord{CallTargetKind::Direct, *resolved});
                    }
                }
            }
            return;
        }
        if (const auto* memberCall = dynamic_cast<const MemberCallExpr*>(expression)) {
            collectExpression(memberCall->receiver.get());
            for (const ExprPtr& argument : memberCall->arguments) {
                collectExpression(argument.get());
            }
            index_.memberCallCandidates_.emplace(
                memberCall, memberCall->name.lexeme);
            if (isNativeStdlibName(memberCall->name.lexeme)) {
                index_.nativeCallCandidates_.emplace(memberCall, memberCall->name.lexeme);
            }
            return;
        }
        if (const auto* array = dynamic_cast<const ArrayExpr*>(expression)) {
            for (const ExprPtr& element : array->elements) {
                collectExpression(element.get());
            }
            index_.arrayExpressions_.insert(array);
            return;
        }
        if (const auto* map = dynamic_cast<const MapExpr*>(expression)) {
            for (const MapEntry& entry : map->entries) {
                collectExpression(entry.key.get());
                collectExpression(entry.value.get());
            }
            index_.mapExpressions_.insert(map);
            return;
        }
        if (const auto* construct = dynamic_cast<const StructConstructExpr*>(expression)) {
            for (const StructField& field : construct->fields) {
                collectExpression(field.value.get());
            }
            index_.structConstructors_.insert(construct);
            return;
        }
        if (const auto* index = dynamic_cast<const IndexExpr*>(expression)) {
            collectExpression(index->collection.get());
            collectExpression(index->index.get());
            index_.indexExpressions_.insert(index);
            return;
        }
        if (const auto* field = dynamic_cast<const FieldAccessExpr*>(expression)) {
            collectExpression(field->object.get());
            index_.fieldAccesses_.insert(field);
            return;
        }
        if (const auto* fieldAssign = dynamic_cast<const FieldAssignExpr*>(expression)) {
            collectExpression(fieldAssign->object.get());
            collectExpression(fieldAssign->value.get());
            index_.fieldAssignments_.insert(fieldAssign);
            return;
        }
        if (const auto* fieldCompound = dynamic_cast<const FieldCompoundAssignExpr*>(expression)) {
            collectExpression(fieldCompound->object.get());
            collectExpression(fieldCompound->value.get());
            index_.fieldCompoundAssignments_.insert(fieldCompound);
            return;
        }
        if (const auto* function = dynamic_cast<const FunctionExpr*>(expression)) {
            index_.functionExpressions_.insert(function);
            beginScope(nullptr);
            beginFunctionContext(scopeStack_.back(), nullptr, function);
            for (const Parameter& parameter : function->parameters) {
                addDeclaration(
                    DeclarationKind::Parameter,
                    parameter.name.lexeme,
                    tokenRange(parameter.name),
                    std::nullopt,
                    nullptr,
                    nullptr,
                    &parameter,
                    {},
                    {},
                    {},
                    parameter.typeName);
            }
            collectStatementList(function->body);
            endFunctionContext();
            endScope();
            return;
        }
        if (const auto* match = dynamic_cast<const MatchExpr*>(expression)) {
            index_.matchExpressionNodes_.insert(match);
            collectExpression(match->value.get());
            for (const MatchExprArm& arm : match->arms) {
                beginScope(nullptr);
                std::unordered_map<std::string, DeclarationId> bindings;
                collectPattern(arm.pattern.get(), bindings);
                if (arm.guard) {
                    index_.patternGuardNodes_.insert(arm.guard.get());
                }
                collectExpression(arm.guard.get());
                collectExpression(arm.value.get());
                endScope();
            }
        }
    }

    const VariableExpr* directCallee(const Expr* expression) const
    {
        if (!expression) {
            return nullptr;
        }
        if (const auto* grouping = dynamic_cast<const GroupingExpr*>(expression)) {
            return directCallee(grouping->expression.get());
        }
        return dynamic_cast<const VariableExpr*>(expression);
    }

    std::optional<ResolvedSymbol> lookupReference(const std::string& name) const
    {
        for (auto scope = scopeStack_.rbegin(); scope != scopeStack_.rend(); ++scope) {
            const ScopeRecord& record = index_.scopes_.at(scope->value);
            const auto found = record.declarations.find(name);
            if (found != record.declarations.end()) {
                const DeclarationRecord& declaration = index_.declarations_.at(found->second.value);
                return ResolvedSymbol{declaration.declarationId, declaration.symbolId};
            }
        }
        return std::nullopt;
    }

    static bool isValueDeclaration(DeclarationKind kind)
    {
        return kind == DeclarationKind::Variable
            || kind == DeclarationKind::Function
            || kind == DeclarationKind::Parameter
            || kind == DeclarationKind::ForInVariable;
    }

    bool scopeContains(ScopeId ancestor, ScopeId descendant) const
    {
        std::optional<ScopeId> current = descendant;
        while (current) {
            if (*current == ancestor) {
                return true;
            }
            const ScopeRecord* record = index_.scope(*current);
            if (!record) {
                return false;
            }
            current = record->parent;
        }
        return false;
    }

    void recordCapture(const ResolvedSymbol& resolved)
    {
        if (functionStack_.empty()) {
            return;
        }
        const DeclarationRecord* target = index_.declaration(resolved.declarationId);
        if (!target || !isValueDeclaration(target->kind)) {
            return;
        }

        FunctionContext& current = functionStack_.back();
        if (scopeContains(current.scopeId, target->scopeId)) {
            return;
        }

        bool isEnclosingLocal = false;
        for (std::size_t index = functionStack_.size() - 1; index > 0; --index) {
            const FunctionContext& enclosing = functionStack_[index - 1];
            if (scopeContains(enclosing.scopeId, target->scopeId)) {
                isEnclosingLocal = true;
                break;
            }
        }
        if (!isEnclosingLocal) {
            return;
        }

        const auto duplicate = std::find_if(
            current.captures.symbols.begin(),
            current.captures.symbols.end(),
            [&resolved](const ResolvedSymbol& symbol) {
                return symbol.declarationId == resolved.declarationId;
            });
        if (duplicate == current.captures.symbols.end()) {
            current.captures.symbols.push_back(resolved);
        }
    }

    void recordVariableReference(const VariableExpr& expression)
    {
        if (const std::optional<ResolvedSymbol> resolved = lookupReference(expression.name.lexeme)) {
            const DeclarationRecord* target = index_.declaration(resolved->declarationId);
            if (target && isValueDeclaration(target->kind)) {
                index_.variableReferences_.emplace(&expression, *resolved);
                recordCapture(*resolved);
            }
        }
    }

    void recordAssignmentReference(const AssignExpr& expression)
    {
        if (const std::optional<ResolvedSymbol> resolved = lookupReference(expression.name.lexeme)) {
            const DeclarationRecord* target = index_.declaration(resolved->declarationId);
            if (target && isValueDeclaration(target->kind)) {
                index_.assignmentReferences_.emplace(&expression, *resolved);
                recordCapture(*resolved);
            }
        }
    }

    void recordCompoundAssignmentReference(const CompoundAssignExpr& expression)
    {
        if (const std::optional<ResolvedSymbol> resolved = lookupReference(expression.name.lexeme)) {
            const DeclarationRecord* target = index_.declaration(resolved->declarationId);
            if (target && isValueDeclaration(target->kind)) {
                index_.compoundAssignmentReferences_.emplace(&expression, *resolved);
                recordCapture(*resolved);
            }
        }
    }

    DeclarationIndex& index_;
    std::vector<ScopeId> scopeStack_;
    std::vector<LoopContext> loopStack_;
    std::vector<FunctionContext> functionStack_;
};

DeclarationIndex DeclarationIndex::collect(const Program& program)
{
    DeclarationIndex index;
    DeclarationIndexCollector collector(index);
    collector.collect(program);
    return index;
}

const std::vector<DeclarationRecord>& DeclarationIndex::declarations() const
{
    return declarations_;
}

const std::vector<ScopeRecord>& DeclarationIndex::scopes() const
{
    return scopes_;
}

const std::vector<ImportRecord>& DeclarationIndex::imports() const
{
    return imports_;
}

const std::vector<ExportRecord>& DeclarationIndex::exports() const
{
    return exports_;
}

const DeclarationRecord* DeclarationIndex::declaration(DeclarationId id) const
{
    if (!id.valid() || id.value >= declarations_.size()) {
        return nullptr;
    }
    return &declarations_[id.value];
}

const DeclarationRecord* DeclarationIndex::declaration(const Stmt& statement) const
{
    const auto found = statementDeclarations_.find(&statement);
    return found == statementDeclarations_.end() ? nullptr : declaration(found->second);
}

const DeclarationRecord* DeclarationIndex::declaration(const MethodDecl& method) const
{
    const auto found = methodDeclarations_.find(&method);
    return found == methodDeclarations_.end() ? nullptr : declaration(found->second);
}

const DeclarationRecord* DeclarationIndex::declaration(const Parameter& parameter) const
{
    const auto found = parameterDeclarations_.find(&parameter);
    return found == parameterDeclarations_.end() ? nullptr : declaration(found->second);
}

const DeclarationRecord* DeclarationIndex::declaration(const VariablePattern& pattern) const
{
    const auto found = patternDeclarations_.find(&pattern);
    return found == patternDeclarations_.end() ? nullptr : declaration(found->second);
}

const BindingMetadataRecord* DeclarationIndex::letBindingMetadata(const LetStmt& statement) const
{
    const auto found = letBindingMetadata_.find(&statement);
    return found == letBindingMetadata_.end() ? nullptr : &found->second;
}

const BindingMetadataRecord* DeclarationIndex::variableBindingMetadata(
    const VariableExpr& expression) const
{
    const auto found = variableBindingMetadata_.find(&expression);
    return found == variableBindingMetadata_.end() ? nullptr : &found->second;
}

const BindingMetadataRecord* DeclarationIndex::assignmentBindingMetadata(
    const AssignExpr& expression) const
{
    const auto found = assignmentBindingMetadata_.find(&expression);
    return found == assignmentBindingMetadata_.end() ? nullptr : &found->second;
}

const BindingMetadataRecord* DeclarationIndex::compoundAssignmentBindingMetadata(
    const CompoundAssignExpr& expression) const
{
    const auto found = compoundAssignmentBindingMetadata_.find(&expression);
    return found == compoundAssignmentBindingMetadata_.end() ? nullptr : &found->second;
}

const BindingMetadataRecord* DeclarationIndex::forInBindingMetadata(
    const ForInStmt& statement) const
{
    const auto found = forInBindingMetadata_.find(&statement);
    return found == forInBindingMetadata_.end() ? nullptr : &found->second;
}

const FunctionMetadataRecord* DeclarationIndex::functionMetadata(
    const FunctionStmt& statement) const
{
    const auto found = functionMetadata_.find(&statement);
    return found == functionMetadata_.end() ? nullptr : &found->second;
}

const FunctionMetadataRecord* DeclarationIndex::functionMetadata(
    const FunctionExpr& expression) const
{
    const auto found = functionExpressionMetadata_.find(&expression);
    return found == functionExpressionMetadata_.end() ? nullptr : &found->second;
}

const FunctionMetadataRecord* DeclarationIndex::functionMetadata(const MethodDecl& method) const
{
    const auto found = methodMetadata_.find(&method);
    return found == methodMetadata_.end() ? nullptr : &found->second;
}

const MemberCallMetadataRecord* DeclarationIndex::memberCallMetadata(
    const MemberCallExpr& expression) const
{
    const auto found = memberCallMetadata_.find(&expression);
    return found == memberCallMetadata_.end() ? nullptr : &found->second;
}

std::optional<DeclarationSignature> DeclarationIndex::signature(DeclarationId id) const
{
    const DeclarationRecord* record = declaration(id);
    if (!record
        || (record->kind != DeclarationKind::Function
            && record->kind != DeclarationKind::Method
            && record->kind != DeclarationKind::Struct
            && record->kind != DeclarationKind::Enum)) {
        return std::nullopt;
    }
    return DeclarationSignature{
        record->typeParameters,
        record->parameters,
        record->returnType};
}

const ResolvedSignatureRecord* DeclarationIndex::resolvedSignature(DeclarationId id) const
{
    const auto found = resolvedSignatures_.find(id);
    return found == resolvedSignatures_.end() ? nullptr : &found->second;
}

std::optional<DeclarationShape> DeclarationIndex::shape(DeclarationId id) const
{
    const DeclarationRecord* record = declaration(id);
    if (!record || !record->statement) {
        return std::nullopt;
    }
    if (const auto* structDecl = dynamic_cast<const StructDeclStmt*>(record->statement)) {
        return DeclarationShape{structDecl->fields, {}};
    }
    if (const auto* enumDecl = dynamic_cast<const EnumDeclStmt*>(record->statement)) {
        return DeclarationShape{{}, enumDecl->variants};
    }
    return std::nullopt;
}

const ScopeRecord* DeclarationIndex::scope(ScopeId id) const
{
    if (!id.valid() || id.value >= scopes_.size()) {
        return nullptr;
    }
    return &scopes_[id.value];
}

std::optional<ScopeId> DeclarationIndex::scopeFor(const Stmt& statement) const
{
    const auto found = statementScopes_.find(&statement);
    if (found == statementScopes_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<ResolvedSymbol> DeclarationIndex::forInBinding(const ForInStmt& statement) const
{
    const DeclarationRecord* target = declaration(statement);
    if (!target || target->kind != DeclarationKind::ForInVariable) {
        return std::nullopt;
    }
    return ResolvedSymbol{target->declarationId, target->symbolId};
}

std::optional<ResolvedSymbol> DeclarationIndex::patternBinding(const VariablePattern& pattern) const
{
    const DeclarationRecord* target = declaration(pattern);
    if (!target || target->kind != DeclarationKind::Variable) {
        return std::nullopt;
    }
    return ResolvedSymbol{target->declarationId, target->symbolId};
}

const CallTargetRecord* DeclarationIndex::callTarget(const CallExpr& expression) const
{
    const auto found = callTargets_.find(&expression);
    return found == callTargets_.end() ? nullptr : &found->second;
}

const CallTargetRecord* DeclarationIndex::callTarget(const MemberCallExpr& expression) const
{
    const auto found = memberCallTargets_.find(&expression);
    return found == memberCallTargets_.end() ? nullptr : &found->second;
}

const TypedExpressionRecord* DeclarationIndex::typedExpression(const Expr& expression) const
{
    const auto found = typedExpressions_.find(&expression);
    return found == typedExpressions_.end() ? nullptr : &found->second;
}

void DeclarationIndex::recordTypedExpression(const Expr& expression, TypeInfo type)
{
    typedExpressions_.insert_or_assign(&expression, TypedExpressionRecord{std::move(type)});
}

const NativeCallRecord* DeclarationIndex::nativeCall(const Expr& expression) const
{
    const auto found = nativeCalls_.find(&expression);
    return found == nativeCalls_.end() ? nullptr : &found->second;
}

const VariantConstructorRecord* DeclarationIndex::variantConstructor(
    const MemberCallExpr& expression) const
{
    const auto found = variantConstructors_.find(&expression);
    return found == variantConstructors_.end() ? nullptr : &found->second;
}

const LiteralPatternRecord* DeclarationIndex::literalPattern(const LiteralPattern& pattern) const
{
    const auto found = literalPatterns_.find(&pattern);
    return found == literalPatterns_.end() ? nullptr : &found->second;
}

const VariantPatternRecord* DeclarationIndex::variantPattern(const VariantPattern& pattern) const
{
    const auto found = variantPatterns_.find(&pattern);
    return found == variantPatterns_.end() ? nullptr : &found->second;
}

const RecordPatternRecord* DeclarationIndex::recordPattern(const RecordPattern& pattern) const
{
    const auto found = recordPatterns_.find(&pattern);
    return found == recordPatterns_.end() ? nullptr : &found->second;
}

const PatternBindingRecord* DeclarationIndex::patternBindingMetadata(
    const VariablePattern& pattern) const
{
    const auto found = patternBindingMetadata_.find(&pattern);
    return found == patternBindingMetadata_.end() ? nullptr : &found->second;
}

const OrPatternRecord* DeclarationIndex::orPattern(const OrPattern& pattern) const
{
    const auto found = orPatterns_.find(&pattern);
    return found == orPatterns_.end() ? nullptr : &found->second;
}

const PatternGuardRecord* DeclarationIndex::patternGuard(const Expr& guard) const
{
    const auto found = patternGuards_.find(&guard);
    return found == patternGuards_.end() ? nullptr : &found->second;
}

const MatchCoverageRecord* DeclarationIndex::matchCoverage(const MatchStmt& match) const
{
    const auto found = matchStatementCoverage_.find(&match);
    return found == matchStatementCoverage_.end() ? nullptr : &found->second;
}

const MatchCoverageRecord* DeclarationIndex::matchCoverage(const MatchExpr& match) const
{
    const auto found = matchExpressionCoverage_.find(&match);
    return found == matchExpressionCoverage_.end() ? nullptr : &found->second;
}

const IndexOperationRecord* DeclarationIndex::indexOperation(const Expr& expression) const
{
    const auto found = indexOperations_.find(&expression);
    return found == indexOperations_.end() ? nullptr : &found->second;
}

const FieldOperationRecord* DeclarationIndex::fieldOperation(const Expr& expression) const
{
    const auto found = fieldOperations_.find(&expression);
    return found == fieldOperations_.end() ? nullptr : &found->second;
}

const StructConstructorRecord* DeclarationIndex::structConstructor(
    const StructConstructExpr& expression) const
{
    const auto found = structConstructorsMetadata_.find(&expression);
    return found == structConstructorsMetadata_.end() ? nullptr : &found->second;
}

const ReturnRecord* DeclarationIndex::returnMetadata(const ReturnStmt& statement) const
{
    const auto found = returnMetadata_.find(&statement);
    return found == returnMetadata_.end() ? nullptr : &found->second;
}

const CaptureRecord* DeclarationIndex::captureMetadata(const FunctionStmt& statement) const
{
    const auto found = functionCaptures_.find(&statement);
    return found == functionCaptures_.end() ? nullptr : &found->second;
}

const CaptureRecord* DeclarationIndex::captureMetadata(const FunctionExpr& expression) const
{
    const auto found = functionExpressionCaptures_.find(&expression);
    return found == functionExpressionCaptures_.end() ? nullptr : &found->second;
}

const CaptureRecord* DeclarationIndex::captureMetadata(const MethodDecl& method) const
{
    const auto found = methodCaptures_.find(&method);
    return found == methodCaptures_.end() ? nullptr : &found->second;
}

const LoopTargetRecord* DeclarationIndex::breakTarget(const BreakStmt& statement) const
{
    const auto found = breakTargets_.find(&statement);
    return found == breakTargets_.end() ? nullptr : &found->second;
}

const LoopTargetRecord* DeclarationIndex::continueTarget(const ContinueStmt& statement) const
{
    const auto found = continueTargets_.find(&statement);
    return found == continueTargets_.end() ? nullptr : &found->second;
}

void DeclarationIndex::recordNativeCall(const Expr& expression, std::string name)
{
    nativeCalls_.insert_or_assign(&expression, NativeCallRecord{std::move(name)});
}

void DeclarationIndex::recordVariantConstructor(
    const MemberCallExpr& expression,
    std::string enumName,
    std::string variantName,
    TypeInfo resultType,
    std::vector<TypeInfo> payloadTypes)
{
    variantConstructors_.insert_or_assign(
        &expression,
        VariantConstructorRecord{
            std::move(enumName),
            std::move(variantName),
            std::move(resultType),
            std::move(payloadTypes)});
}

void DeclarationIndex::recordLiteralPattern(const LiteralPattern& pattern, LiteralPatternRecord record)
{
    literalPatterns_.insert_or_assign(&pattern, std::move(record));
}

void DeclarationIndex::recordVariantPattern(const VariantPattern& pattern, VariantPatternRecord record)
{
    variantPatterns_.insert_or_assign(&pattern, std::move(record));
}

void DeclarationIndex::recordRecordPattern(const RecordPattern& pattern, RecordPatternRecord record)
{
    recordPatterns_.insert_or_assign(&pattern, std::move(record));
}

void DeclarationIndex::recordPatternBinding(
    const VariablePattern& pattern,
    PatternBindingRecord record)
{
    patternBindingMetadata_.insert_or_assign(&pattern, std::move(record));
}

void DeclarationIndex::recordOrPattern(const OrPattern& pattern, OrPatternRecord record)
{
    orPatterns_.insert_or_assign(&pattern, std::move(record));
}

void DeclarationIndex::recordPatternGuard(const Expr& guard, PatternGuardRecord record)
{
    patternGuards_.insert_or_assign(&guard, std::move(record));
}

void DeclarationIndex::recordMatchCoverage(const MatchStmt& match, MatchCoverageRecord record)
{
    matchStatementCoverage_.insert_or_assign(&match, std::move(record));
}

void DeclarationIndex::recordMatchCoverage(const MatchExpr& match, MatchCoverageRecord record)
{
    matchExpressionCoverage_.insert_or_assign(&match, std::move(record));
}

void DeclarationIndex::recordIndexOperation(const Expr& expression, IndexOperationRecord record)
{
    indexOperations_.insert_or_assign(&expression, std::move(record));
}

void DeclarationIndex::recordFieldOperation(const Expr& expression, FieldOperationRecord record)
{
    fieldOperations_.insert_or_assign(&expression, std::move(record));
}

void DeclarationIndex::recordStructConstructor(
    const StructConstructExpr& expression,
    StructConstructorRecord record)
{
    structConstructorsMetadata_.insert_or_assign(&expression, std::move(record));
}

void DeclarationIndex::recordLetBinding(const LetStmt& statement, BindingMetadataRecord record)
{
    letBindingMetadata_.insert_or_assign(&statement, std::move(record));
}

void DeclarationIndex::recordVariableBinding(
    const VariableExpr& expression,
    BindingMetadataRecord record)
{
    variableBindingMetadata_.insert_or_assign(&expression, std::move(record));
}

void DeclarationIndex::recordAssignmentBinding(
    const AssignExpr& expression,
    BindingMetadataRecord record)
{
    assignmentBindingMetadata_.insert_or_assign(&expression, std::move(record));
}

void DeclarationIndex::recordCompoundAssignmentBinding(
    const CompoundAssignExpr& expression,
    BindingMetadataRecord record)
{
    compoundAssignmentBindingMetadata_.insert_or_assign(&expression, std::move(record));
}

void DeclarationIndex::recordForInBinding(
    const ForInStmt& statement,
    BindingMetadataRecord record)
{
    forInBindingMetadata_.insert_or_assign(&statement, std::move(record));
}

void DeclarationIndex::recordFunctionMetadata(
    const FunctionStmt& statement,
    FunctionMetadataRecord record)
{
    functionMetadata_.insert_or_assign(&statement, std::move(record));
}

void DeclarationIndex::recordFunctionMetadata(
    const FunctionExpr& expression,
    FunctionMetadataRecord record)
{
    functionExpressionMetadata_.insert_or_assign(&expression, std::move(record));
}

void DeclarationIndex::recordFunctionMetadata(
    const MethodDecl& method,
    FunctionMetadataRecord record)
{
    methodMetadata_.insert_or_assign(&method, std::move(record));
}

void DeclarationIndex::recordMemberCallMetadata(
    const MemberCallExpr& expression,
    MemberCallMetadataRecord record)
{
    memberCallMetadata_.insert_or_assign(&expression, std::move(record));
}

void DeclarationIndex::recordReturn(const ReturnStmt& statement, TypeInfo type)
{
    returnMetadata_.insert_or_assign(&statement, ReturnRecord{std::move(type)});
}

void DeclarationIndex::recordResolvedSignature(DeclarationId id, TypeInfo type)
{
    resolvedSignatures_.insert_or_assign(id, ResolvedSignatureRecord{std::move(type)});
}

std::optional<DeclarationId> DeclarationIndex::lookup(ScopeId scopeId, const std::string& name) const
{
    std::optional<ScopeId> current = scopeId;
    while (current) {
        const ScopeRecord* record = scope(*current);
        if (!record) {
            return std::nullopt;
        }
        const auto found = record->declarations.find(name);
        if (found != record->declarations.end()) {
            return found->second;
        }
        current = record->parent;
    }
    return std::nullopt;
}

std::optional<ResolvedSymbol> DeclarationIndex::variableReference(const VariableExpr& expression) const
{
    const auto found = variableReferences_.find(&expression);
    return found == variableReferences_.end()
        ? std::nullopt
        : std::optional<ResolvedSymbol>(found->second);
}

std::optional<ResolvedSymbol> DeclarationIndex::assignmentReference(const AssignExpr& expression) const
{
    const auto found = assignmentReferences_.find(&expression);
    return found == assignmentReferences_.end()
        ? std::nullopt
        : std::optional<ResolvedSymbol>(found->second);
}

std::optional<ResolvedSymbol> DeclarationIndex::compoundAssignmentReference(
    const CompoundAssignExpr& expression) const
{
    const auto found = compoundAssignmentReferences_.find(&expression);
    return found == compoundAssignmentReferences_.end()
        ? std::nullopt
        : std::optional<ResolvedSymbol>(found->second);
}

std::size_t DeclarationIndex::compareResolvedNames(const ResolvedNames& resolved)
{
    std::size_t mismatches = 0;
    memberCallTargets_.clear();
    const auto requireTypedExpression = [&](const Expr& expression) {
        if (!typedExpression(expression)) {
            ++mismatches;
        }
    };
    const auto bindingMatches = [](const TypeBinding& binding, const DeclarationRecord& target) {
        return binding.resolvedName.substr(0, binding.resolvedName.find('#')) == target.name
            && sameRange(binding.range, target.range);
    };
    const auto patternBindingMatches = [](const PatternBindingRecord& binding, const DeclarationRecord& target) {
        return binding.bindingId.valid()
            && binding.resolvedName.substr(0, binding.resolvedName.find('#')) == target.name
            && sameRange(binding.range, target.range)
            && binding.symbol.declarationId == target.declarationId
            && binding.symbol.symbolId == target.symbolId;
    };
    const auto compareReference = [&](const auto& references, const auto& resolve) {
        for (const auto& entry : references) {
            const DeclarationRecord* target = declaration(entry.second.declarationId);
            if (!target) {
                ++mismatches;
                continue;
            }
            try {
                const BindingId bindingId = resolve(*entry.first);
                const TypeBinding& binding = resolved.binding(bindingId);
                if (!bindingMatches(binding, *target)) {
                    ++mismatches;
                } else {
                    requireTypedExpression(*entry.first);
                }
            } catch (const std::logic_error&) {
                ++mismatches;
            }
        }
    };

    compareReference(
        variableReferences_,
        [&resolved](const VariableExpr& expression) {
            return resolved.variableBindingId(expression);
        });
    compareReference(
        assignmentReferences_,
        [&resolved](const AssignExpr& expression) {
            return resolved.assignmentBindingId(expression);
        });
    compareReference(
        compoundAssignmentReferences_,
        [&resolved](const CompoundAssignExpr& expression) {
            return resolved.compoundAssignmentBindingId(expression);
        });

    const auto compareBindingMetadata = [&](const auto& records, const auto& resolve) {
        for (const auto& entry : records) {
            try {
                const BindingId bindingId = resolve(*entry.first);
                const TypeBinding& binding = resolved.binding(bindingId);
                const BindingMetadataRecord& metadata = entry.second;
                if (metadata.bindingId != bindingId
                    || metadata.resolvedName != binding.resolvedName
                    || metadata.symbol.declarationId != binding.declarationId
                    || metadata.symbol.symbolId != binding.symbolId) {
                    ++mismatches;
                }
            } catch (const std::logic_error&) {
                ++mismatches;
            }
        }
    };

    compareBindingMetadata(
        letBindingMetadata_,
        [&resolved](const LetStmt& statement) {
            return resolved.letBindingId(statement);
        });
    compareBindingMetadata(
        variableBindingMetadata_,
        [&resolved](const VariableExpr& expression) {
            return resolved.variableBindingId(expression);
        });
    compareBindingMetadata(
        assignmentBindingMetadata_,
        [&resolved](const AssignExpr& expression) {
            return resolved.assignmentBindingId(expression);
        });
    compareBindingMetadata(
        compoundAssignmentBindingMetadata_,
        [&resolved](const CompoundAssignExpr& expression) {
            return resolved.compoundAssignmentBindingId(expression);
        });
    compareBindingMetadata(
        forInBindingMetadata_,
        [&resolved](const ForInStmt& statement) {
            return resolved.forInBindingId(statement);
        });

    for (const DeclarationRecord& record : declarations_) {
        if (record.kind == DeclarationKind::Variable
            && record.statement
            && dynamic_cast<const LetStmt*>(record.statement)) {
            try {
                const BindingId bindingId = resolved.letBindingId(
                    *static_cast<const LetStmt*>(record.statement));
                const TypeBinding& binding = resolved.binding(bindingId);
                if (!bindingMatches(binding, record)) {
                    ++mismatches;
                }
            } catch (const std::logic_error&) {
                ++mismatches;
            }
        } else if (record.kind == DeclarationKind::Function && record.statement) {
            try {
                if (!resolved.declarationId(*record.statement).valid()) {
                    ++mismatches;
                }
            } catch (const std::logic_error&) {
                ++mismatches;
            }
        } else if ((record.kind == DeclarationKind::Struct
                || record.kind == DeclarationKind::Enum)
            && record.statement) {
            try {
                if (!resolved.declarationId(*record.statement).valid()) {
                    ++mismatches;
                }
            } catch (const std::logic_error&) {
                ++mismatches;
            }
        } else if (record.kind == DeclarationKind::ForInVariable && record.statement) {
            try {
                const auto* forIn = dynamic_cast<const ForInStmt*>(record.statement);
                const BindingId bindingId = forIn
                    ? resolved.forInBindingId(*forIn)
                    : BindingId{};
                if (!forIn
                    || !resolved.declarationId(*forIn).valid()
                    || !bindingMatches(resolved.binding(bindingId), record)) {
                    ++mismatches;
                }
            } catch (const std::logic_error&) {
                ++mismatches;
            }
        } else if (record.kind == DeclarationKind::Method && record.method) {
            try {
                if (!resolved.methodDeclarationId(*record.method).valid()) {
                    ++mismatches;
                }
            } catch (const std::logic_error&) {
                ++mismatches;
            }
        }
    }

    for (const auto& entry : functionMetadata_) {
        const FunctionMetadataRecord& metadata = entry.second;
        try {
            if (metadata.resolvedName != resolved.functionName(*entry.first)
                || metadata.parameterNames != resolved.parameterNames(*entry.first)
                || metadata.functionLabel != entry.first->name.lexeme) {
                ++mismatches;
            }
        } catch (const std::logic_error&) {
            ++mismatches;
        }
    }
    for (const auto& entry : functionExpressionMetadata_) {
        const FunctionMetadataRecord& metadata = entry.second;
        try {
            if (metadata.resolvedName != resolved.functionName(*entry.first)
                || metadata.parameterNames != resolved.parameterNames(*entry.first)
                || metadata.functionLabel != "<lambda>") {
                ++mismatches;
            }
        } catch (const std::logic_error&) {
            ++mismatches;
        }
    }
    for (const auto& entry : methodMetadata_) {
        const FunctionMetadataRecord& metadata = entry.second;
        try {
            if (metadata.resolvedName != resolved.methodName(*entry.first)
                || metadata.parameterNames != resolved.methodParameterNames(*entry.first)
                || metadata.functionLabel != entry.first->name.lexeme) {
                ++mismatches;
            }
        } catch (const std::logic_error&) {
            ++mismatches;
        }
    }

    for (const auto& entry : patternDeclarations_) {
        const DeclarationRecord* target = declaration(entry.second);
        const PatternBindingRecord* metadata = patternBindingMetadata(*entry.first);
        if (!target || !metadata || !patternBindingMatches(*metadata, *target)) {
            ++mismatches;
        }
    }

    for (const auto& entry : directCallCallees_) {
        const VariableExpr& callee = *entry.second;
        if (!resolved.hasVariable(callee)) {
            continue;
        }
        const auto targetFound = callTargets_.find(entry.first);
        if (targetFound == callTargets_.end()) {
            ++mismatches;
            continue;
        }
        const DeclarationRecord* target = declaration(targetFound->second.target.declarationId);
        try {
            const TypeBinding& binding = resolved.binding(resolved.variableBindingId(callee));
            if (!target || !bindingMatches(binding, *target)) {
                ++mismatches;
            } else {
                requireTypedExpression(*entry.first);
            }
        } catch (const std::logic_error&) {
            ++mismatches;
        }
    }

    for (const auto& entry : memberCallCandidates_) {
        const MemberCallExpr& expression = *entry.first;
        if (resolved.hasVariantConstructor(expression)) {
            const auto variant = variantConstructors_.find(entry.first);
            if (variant == variantConstructors_.end()
                || variant->second.enumName != resolved.variantEnumName(expression)
                || variant->second.variantName != resolved.variantName(expression)) {
                ++mismatches;
            } else {
                requireTypedExpression(expression);
            }
            continue;
        }
        if (!resolved.hasMemberCallCallee(expression)) {
            if (memberCallMetadata(expression)) {
                ++mismatches;
            }
            continue;
        }

        const MemberCallMetadataRecord* metadata = memberCallMetadata(expression);
        try {
            if (!metadata
                || metadata->calleeName != resolved.memberCallCalleeName(expression)
                || metadata->passesReceiver != resolved.memberCallPassesReceiver(expression)
                || metadata->hasTarget != (resolved.memberCallMethodTarget(expression) != nullptr)) {
                ++mismatches;
                continue;
            }
        } catch (const std::logic_error&) {
            ++mismatches;
            continue;
        }

        requireTypedExpression(expression);
        if (!metadata->passesReceiver) {
            continue;
        }

        // Imported method signatures have no AST MethodDecl in the legacy
        // checker. They remain external targets until module symbol
        // materialization is migrated; local methods have an exact pointer.
        const MethodDecl* method = resolved.memberCallMethodTarget(expression);
        if (!method) {
            continue;
        }
        const DeclarationRecord* target = declaration(*method);
        if (!target || target->kind != DeclarationKind::Method) {
            ++mismatches;
            continue;
        }
        memberCallTargets_.emplace(
            entry.first,
            CallTargetRecord{
                CallTargetKind::StructMethod,
                ResolvedSymbol{target->declarationId, target->symbolId}});
    }

    for (const FunctionExpr* expression : functionExpressions_) {
        requireTypedExpression(*expression);
    }
    for (const ReturnStmt* statement : returnStatements_) {
        if (!returnMetadata(*statement)) {
            ++mismatches;
        }
    }
    for (const DeclarationRecord& record : declarations_) {
        if (record.kind == DeclarationKind::Function && record.statement) {
            const auto* function = dynamic_cast<const FunctionStmt*>(record.statement);
            const ResolvedSignatureRecord* signature = resolvedSignature(record.declarationId);
            if (!function
                || !signature
                || signature->type.kind != StaticType::Function
                || !captureMetadata(*function)) {
                ++mismatches;
            }
        } else if (record.kind == DeclarationKind::Method && record.method) {
            const ResolvedSignatureRecord* signature = resolvedSignature(record.declarationId);
            if (!signature
                || signature->type.kind != StaticType::Function
                || !captureMetadata(*record.method)) {
                ++mismatches;
            }
        }
    }
    for (const FunctionExpr* expression : functionExpressions_) {
        if (!captureMetadata(*expression)) {
            ++mismatches;
        }
    }
    for (const BreakStmt* statement : breakStatements_) {
        if (!breakTarget(*statement)) {
            ++mismatches;
        }
    }
    for (const ContinueStmt* statement : continueStatements_) {
        if (!continueTarget(*statement)) {
            ++mismatches;
        }
    }
    for (const LiteralPattern* pattern : literalPatternNodes_) {
        if (!literalPattern(*pattern)) {
            ++mismatches;
        }
    }
    for (const VariantPattern* pattern : variantPatternNodes_) {
        if (!variantPattern(*pattern)) {
            ++mismatches;
        }
    }
    for (const RecordPattern* pattern : recordPatternNodes_) {
        if (!recordPattern(*pattern)) {
            ++mismatches;
        }
    }
    for (const OrPattern* pattern : orPatternNodes_) {
        if (!orPattern(*pattern)) {
            ++mismatches;
        }
    }
    for (const Expr* guard : patternGuardNodes_) {
        if (!patternGuard(*guard)) {
            ++mismatches;
        }
    }
    for (const MatchStmt* match : matchStatementNodes_) {
        if (!matchCoverage(*match)) {
            ++mismatches;
        }
    }
    for (const MatchExpr* match : matchExpressionNodes_) {
        if (!matchCoverage(*match)) {
            ++mismatches;
        }
    }

    for (const FieldAccessExpr* expression : fieldAccesses_) {
        requireTypedExpression(*expression);
    }
    for (const FieldAssignExpr* expression : fieldAssignments_) {
        requireTypedExpression(*expression);
    }
    for (const FieldCompoundAssignExpr* expression : fieldCompoundAssignments_) {
        requireTypedExpression(*expression);
    }
    for (const IndexExpr* expression : indexExpressions_) {
        requireTypedExpression(*expression);
    }
    for (const IndexAssignExpr* expression : indexAssignments_) {
        requireTypedExpression(*expression);
    }
    for (const IndexCompoundAssignExpr* expression : indexCompoundAssignments_) {
        requireTypedExpression(*expression);
    }
    for (const IndexExpr* expression : indexExpressions_) {
        if (!indexOperation(*expression)
            || indexOperation(*expression)->kind != IndexOperationKind::Read) {
            ++mismatches;
        }
    }
    for (const IndexAssignExpr* expression : indexAssignments_) {
        if (!indexOperation(*expression)
            || indexOperation(*expression)->kind != IndexOperationKind::Assign) {
            ++mismatches;
        }
    }
    for (const IndexCompoundAssignExpr* expression : indexCompoundAssignments_) {
        if (!indexOperation(*expression)
            || indexOperation(*expression)->kind != IndexOperationKind::CompoundAssign) {
            ++mismatches;
        }
    }
    for (const FieldAccessExpr* expression : fieldAccesses_) {
        if (!fieldOperation(*expression)
            || fieldOperation(*expression)->kind != FieldOperationKind::Read) {
            ++mismatches;
        }
    }
    for (const FieldAssignExpr* expression : fieldAssignments_) {
        if (!fieldOperation(*expression)
            || fieldOperation(*expression)->kind != FieldOperationKind::Assign) {
            ++mismatches;
        }
    }
    for (const FieldCompoundAssignExpr* expression : fieldCompoundAssignments_) {
        if (!fieldOperation(*expression)
            || fieldOperation(*expression)->kind != FieldOperationKind::CompoundAssign) {
            ++mismatches;
        }
    }
    for (const ArrayExpr* expression : arrayExpressions_) {
        requireTypedExpression(*expression);
    }
    for (const MapExpr* expression : mapExpressions_) {
        requireTypedExpression(*expression);
    }
    for (const StructConstructExpr* expression : structConstructors_) {
        requireTypedExpression(*expression);
        if (!structConstructor(*expression)) {
            ++mismatches;
        }
    }

    for (const auto& entry : nativeCallCandidates_) {
        const Expr& expression = *entry.first;
        if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
            const auto callee = directCallCallees_.find(call);
            if (callee != directCallCallees_.end()
                && resolved.hasVariable(*callee->second)) {
                continue;
            }
        } else if (const auto* memberCall = dynamic_cast<const MemberCallExpr*>(&expression)) {
            if (resolved.hasMemberCallCallee(*memberCall)
                || resolved.hasVariantConstructor(*memberCall)) {
                continue;
            }
        }

        const auto native = nativeCalls_.find(entry.first);
        if (native == nativeCalls_.end() || native->second.name != entry.second) {
            ++mismatches;
            continue;
        }
        requireTypedExpression(expression);
    }
    return mismatches;
}
