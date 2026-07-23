#include "FrontendSession.hpp"
#include "IRCompiler.hpp"
#include "TypeChecker.hpp"

#include <cassert>
#include <sstream>

namespace {

void test_snapshot_identity_metadata()
{
    const std::string source =
        "let x = 1;\n"
        "{ let x = 2; print x; x = 3; }\n"
        "x = 4;\n";
    std::istringstream input(source);
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    assert(program.sources.size() == 1);
    assert(program.sources[0].id == SourceFileId{0});
    for (const Token& token : frontend.displayTokens()) {
        assert(token.range.has_value());
        assert(isValidSourceRange(*token.range, program.sources));
    }

    const auto* outer = dynamic_cast<const LetStmt*>(program.statements[0].get());
    const auto* block = dynamic_cast<const BlockStmt*>(program.statements[1].get());
    const auto* trailing = dynamic_cast<const ExpressionStmt*>(program.statements[2].get());
    assert(outer != nullptr && block != nullptr && trailing != nullptr);
    assert(outer->range.has_value() && outer->syntaxNodeId.has_value());
    assert(block->range.has_value() && block->syntaxNodeId.has_value());
    assert(trailing->range.has_value() && trailing->syntaxNodeId.has_value());
    assert(isValidSourceRange(*outer->range, program.sources));
    assert(isValidSourceRange(*block->range, program.sources));
    assert(isValidSourceRange(*trailing->range, program.sources));
    assert(outer->syntaxNodeId != block->syntaxNodeId);

    const auto* inner = dynamic_cast<const LetStmt*>(block->statements[0].get());
    const auto* print = dynamic_cast<const PrintStmt*>(block->statements[1].get());
    const auto* assignmentStatement = dynamic_cast<const ExpressionStmt*>(block->statements[2].get());
    assert(inner != nullptr && print != nullptr && assignmentStatement != nullptr);
    const auto* read = dynamic_cast<const VariableExpr*>(print->expression.get());
    const auto* assignment = dynamic_cast<const AssignExpr*>(assignmentStatement->expression.get());
    assert(read != nullptr && assignment != nullptr);

    TypeChecker checker;
    const ResolvedNames& resolved = checker.check(program);
    const BindingId outerBinding = resolved.letBindingId(*outer);
    const BindingId innerBinding = resolved.letBindingId(*inner);
    assert(outerBinding != innerBinding);
    assert(resolved.declarationId(*outer) == resolved.binding(outerBinding).declarationId);
    assert(resolved.symbolId(*outer) == resolved.binding(outerBinding).symbolId);
    assert(resolved.declarationId(*inner) == resolved.binding(innerBinding).declarationId);
    assert(resolved.symbolId(*inner) == resolved.binding(innerBinding).symbolId);
    assert(resolved.variableBindingId(*read) == innerBinding);
    assert(resolved.assignmentBindingId(*assignment) == innerBinding);
    assert(resolved.hasScope(*block));
    assert(resolved.binding(outerBinding).scopeId != resolved.binding(innerBinding).scopeId);
    assert(resolved.bindingCount() >= 2);
    assert(resolved.bindingShadowMismatchCount() == 0);
}

void test_declaration_index()
{
    const std::string source =
        "struct Box { value: number }\n"
        "enum Result { Ok(number), Empty }\n"
        "enum Choice { Left(number), Right(number) }\n"
        "impl Box {\n"
        "  fun bump(delta: number): number {\n"
        "    let local = delta;\n"
        "    local += 1;\n"
        "    local = local + 1;\n"
        "    return local;\n"
        "  }\n"
        "}\n"
        "fun use(value: number): number {\n"
        "  let local = value;\n"
        "  local += 1;\n"
        "  local = local + 1;\n"
        "  return local;\n"
        "}\n"
        "fun choose(value: Choice): number {\n"
        "  return match value {\n"
        "    Choice.Left(number) | Choice.Right(number) => number,\n"
        "  };\n"
        "}\n"
        "let x = 1;\n"
        "let result = Result.Ok(1);\n"
        "let box = Box { value: 1 };\n"
        "print box.bump(1);\n"
        "{\n"
        "  let x = 2;\n"
        "  print x;\n"
        "  x = 3;\n"
        "  x += 1;\n"
        "}\n"
        "print use(x);\n";
    std::istringstream input(source);
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* structDecl = dynamic_cast<const StructDeclStmt*>(program.statements[0].get());
    const auto* enumDecl = dynamic_cast<const EnumDeclStmt*>(program.statements[1].get());
    const auto* choiceDecl = dynamic_cast<const EnumDeclStmt*>(program.statements[2].get());
    const auto* impl = dynamic_cast<const ImplStmt*>(program.statements[3].get());
    const auto* function = dynamic_cast<const FunctionStmt*>(program.statements[4].get());
    const auto* choose = dynamic_cast<const FunctionStmt*>(program.statements[5].get());
    const auto* outer = dynamic_cast<const LetStmt*>(program.statements[6].get());
    const auto* result = dynamic_cast<const LetStmt*>(program.statements[7].get());
    const auto* box = dynamic_cast<const LetStmt*>(program.statements[8].get());
    const auto* block = dynamic_cast<const BlockStmt*>(program.statements[10].get());
    assert(structDecl != nullptr && enumDecl != nullptr && choiceDecl != nullptr);
    assert(impl != nullptr && function != nullptr && choose != nullptr);
    assert(outer != nullptr && result != nullptr && box != nullptr && block != nullptr);
    assert(impl->methods.size() == 1);
    const MethodDecl& method = impl->methods.front();

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* structRecord = index.declaration(*structDecl);
    const DeclarationRecord* enumRecord = index.declaration(*enumDecl);
    const DeclarationRecord* choiceRecord = index.declaration(*choiceDecl);
    const DeclarationRecord* methodRecord = index.declaration(method);
    const DeclarationRecord* functionRecord = index.declaration(*function);
    const DeclarationRecord* chooseRecord = index.declaration(*choose);
    const DeclarationRecord* outerRecord = index.declaration(*outer);
    const auto* inner = dynamic_cast<const LetStmt*>(block->statements[0].get());
    assert(structRecord != nullptr && enumRecord != nullptr && choiceRecord != nullptr);
    assert(methodRecord != nullptr && functionRecord != nullptr && chooseRecord != nullptr);
    const DeclarationRecord* innerRecord = inner ? index.declaration(*inner) : nullptr;
    assert(outerRecord != nullptr && innerRecord != nullptr);
    assert(structRecord->kind == DeclarationKind::Struct);
    assert(enumRecord->kind == DeclarationKind::Enum);
    assert(choiceRecord->kind == DeclarationKind::Enum);
    assert(methodRecord->kind == DeclarationKind::Method);
    assert(methodRecord->ownerType == "Box");
    assert(methodRecord->parameters.size() == 1);
    const std::optional<DeclarationSignature> methodSignature
        = index.signature(methodRecord->declarationId);
    assert(methodSignature.has_value());
    assert(methodSignature->parameters.size() == 1);
    assert(methodSignature->parameters.front().name.lexeme == "delta");
    assert(functionRecord->kind == DeclarationKind::Function);
    assert(functionRecord->parameters.size() == 1);
    assert(chooseRecord->kind == DeclarationKind::Function);
    assert(outerRecord->kind == DeclarationKind::Variable);
    assert(outerRecord->range.has_value());
    assert(isValidSourceRange(*outerRecord->range, program.sources));
    assert(index.declaration(function->parameters.front())->kind == DeclarationKind::Parameter);
    assert(index.declaration(method.parameters.front())->kind == DeclarationKind::Parameter);

    const std::optional<ScopeId> rootScope = index.scopes().empty()
        ? std::nullopt
        : std::optional<ScopeId>(index.scopes().front().id);
    const std::optional<ScopeId> blockScope = index.scopeFor(*block);
    const std::optional<ScopeId> functionScope = index.scopeFor(*function);
    assert(rootScope.has_value() && blockScope.has_value() && functionScope.has_value());
    assert(index.scope(*blockScope) != nullptr);
    assert(index.scope(*blockScope)->parent == rootScope);
    assert(index.scope(*functionScope)->parent == rootScope);
    assert(index.lookup(*rootScope, "x") == outerRecord->declarationId);
    const DeclarationId innerId = innerRecord->declarationId;
    assert(index.lookup(*blockScope, "x") == innerId);

    const auto* read = dynamic_cast<const VariableExpr*>(
        dynamic_cast<const PrintStmt*>(block->statements[1].get())->expression.get());
    const auto* assignment = dynamic_cast<const AssignExpr*>(
        dynamic_cast<const ExpressionStmt*>(block->statements[2].get())->expression.get());
    const auto* compound = dynamic_cast<const CompoundAssignExpr*>(
        dynamic_cast<const ExpressionStmt*>(block->statements[3].get())->expression.get());
    assert(read != nullptr && assignment != nullptr && compound != nullptr);
    assert(index.variableReference(*read)->declarationId == innerId);
    assert(index.assignmentReference(*assignment)->declarationId == innerId);
    assert(index.compoundAssignmentReference(*compound)->declarationId == innerId);

    const auto* resultConstructor = dynamic_cast<const MemberCallExpr*>(result->initializer.get());
    const auto* enumQualifier = resultConstructor
        ? dynamic_cast<const VariableExpr*>(resultConstructor->receiver.get())
        : nullptr;
    assert(resultConstructor != nullptr && enumQualifier != nullptr);
    assert(!index.variableReference(*enumQualifier).has_value());

    const auto* callStatement = dynamic_cast<const PrintStmt*>(program.statements[9].get());
    const auto* methodCall = callStatement
        ? dynamic_cast<const MemberCallExpr*>(callStatement->expression.get())
        : nullptr;
    const auto* methodReceiver = methodCall
        ? dynamic_cast<const VariableExpr*>(methodCall->receiver.get())
        : nullptr;
    assert(methodCall != nullptr && methodReceiver != nullptr);
    assert(index.variableReference(*methodReceiver)->declarationId == index.declaration(*box)->declarationId);
    const CallTargetRecord* methodTarget = index.callTarget(*methodCall);
    assert(methodTarget != nullptr);
    assert(methodTarget->kind == CallTargetKind::StructMethod);
    assert(methodTarget->target.declarationId == methodRecord->declarationId);
    assert(index.callTarget(*resultConstructor) == nullptr);

    const auto* directCallStatement = dynamic_cast<const PrintStmt*>(program.statements[11].get());
    const auto* directCall = directCallStatement
        ? dynamic_cast<const CallExpr*>(directCallStatement->expression.get())
        : nullptr;
    const auto* directCallee = directCall
        ? dynamic_cast<const VariableExpr*>(directCall->callee.get())
        : nullptr;
    assert(directCall != nullptr && directCallee != nullptr);
    const CallTargetRecord* directTarget = index.callTarget(*directCall);
    assert(directTarget != nullptr);
    assert(directTarget->kind == CallTargetKind::Direct);
    assert(directTarget->target.declarationId == index.declaration(*function)->declarationId);

    const auto* chooseMatch = dynamic_cast<const MatchExpr*>(
        dynamic_cast<const ReturnStmt*>(choose->body.front().get())->value.get());
    const auto* orPattern = chooseMatch && !chooseMatch->arms.empty()
        ? dynamic_cast<const OrPattern*>(chooseMatch->arms.front().pattern.get())
        : nullptr;
    assert(orPattern != nullptr && orPattern->alternatives.size() == 2);
    const auto* firstPattern = dynamic_cast<const VariantPattern*>(orPattern->alternatives[0].get());
    const auto* secondPattern = dynamic_cast<const VariantPattern*>(orPattern->alternatives[1].get());
    assert(firstPattern != nullptr && secondPattern != nullptr);
    const auto* firstBinding = dynamic_cast<const VariablePattern*>(firstPattern->arguments[0].get());
    const auto* secondBinding = dynamic_cast<const VariablePattern*>(secondPattern->arguments[0].get());
    assert(firstBinding != nullptr && secondBinding != nullptr);
    assert(index.declaration(*firstBinding) != nullptr);
    assert(index.declaration(*firstBinding)->declarationId
        == index.declaration(*secondBinding)->declarationId);
    assert(index.patternBinding(*firstBinding)->declarationId
        == index.patternBinding(*secondBinding)->declarationId);
}

void test_declaration_index_module_metadata()
{
    Token importKeyword{TokenType::Import, "import", 1, 1};
    Token importPath{TokenType::String, "\"lib.cd\"", 1, 8};
    Token alias{TokenType::Identifier, "lib", 1, 18};
    auto import = std::make_unique<ImportStmt>(
        importKeyword, importPath, std::optional<Token>(alias));
    import->resolvedModuleId = 7;

    Token exportKeyword{TokenType::Export, "export", 2, 1};
    Token exportedName{TokenType::Identifier, "value", 2, 8};
    Token sourcePath{TokenType::String, "\"base.cd\"", 2, 19};
    auto exportStmt = std::make_unique<ExportStmt>(
        exportKeyword,
        std::vector<Token>{exportedName},
        std::optional<Token>(sourcePath));
    exportStmt->resolvedModuleId = 11;

    Program program;
    program.statements.push_back(std::move(import));
    program.statements.push_back(std::move(exportStmt));
    const DeclarationIndex index = DeclarationIndex::collect(program);

    assert(index.imports().size() == 1);
    assert(index.imports().front().resolvedModuleId == 7);
    assert(index.imports().front().alias == std::optional<std::string>("lib"));
    assert(index.exports().size() == 1);
    assert(index.exports().front().resolvedModuleId == 11);
    assert(index.exports().front().names == std::vector<std::string>{"value"});
    assert(index.exports().front().sourcePath == std::optional<std::string>("\"base.cd\""));
    assert(index.scopes().size() == 1);
    const std::optional<DeclarationId> aliasId = index.lookup(index.scopes().front().id, "lib");
    assert(aliasId.has_value());
    assert(index.declaration(*aliasId)->kind == DeclarationKind::NamespaceAlias);
}

void test_declaration_index_for_in_binding()
{
    std::istringstream input(
        "let outer = 0;\n"
        "for item in [1, 2] {\n"
        "  print item;\n"
        "  item += 1;\n"
        "}\n"
        "print outer;\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);
    const auto* loop = dynamic_cast<const ForInStmt*>(program.statements[1].get());
    assert(loop != nullptr);
    const auto* body = dynamic_cast<const BlockStmt*>(loop->body.get());
    assert(body != nullptr);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* loopRecord = index.declaration(*loop);
    assert(loopRecord != nullptr);
    assert(loopRecord->kind == DeclarationKind::ForInVariable);
    assert(loopRecord->range.has_value());
    assert(isValidSourceRange(*loopRecord->range, program.sources));
    const std::optional<ResolvedSymbol> binding = index.forInBinding(*loop);
    assert(binding.has_value());
    assert(binding->declarationId == loopRecord->declarationId);

    const std::optional<ScopeId> loopScope = index.scopeFor(*loop);
    const std::optional<ScopeId> bodyScope = index.scopeFor(*body);
    assert(loopScope.has_value() && bodyScope.has_value());
    assert(loopScope == bodyScope);
    assert(index.lookup(*loopScope, "item") == loopRecord->declarationId);

    const auto* print = dynamic_cast<const PrintStmt*>(body->statements[0].get());
    const auto* read = print ? dynamic_cast<const VariableExpr*>(print->expression.get()) : nullptr;
    assert(read != nullptr);
    assert(index.variableReference(*read)->declarationId == loopRecord->declarationId);
}

void test_declaration_index_signature_shapes()
{
    std::istringstream input(
        "struct Box<T> { value: T }\n"
        "enum Result<T> { Ok(value: T), Empty }\n"
        "fun identity<T>(value: T): T { return value; }\n"
        "let box: Box<number> = Box { value: 1 };\n"
        "let result: Result<number> = Result.Ok(1);\n"
        "print identity<number>(1);\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);
    const auto* structDecl = dynamic_cast<const StructDeclStmt*>(program.statements[0].get());
    const auto* enumDecl = dynamic_cast<const EnumDeclStmt*>(program.statements[1].get());
    const auto* function = dynamic_cast<const FunctionStmt*>(program.statements[2].get());
    assert(structDecl != nullptr && enumDecl != nullptr && function != nullptr);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* structRecord = index.declaration(*structDecl);
    const DeclarationRecord* enumRecord = index.declaration(*enumDecl);
    const DeclarationRecord* functionRecord = index.declaration(*function);
    assert(structRecord != nullptr && enumRecord != nullptr && functionRecord != nullptr);

    const std::optional<DeclarationSignature> structSignature
        = index.signature(structRecord->declarationId);
    assert(structSignature.has_value());
    assert(structSignature->typeParameters.size() == 1);
    assert(structSignature->typeParameters.front().name.lexeme == "T");
    const std::optional<DeclarationShape> structShape = index.shape(structRecord->declarationId);
    assert(structShape.has_value());
    assert(structShape->structFields.size() == 1);
    assert(structShape->structFields.front().name.lexeme == "value");
    assert(structShape->structFields.front().typeName.token.lexeme == "T");

    const std::optional<DeclarationSignature> enumSignature
        = index.signature(enumRecord->declarationId);
    assert(enumSignature.has_value());
    assert(enumSignature->typeParameters.size() == 1);
    const std::optional<DeclarationShape> enumShape = index.shape(enumRecord->declarationId);
    assert(enumShape.has_value());
    assert(enumShape->enumVariants.size() == 2);
    assert(enumShape->enumVariants.front().name.lexeme == "Ok");
    assert(enumShape->enumVariants.front().payloadTypes.front().token.lexeme == "T");
    assert(enumShape->enumVariants.front().payloadNames.front()->lexeme == "value");

    const std::optional<DeclarationSignature> functionSignature
        = index.signature(functionRecord->declarationId);
    assert(functionSignature.has_value());
    assert(functionSignature->typeParameters.size() == 1);
    assert(functionSignature->parameters.size() == 1);
    assert(functionSignature->parameters.front().name.lexeme == "value");
    assert(functionSignature->parameters.front().typeName->token.lexeme == "T");
    assert(functionSignature->returnType.has_value());
    assert(functionSignature->returnType->token.lexeme == "T");
}

void test_typed_expression_metadata()
{
    std::istringstream input(
        "struct Box { value: number }\n"
        "fun add(value: number): number { return value; }\n"
        "let x = 1;\n"
        "print x;\n"
        "x = add(2);\n"
        "x += 1;\n"
        "let box = Box { value: 3 };\n"
        "print box.value;\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* printVariable = dynamic_cast<const PrintStmt*>(program.statements[3].get());
    const auto* assignmentStatement = dynamic_cast<const ExpressionStmt*>(program.statements[4].get());
    const auto* compoundStatement = dynamic_cast<const ExpressionStmt*>(program.statements[5].get());
    const auto* printField = dynamic_cast<const PrintStmt*>(program.statements[7].get());
    const auto* assignment = assignmentStatement
        ? dynamic_cast<const AssignExpr*>(assignmentStatement->expression.get())
        : nullptr;
    const auto* compound = compoundStatement
        ? dynamic_cast<const CompoundAssignExpr*>(compoundStatement->expression.get())
        : nullptr;
    const auto* directCall = assignment
        ? dynamic_cast<const CallExpr*>(assignment->value.get())
        : nullptr;
    const auto* field = printField
        ? dynamic_cast<const FieldAccessExpr*>(printField->expression.get())
        : nullptr;
    const auto* variable = printVariable
        ? dynamic_cast<const VariableExpr*>(printVariable->expression.get())
        : nullptr;
    assert(variable != nullptr && assignment != nullptr && compound != nullptr);
    assert(directCall != nullptr && field != nullptr);

    TypeChecker checker;
    const ResolvedNames& resolved = checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const auto assertType = [&index](const Expr& expression, const std::string& expected) {
        const TypedExpressionRecord* record = index.typedExpression(expression);
        assert(record != nullptr);
        assert(typeInfoName(record->type) == expected);
    };
    assertType(*variable, "number");
    assertType(*assignment, "number");
    assertType(*compound, "number");
    assertType(*directCall, "number");
    assertType(*field, "number");

    IRCompiler compiler;
    compiler.compile(program, resolved, index);
}

void test_variable_lowering_metadata()
{
    std::istringstream input(
        "fun id(value) { return value; }\n"
        "let outer = 1;\n"
        "{\n"
        "  let outer = id(2);\n"
        "  print outer;\n"
        "  outer = 3;\n"
        "  outer += 1;\n"
        "  print outer;\n"
        "}\n"
        "print outer;\n"
        "let dynamic = id(4);\n"
        "dynamic = 5;\n"
        "dynamic += 1;\n"
        "print dynamic;\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* outerLet = dynamic_cast<const LetStmt*>(program.statements[1].get());
    const auto* block = dynamic_cast<const BlockStmt*>(program.statements[2].get());
    const auto* outerPrint = dynamic_cast<const PrintStmt*>(program.statements[3].get());
    const auto* dynamicLet = dynamic_cast<const LetStmt*>(program.statements[4].get());
    const auto* dynamicAssignStatement = dynamic_cast<const ExpressionStmt*>(program.statements[5].get());
    const auto* dynamicCompoundStatement = dynamic_cast<const ExpressionStmt*>(program.statements[6].get());
    assert(outerLet != nullptr && block != nullptr && outerPrint != nullptr);
    assert(dynamicLet != nullptr && dynamicAssignStatement != nullptr && dynamicCompoundStatement != nullptr);
    assert(block->statements.size() == 5);

    const auto* innerLet = dynamic_cast<const LetStmt*>(block->statements[0].get());
    const auto* innerPrint = dynamic_cast<const PrintStmt*>(block->statements[1].get());
    const auto* innerAssignStatement = dynamic_cast<const ExpressionStmt*>(block->statements[2].get());
    const auto* innerCompoundStatement = dynamic_cast<const ExpressionStmt*>(block->statements[3].get());
    assert(innerLet != nullptr && innerPrint != nullptr);
    assert(innerAssignStatement != nullptr && innerCompoundStatement != nullptr);

    const auto* innerRead = dynamic_cast<const VariableExpr*>(innerPrint->expression.get());
    const auto* innerAssign = dynamic_cast<const AssignExpr*>(innerAssignStatement->expression.get());
    const auto* innerCompound = dynamic_cast<const CompoundAssignExpr*>(innerCompoundStatement->expression.get());
    const auto* afterBlockRead = dynamic_cast<const VariableExpr*>(outerPrint->expression.get());
    const auto* dynamicAssign = dynamic_cast<const AssignExpr*>(dynamicAssignStatement->expression.get());
    const auto* dynamicCompound = dynamic_cast<const CompoundAssignExpr*>(dynamicCompoundStatement->expression.get());
    assert(innerRead != nullptr && innerAssign != nullptr && innerCompound != nullptr);
    assert(afterBlockRead != nullptr && dynamicAssign != nullptr && dynamicCompound != nullptr);

    TypeChecker checker;
    const ResolvedNames& resolved = checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* outerRecord = index.declaration(*outerLet);
    const DeclarationRecord* innerRecord = index.declaration(*innerLet);
    const DeclarationRecord* dynamicRecord = index.declaration(*dynamicLet);
    assert(outerRecord != nullptr && innerRecord != nullptr && dynamicRecord != nullptr);
    assert(outerRecord->declarationId != innerRecord->declarationId);
    assert(index.variableReference(*innerRead)->declarationId == innerRecord->declarationId);
    assert(index.assignmentReference(*innerAssign)->declarationId == innerRecord->declarationId);
    assert(index.compoundAssignmentReference(*innerCompound)->declarationId == innerRecord->declarationId);
    assert(index.variableReference(*afterBlockRead)->declarationId == outerRecord->declarationId);
    assert(index.assignmentReference(*dynamicAssign)->declarationId == dynamicRecord->declarationId);
    assert(index.compoundAssignmentReference(*dynamicCompound)->declarationId == dynamicRecord->declarationId);

    const auto assertType = [&index](const Expr& expression, const std::string& expected) {
        const TypedExpressionRecord* record = index.typedExpression(expression);
        assert(record != nullptr);
        assert(typeInfoName(record->type) == expected);
    };
    assertType(*innerRead, "unknown");
    assertType(*innerAssign, "number");
    assertType(*innerCompound, "number");
    assertType(*afterBlockRead, "number");
    assertType(*dynamicAssign, "number");
    assertType(*dynamicCompound, "number");

    IRCompiler compiler;
    compiler.compile(program, resolved, index);
}

void test_typed_index_expression_metadata()
{
    std::istringstream input(
        "fun id(value) { return value; }\n"
        "let xs: [number] = [1, 2];\n"
        "let table: map<string, number> = {\"a\": 1};\n"
        "let values = range(3);\n"
        "print xs[0];\n"
        "xs[0] = 2;\n"
        "xs[0] += 3;\n"
        "print table[\"a\"];\n"
        "table[\"a\"] = 4;\n"
        "print values[0];\n"
        "print id(xs)[0];\n"
        "id(xs)[0] = 5;\n"
        "id(xs)[0] += 1;\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* arrayReadStatement = dynamic_cast<const PrintStmt*>(program.statements[4].get());
    const auto* arrayAssignStatement = dynamic_cast<const ExpressionStmt*>(program.statements[5].get());
    const auto* arrayCompoundStatement = dynamic_cast<const ExpressionStmt*>(program.statements[6].get());
    const auto* mapReadStatement = dynamic_cast<const PrintStmt*>(program.statements[7].get());
    const auto* mapAssignStatement = dynamic_cast<const ExpressionStmt*>(program.statements[8].get());
    const auto* rangeReadStatement = dynamic_cast<const PrintStmt*>(program.statements[9].get());
    const auto* dynamicReadStatement = dynamic_cast<const PrintStmt*>(program.statements[10].get());
    const auto* dynamicAssignStatement = dynamic_cast<const ExpressionStmt*>(program.statements[11].get());
    const auto* dynamicCompoundStatement = dynamic_cast<const ExpressionStmt*>(program.statements[12].get());
    const auto* arrayRead = arrayReadStatement
        ? dynamic_cast<const IndexExpr*>(arrayReadStatement->expression.get())
        : nullptr;
    const auto* arrayAssign = arrayAssignStatement
        ? dynamic_cast<const IndexAssignExpr*>(arrayAssignStatement->expression.get())
        : nullptr;
    const auto* arrayCompound = arrayCompoundStatement
        ? dynamic_cast<const IndexCompoundAssignExpr*>(arrayCompoundStatement->expression.get())
        : nullptr;
    const auto* mapRead = mapReadStatement
        ? dynamic_cast<const IndexExpr*>(mapReadStatement->expression.get())
        : nullptr;
    const auto* mapAssign = mapAssignStatement
        ? dynamic_cast<const IndexAssignExpr*>(mapAssignStatement->expression.get())
        : nullptr;
    const auto* rangeRead = rangeReadStatement
        ? dynamic_cast<const IndexExpr*>(rangeReadStatement->expression.get())
        : nullptr;
    const auto* dynamicRead = dynamicReadStatement
        ? dynamic_cast<const IndexExpr*>(dynamicReadStatement->expression.get())
        : nullptr;
    const auto* dynamicAssign = dynamicAssignStatement
        ? dynamic_cast<const IndexAssignExpr*>(dynamicAssignStatement->expression.get())
        : nullptr;
    const auto* dynamicCompound = dynamicCompoundStatement
        ? dynamic_cast<const IndexCompoundAssignExpr*>(dynamicCompoundStatement->expression.get())
        : nullptr;
    assert(arrayRead != nullptr && arrayAssign != nullptr && arrayCompound != nullptr);
    assert(mapRead != nullptr && mapAssign != nullptr && rangeRead != nullptr);
    assert(dynamicRead != nullptr && dynamicAssign != nullptr && dynamicCompound != nullptr);

    TypeChecker checker;
    const ResolvedNames& resolved = checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const auto assertType = [&index](const Expr& expression, const std::string& expected) {
        const TypedExpressionRecord* record = index.typedExpression(expression);
        assert(record != nullptr);
        assert(typeInfoName(record->type) == expected);
    };
    assertType(*arrayRead, "number");
    assertType(*arrayAssign, "number");
    assertType(*arrayCompound, "number");
    assertType(*mapRead, "number");
    assertType(*mapAssign, "number");
    assertType(*rangeRead, "number");
    assertType(*dynamicRead, "unknown");
    assertType(*dynamicAssign, "number");
    assertType(*dynamicCompound, "number");

    IRCompiler compiler;
    compiler.compile(program, resolved, index);
}

void test_call_lowering_metadata()
{
    std::istringstream input(
        "fun add(value: number): number { return value + 1; }\n"
        "let alias = add;\n"
        "print add(1);\n"
        "print alias(2);\n"
        "fun floor(value) { return value; }\n"
        "print floor(3);\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* directPrint = dynamic_cast<const PrintStmt*>(program.statements[2].get());
    const auto* aliasPrint = dynamic_cast<const PrintStmt*>(program.statements[3].get());
    const auto* shadowedPrint = dynamic_cast<const PrintStmt*>(program.statements[5].get());
    const auto* directCall = directPrint
        ? dynamic_cast<const CallExpr*>(directPrint->expression.get())
        : nullptr;
    const auto* aliasCall = aliasPrint
        ? dynamic_cast<const CallExpr*>(aliasPrint->expression.get())
        : nullptr;
    const auto* shadowedCall = shadowedPrint
        ? dynamic_cast<const CallExpr*>(shadowedPrint->expression.get())
        : nullptr;
    assert(directCall != nullptr && aliasCall != nullptr && shadowedCall != nullptr);

    TypeChecker checker;
    const ResolvedNames& resolved = checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const auto assertDirectTarget = [&index](const CallExpr& expression) {
        const TypedExpressionRecord* typed = index.typedExpression(expression);
        assert(typed != nullptr);
        const CallTargetRecord* target = index.callTarget(expression);
        assert(target != nullptr);
        assert(target->kind == CallTargetKind::Direct);
    };
    assertDirectTarget(*directCall);
    assertDirectTarget(*aliasCall);
    assertDirectTarget(*shadowedCall);
    assert(index.nativeCall(*shadowedCall) == nullptr);

    IRCompiler compiler;
    compiler.compile(program, resolved, index);
}

void test_method_call_lowering_metadata()
{
    std::istringstream input(
        "struct Box { value: number }\n"
        "impl Box {\n"
        "  fun add(delta: number): number { return this.value + delta; }\n"
        "}\n"
        "let box = Box { value: 1 };\n"
        "print box.add(2);\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* print = dynamic_cast<const PrintStmt*>(program.statements[3].get());
    const auto* methodCall = print
        ? dynamic_cast<const MemberCallExpr*>(print->expression.get())
        : nullptr;
    assert(methodCall != nullptr);

    TypeChecker checker;
    const ResolvedNames& resolved = checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const TypedExpressionRecord* typed = index.typedExpression(*methodCall);
    assert(typed != nullptr);
    const CallTargetRecord* target = index.callTarget(*methodCall);
    assert(target != nullptr);
    assert(target->kind == CallTargetKind::StructMethod);

    IRCompiler compiler;
    compiler.compile(program, resolved, index);
}

void test_variant_constructor_lowering_metadata()
{
    std::istringstream input(
        "enum Result { Ok(number), Empty }\n"
        "enum Option<T> { Some(T), None }\n"
        "let ok = Result.Ok(1);\n"
        "let none: Option<number> = Option.None<number>();\n"
        "print ok;\n"
        "print none;\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* ok = dynamic_cast<const LetStmt*>(program.statements[2].get());
    const auto* none = dynamic_cast<const LetStmt*>(program.statements[3].get());
    assert(ok != nullptr && none != nullptr);
    const auto* okConstructor = dynamic_cast<const MemberCallExpr*>(ok->initializer.get());
    const auto* noneConstructor = dynamic_cast<const MemberCallExpr*>(none->initializer.get());
    assert(okConstructor != nullptr && noneConstructor != nullptr);

    TypeChecker checker;
    const ResolvedNames& resolved = checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const auto assertVariant = [&index](
        const MemberCallExpr& expression,
        const std::string& enumName,
        const std::string& variantName) {
        assert(index.typedExpression(expression) != nullptr);
        const VariantConstructorRecord* variant = index.variantConstructor(expression);
        assert(variant != nullptr);
        assert(variant->enumName == enumName);
        assert(variant->variantName == variantName);
    };
    assertVariant(*okConstructor, "Result", "Ok");
    assertVariant(*noneConstructor, "Option", "None");

    IRCompiler compiler;
    compiler.compile(program, resolved, index);
}

void test_typed_field_assignment_metadata()
{
    std::istringstream input(
        "fun id(value) { return value; }\n"
        "struct Box { value: number }\n"
        "let box = Box { value: 1 };\n"
        "box.value = 2;\n"
        "box.value += 3;\n"
        "id(box).value = 4;\n"
        "id(box).value += 5;\n"
        "print id(box).value;\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* staticAssignStatement = dynamic_cast<const ExpressionStmt*>(program.statements[3].get());
    const auto* staticCompoundStatement = dynamic_cast<const ExpressionStmt*>(program.statements[4].get());
    const auto* dynamicAssignStatement = dynamic_cast<const ExpressionStmt*>(program.statements[5].get());
    const auto* dynamicCompoundStatement = dynamic_cast<const ExpressionStmt*>(program.statements[6].get());
    const auto* dynamicReadStatement = dynamic_cast<const PrintStmt*>(program.statements[7].get());
    const auto* staticAssign = staticAssignStatement
        ? dynamic_cast<const FieldAssignExpr*>(staticAssignStatement->expression.get())
        : nullptr;
    const auto* staticCompound = staticCompoundStatement
        ? dynamic_cast<const FieldCompoundAssignExpr*>(staticCompoundStatement->expression.get())
        : nullptr;
    const auto* dynamicAssign = dynamicAssignStatement
        ? dynamic_cast<const FieldAssignExpr*>(dynamicAssignStatement->expression.get())
        : nullptr;
    const auto* dynamicCompound = dynamicCompoundStatement
        ? dynamic_cast<const FieldCompoundAssignExpr*>(dynamicCompoundStatement->expression.get())
        : nullptr;
    const auto* dynamicRead = dynamicReadStatement
        ? dynamic_cast<const FieldAccessExpr*>(dynamicReadStatement->expression.get())
        : nullptr;
    assert(staticAssign != nullptr && staticCompound != nullptr);
    assert(dynamicAssign != nullptr && dynamicCompound != nullptr && dynamicRead != nullptr);

    TypeChecker checker;
    const ResolvedNames& resolved = checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const auto assertType = [&index](const Expr& expression, const std::string& expected) {
        const TypedExpressionRecord* record = index.typedExpression(expression);
        assert(record != nullptr);
        assert(typeInfoName(record->type) == expected);
    };
    assertType(*staticAssign, "number");
    assertType(*staticCompound, "number");
    assertType(*dynamicAssign, "number");
    assertType(*dynamicCompound, "number");
    assertType(*dynamicRead, "unknown");

    IRCompiler compiler;
    compiler.compile(program, resolved, index);
}

void test_native_call_metadata()
{
    std::istringstream input(
        "fun floor(value) { return value; }\n"
        "let xs: [number] = [1, 2];\n"
        "let shadowed = floor(1);\n"
        "let rounded = ceil(1.5);\n"
        "print xs.contains(1);\n"
        "let doubled = xs.map(fun (value: number): number { return value + 1; });\n"
        "print str(rounded);\n"
        "print xs.len();\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* shadowed = dynamic_cast<const LetStmt*>(program.statements[2].get());
    const auto* rounded = dynamic_cast<const LetStmt*>(program.statements[3].get());
    const auto* containsPrint = dynamic_cast<const PrintStmt*>(program.statements[4].get());
    const auto* doubled = dynamic_cast<const LetStmt*>(program.statements[5].get());
    const auto* strPrint = dynamic_cast<const PrintStmt*>(program.statements[6].get());
    const auto* lenPrint = dynamic_cast<const PrintStmt*>(program.statements[7].get());
    const auto* shadowedCall = shadowed
        ? dynamic_cast<const CallExpr*>(shadowed->initializer.get())
        : nullptr;
    const auto* roundedCall = rounded
        ? dynamic_cast<const CallExpr*>(rounded->initializer.get())
        : nullptr;
    const auto* containsCall = containsPrint
        ? dynamic_cast<const MemberCallExpr*>(containsPrint->expression.get())
        : nullptr;
    const auto* mapCall = doubled
        ? dynamic_cast<const MemberCallExpr*>(doubled->initializer.get())
        : nullptr;
    const auto* strCall = strPrint
        ? dynamic_cast<const CallExpr*>(strPrint->expression.get())
        : nullptr;
    const auto* lenCall = lenPrint
        ? dynamic_cast<const MemberCallExpr*>(lenPrint->expression.get())
        : nullptr;
    assert(shadowedCall != nullptr && roundedCall != nullptr);
    assert(containsCall != nullptr && mapCall != nullptr && strCall != nullptr);
    assert(lenCall != nullptr);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    assert(index.nativeCall(*shadowedCall) == nullptr);
    assert(index.typedExpression(*shadowedCall) != nullptr);

    const auto assertNative = [&index](
        const Expr& expression,
        const std::string& name,
        const std::string& type) {
        const NativeCallRecord* native = index.nativeCall(expression);
        assert(native != nullptr);
        assert(native->name == name);
        const TypedExpressionRecord* typed = index.typedExpression(expression);
        assert(typed != nullptr);
        assert(typeInfoName(typed->type) == type);
    };
    assertNative(*roundedCall, "ceil", "number");
    assertNative(*containsCall, "contains", "bool");
    assertNative(*mapCall, "map", "[number]");
    assertNative(*strCall, "str", "string");
    assert(index.nativeCall(*lenCall) == nullptr);
}

void test_collection_expression_metadata()
{
    std::istringstream input(
        "fun id(value) { return value; }\n"
        "struct Box { value: number }\n"
        "let numbers: [number] = [1, 2];\n"
        "let table: map<string, number> = {\"a\": 1};\n"
        "let box = Box { value: 1 };\n"
        "let dynamicArray = [id(1)];\n"
        "let dynamicMap = {\"a\": id(1)};\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);

    const auto* numbers = dynamic_cast<const LetStmt*>(program.statements[2].get());
    const auto* table = dynamic_cast<const LetStmt*>(program.statements[3].get());
    const auto* box = dynamic_cast<const LetStmt*>(program.statements[4].get());
    const auto* dynamicArray = dynamic_cast<const LetStmt*>(program.statements[5].get());
    const auto* dynamicMap = dynamic_cast<const LetStmt*>(program.statements[6].get());
    const auto* numbersLiteral = numbers
        ? dynamic_cast<const ArrayExpr*>(numbers->initializer.get())
        : nullptr;
    const auto* tableLiteral = table
        ? dynamic_cast<const MapExpr*>(table->initializer.get())
        : nullptr;
    const auto* boxConstructor = box
        ? dynamic_cast<const StructConstructExpr*>(box->initializer.get())
        : nullptr;
    const auto* dynamicArrayLiteral = dynamicArray
        ? dynamic_cast<const ArrayExpr*>(dynamicArray->initializer.get())
        : nullptr;
    const auto* dynamicMapLiteral = dynamicMap
        ? dynamic_cast<const MapExpr*>(dynamicMap->initializer.get())
        : nullptr;
    assert(numbersLiteral != nullptr && tableLiteral != nullptr && boxConstructor != nullptr);
    assert(dynamicArrayLiteral != nullptr && dynamicMapLiteral != nullptr);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const auto assertType = [&index](const Expr& expression, const std::string& expected) {
        const TypedExpressionRecord* record = index.typedExpression(expression);
        assert(record != nullptr);
        assert(typeInfoName(record->type) == expected);
    };
    assertType(*numbersLiteral, "[number]");
    assertType(*tableLiteral, "map<string, number>");
    assertType(*boxConstructor, "Box");
    assertType(*dynamicArrayLiteral, "array");
    assertType(*dynamicMapLiteral, "map");
}

} // namespace

int main()
{
    std::istringstream input("print 1 / 0;\n");
    FrontendSession frontend;
    Program program = frontend.loadStdin(input);
    TypeChecker checker;
    const ResolvedNames& resolved = checker.check(program);
    IRCompiler compiler;
    IRProgram ir = compiler.compile(program, resolved, checker.declarationIndex());

    assert(ir.sources().size() == 1);
    const auto& divide = ir.instructions().at(2);
    assert(divide.op == IROp::Divide);
    assert(divide.span.has_value());
    assert(divide.span->source == 0);
    assert(divide.span->line == 1);
    assert(divide.span->column == 7);

    test_snapshot_identity_metadata();
    test_declaration_index();
    test_declaration_index_module_metadata();
    test_declaration_index_for_in_binding();
    test_declaration_index_signature_shapes();
    test_typed_expression_metadata();
    test_variable_lowering_metadata();
    test_typed_index_expression_metadata();
    test_call_lowering_metadata();
    test_method_call_lowering_metadata();
    test_variant_constructor_lowering_metadata();
    test_typed_field_assignment_metadata();
    test_native_call_metadata();
    test_collection_expression_metadata();
    return 0;
}
