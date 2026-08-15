#include "FrontendSession.hpp"
#include "TypeChecker.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

Program loadProgram(const std::string& source)
{
    std::istringstream input(source);
    FrontendSession frontend;
    return frontend.loadStdin(input);
}

const ModuleStmt& entryModule(const Program& program)
{
    assert(program.statements.size() == 1);
    const auto* module = dynamic_cast<const ModuleStmt*>(program.statements[0].get());
    assert(module != nullptr);
    return *module;
}

const ModuleStmt& entryModuleOf(const Program& program)
{
    for (const StmtPtr& statement : program.statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (module && module->isEntry) {
            return *module;
        }
    }
    assert(false);
}

void writeModuleSource(const fs::path& path, const std::string& source)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << source;
}

const VariableExpr& asVariable(const Expr& expression)
{
    const auto* variable = dynamic_cast<const VariableExpr*>(&expression);
    assert(variable != nullptr);
    return *variable;
}

std::string typeErrorMessage(const std::function<void()>& action)
{
    try {
        action();
    } catch (const TypeError& error) {
        return error.message();
    } catch (const FileDiagnosticErrorList& errors) {
        std::string joined;
        for (const FileDiagnosticError& error : errors.errors()) {
            if (!joined.empty()) {
                joined += '\n';
            }
            joined += error.message();
        }
        return joined;
    }
    assert(false);
    return {};
}

void test_shadowed_scope_resolution()
{
    const std::string source =
        "let x = 1;\n"
        "{\n"
        "  let x = 2;\n"
        "  print x;\n"
        "  x = 3;\n"
        "  x += 1;\n"
        "}\n"
        "print x;\n";
    const Program program = loadProgram(source);
    const ModuleStmt& module = entryModule(program);
    const auto* outer = dynamic_cast<const LetStmt*>(module.statements[0].get());
    const auto* block = dynamic_cast<const BlockStmt*>(module.statements[1].get());
    const auto* trailingPrint = dynamic_cast<const PrintStmt*>(module.statements[2].get());
    assert(outer && block && trailingPrint);

    const auto* inner = dynamic_cast<const LetStmt*>(block->statements[0].get());
    const auto* innerPrint = dynamic_cast<const PrintStmt*>(block->statements[1].get());
    const auto* assignStmt = dynamic_cast<const ExpressionStmt*>(block->statements[2].get());
    const auto* compoundStmt = dynamic_cast<const ExpressionStmt*>(block->statements[3].get());
    assert(inner && innerPrint && assignStmt && compoundStmt);
    const auto* assign = dynamic_cast<const AssignExpr*>(assignStmt->expression.get());
    const auto* compound = dynamic_cast<const CompoundAssignExpr*>(compoundStmt->expression.get());
    const VariableExpr& innerRead = asVariable(*innerPrint->expression);
    const VariableExpr& trailingRead = asVariable(*trailingPrint->expression);
    assert(assign && compound);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* outerRecord = index.declaration(*outer);
    const DeclarationRecord* innerRecord = index.declaration(*inner);
    const BindingMetadataRecord* outerBinding = index.letBindingMetadata(*outer);
    const BindingMetadataRecord* innerBinding = index.letBindingMetadata(*inner);
    assert(outerRecord && innerRecord && outerBinding && innerBinding);
    assert(outerRecord->declarationId != innerRecord->declarationId);

    const auto innerReadRef = index.variableReference(innerRead);
    const auto trailingReadRef = index.variableReference(trailingRead);
    const auto assignRef = index.assignmentReference(*assign);
    const auto compoundRef = index.compoundAssignmentReference(*compound);
    assert(innerReadRef && trailingReadRef && assignRef && compoundRef);
    assert(innerReadRef->declarationId == innerRecord->declarationId);
    assert(trailingReadRef->declarationId == outerRecord->declarationId);
    assert(assignRef->declarationId == innerRecord->declarationId);
    assert(compoundRef->declarationId == innerRecord->declarationId);

    const BindingMetadataRecord* readBinding = index.variableBindingMetadata(innerRead);
    const BindingMetadataRecord* assignBinding = index.assignmentBindingMetadata(*assign);
    const BindingMetadataRecord* compoundBinding
        = index.compoundAssignmentBindingMetadata(*compound);
    assert(readBinding && assignBinding && compoundBinding);
    assert(readBinding->bindingId == innerBinding->bindingId);
    assert(assignBinding->bindingId == innerBinding->bindingId);
    assert(compoundBinding->bindingId == innerBinding->bindingId);
}

void test_parameter_capture_and_this_resolution()
{
    const std::string source =
        "let outer = 10;\n"
        "fun add(delta: number): number {\n"
        "  return outer + delta;\n"
        "}\n"
        "struct Box { value: number }\n"
        "impl Box {\n"
        "  fun total(delta: number): number {\n"
        "    return this.value + delta;\n"
        "  }\n"
        "}\n"
        "let box = Box { value: 1 };\n"
        "print add(box.total(2));\n";
    const Program program = loadProgram(source);
    const ModuleStmt& module = entryModule(program);
    const auto* outer = dynamic_cast<const LetStmt*>(module.statements[0].get());
    const auto* add = dynamic_cast<const FunctionStmt*>(module.statements[1].get());
    const auto* impl = dynamic_cast<const ImplStmt*>(module.statements[3].get());
    assert(outer && add && impl && impl->methods.size() == 1);
    const MethodDecl& method = impl->methods[0];

    const auto* addReturn = dynamic_cast<const ReturnStmt*>(add->body[0].get());
    const auto* methodReturn = dynamic_cast<const ReturnStmt*>(method.body[0].get());
    assert(addReturn && methodReturn);
    const auto* addBinary = dynamic_cast<const BinaryExpr*>(addReturn->value.get());
    const auto* methodBinary = dynamic_cast<const BinaryExpr*>(methodReturn->value.get());
    assert(addBinary && methodBinary);
    const VariableExpr& capturedOuter = asVariable(*addBinary->left);
    const VariableExpr& addDelta = asVariable(*addBinary->right);
    const auto* thisAccess = dynamic_cast<const FieldAccessExpr*>(methodBinary->left.get());
    const VariableExpr& methodDelta = asVariable(*methodBinary->right);
    assert(thisAccess);
    const VariableExpr& thisRef = asVariable(*thisAccess->object);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* outerRecord = index.declaration(*outer);
    const DeclarationRecord* addDeltaRecord = index.declaration(add->parameters[0]);
    const DeclarationRecord* methodDeltaRecord = index.declaration(method.parameters[0]);
    assert(outerRecord && addDeltaRecord && methodDeltaRecord);

    const auto capturedOuterRef = index.variableReference(capturedOuter);
    const auto addDeltaRef = index.variableReference(addDelta);
    const auto thisRefRecord = index.variableReference(thisRef);
    const auto methodDeltaRef = index.variableReference(methodDelta);
    assert(capturedOuterRef && addDeltaRef && thisRefRecord && methodDeltaRef);
    assert(capturedOuterRef->declarationId == outerRecord->declarationId);
    assert(addDeltaRef->declarationId == addDeltaRecord->declarationId);
    assert(methodDeltaRef->declarationId == methodDeltaRecord->declarationId);

    const std::vector<DeclarationId> methodParameters
        = index.functionParameterDeclarations(method);
    assert(!methodParameters.empty());
    const DeclarationRecord* thisDeclaration = index.declaration(methodParameters.front());
    assert(thisDeclaration && thisDeclaration->name == "this");
    assert(thisRefRecord->declarationId == thisDeclaration->declarationId);
}

void test_nested_function_capture_resolution()
{
    const std::string source =
        "fun outer(seed: number): number {\n"
        "  let local = seed + 1;\n"
        "  fun inner(delta: number): number {\n"
        "    return local + delta;\n"
        "  }\n"
        "  return inner(1);\n"
        "}\n"
        "print outer(1);\n";
    const Program program = loadProgram(source);
    const ModuleStmt& module = entryModule(program);
    const auto* outer = dynamic_cast<const FunctionStmt*>(module.statements[0].get());
    assert(outer && outer->body.size() == 3);
    const auto* local = dynamic_cast<const LetStmt*>(outer->body[0].get());
    const auto* inner = dynamic_cast<const FunctionStmt*>(outer->body[1].get());
    const auto* innerReturn = dynamic_cast<const ReturnStmt*>(inner->body[0].get());
    assert(local && inner && innerReturn);
    const auto* binary = dynamic_cast<const BinaryExpr*>(innerReturn->value.get());
    assert(binary);
    const VariableExpr& capturedLocal = asVariable(*binary->left);
    const VariableExpr& deltaRead = asVariable(*binary->right);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* localRecord = index.declaration(*local);
    const DeclarationRecord* deltaRecord = index.declaration(inner->parameters[0]);
    assert(localRecord && deltaRecord);
    const auto capturedLocalRef = index.variableReference(capturedLocal);
    const auto deltaRef = index.variableReference(deltaRead);
    assert(capturedLocalRef && deltaRef);
    assert(capturedLocalRef->declarationId == localRecord->declarationId);
    assert(deltaRef->declarationId == deltaRecord->declarationId);

    const CaptureRecord* captures = index.captureMetadata(*inner);
    assert(captures);
    const auto found = std::find_if(
        captures->symbols.begin(),
        captures->symbols.end(),
        [&](const ResolvedSymbol& symbol) {
            return symbol.declarationId == localRecord->declarationId;
        });
    assert(found != captures->symbols.end());
}

void test_match_pattern_binding_resolution()
{
    const std::string source =
        "enum Option { Some(number), None }\n"
        "let value = Option.Some(5);\n"
        "match value {\n"
        "  Option.Some(x) => { print x; }\n"
        "  Option.None => { print 0; }\n"
        "}\n"
        "print value;\n";
    const Program program = loadProgram(source);
    const ModuleStmt& module = entryModule(program);
    const auto* value = dynamic_cast<const LetStmt*>(module.statements[1].get());
    const auto* matchStatement = dynamic_cast<const ExpressionStmt*>(module.statements[2].get());
    const auto* match = matchStatement
        ? dynamic_cast<const MatchExpr*>(matchStatement->expression.get())
        : nullptr;
    const auto* trailingPrint = dynamic_cast<const PrintStmt*>(module.statements[3].get());
    assert(value && match && trailingPrint && match->arms.size() == 2);

    const auto* variantPattern
        = dynamic_cast<const VariantPattern*>(match->arms[0].pattern.get());
    const auto* armBlock = dynamic_cast<const BlockStmt*>(match->arms[0].body.get());
    const auto* armPrint = armBlock
        ? dynamic_cast<const PrintStmt*>(armBlock->statements[0].get())
        : nullptr;
    assert(variantPattern && armPrint && variantPattern->arguments.size() == 1);
    const auto* bindingPattern
        = dynamic_cast<const VariablePattern*>(variantPattern->arguments[0].get());
    assert(bindingPattern);
    const VariableExpr& bindingRead = asVariable(*armPrint->expression);
    const VariableExpr& trailingRead = asVariable(*trailingPrint->expression);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* valueRecord = index.declaration(*value);
    const DeclarationRecord* bindingRecord = index.declaration(*bindingPattern);
    assert(valueRecord && bindingRecord);
    const auto bindingRef = index.variableReference(bindingRead);
    const auto trailingRef = index.variableReference(trailingRead);
    assert(bindingRef && trailingRef);
    assert(bindingRef->declarationId == bindingRecord->declarationId);
    assert(trailingRef->declarationId == valueRecord->declarationId);

    const PatternBindingRecord* patternBinding = index.patternBindingMetadata(*bindingPattern);
    const BindingMetadataRecord* readBinding = index.variableBindingMetadata(bindingRead);
    assert(patternBinding && readBinding);
    assert(patternBinding->bindingId == readBinding->bindingId);
    assert(patternBinding->symbol.declarationId == bindingRecord->declarationId);
}

void test_or_pattern_shared_binding_resolution()
{
    const std::string source =
        "enum E { A(number), B(number) }\n"
        "let e = E.A(1);\n"
        "match e {\n"
        "  E.A(v) | E.B(v) => { print v; }\n"
        "}\n";
    const Program program = loadProgram(source);
    const ModuleStmt& module = entryModule(program);
    const auto* matchStatement = dynamic_cast<const ExpressionStmt*>(module.statements[2].get());
    const auto* match = matchStatement
        ? dynamic_cast<const MatchExpr*>(matchStatement->expression.get())
        : nullptr;
    assert(match && match->arms.size() == 1);
    const auto* orPattern = dynamic_cast<const OrPattern*>(match->arms[0].pattern.get());
    const auto* armBlock = dynamic_cast<const BlockStmt*>(match->arms[0].body.get());
    const auto* armPrint = armBlock
        ? dynamic_cast<const PrintStmt*>(armBlock->statements[0].get())
        : nullptr;
    assert(orPattern && armPrint && orPattern->alternatives.size() == 2);
    const auto* firstVariant
        = dynamic_cast<const VariantPattern*>(orPattern->alternatives[0].get());
    const auto* secondVariant
        = dynamic_cast<const VariantPattern*>(orPattern->alternatives[1].get());
    assert(firstVariant && secondVariant
        && firstVariant->arguments.size() == 1 && secondVariant->arguments.size() == 1);
    const auto* firstBinding
        = dynamic_cast<const VariablePattern*>(firstVariant->arguments[0].get());
    const auto* secondBinding
        = dynamic_cast<const VariablePattern*>(secondVariant->arguments[0].get());
    assert(firstBinding && secondBinding);
    const VariableExpr& bindingRead = asVariable(*armPrint->expression);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* firstRecord = index.declaration(*firstBinding);
    const DeclarationRecord* secondRecord = index.declaration(*secondBinding);
    assert(firstRecord && secondRecord);
    assert(firstRecord->declarationId == secondRecord->declarationId);
    const auto bindingRef = index.variableReference(bindingRead);
    assert(bindingRef);
    assert(bindingRef->declarationId == firstRecord->declarationId);

    const PatternBindingRecord* firstMetadata = index.patternBindingMetadata(*firstBinding);
    const PatternBindingRecord* secondMetadata = index.patternBindingMetadata(*secondBinding);
    assert(firstMetadata && secondMetadata);
    assert(firstMetadata->bindingId == secondMetadata->bindingId);
}

void test_for_in_binding_resolution()
{
    const std::string source =
        "let items = [1, 2, 3];\n"
        "for item in items {\n"
        "  print item;\n"
        "}\n";
    const Program program = loadProgram(source);
    const ModuleStmt& module = entryModule(program);
    const auto* forIn = dynamic_cast<const ForInStmt*>(module.statements[1].get());
    const auto* bodyBlock = dynamic_cast<const BlockStmt*>(forIn->body.get());
    const auto* bodyPrint = bodyBlock
        ? dynamic_cast<const PrintStmt*>(bodyBlock->statements[0].get())
        : nullptr;
    assert(forIn && bodyPrint);
    const VariableExpr& itemRead = asVariable(*bodyPrint->expression);

    TypeChecker checker;
    checker.check(program);
    const DeclarationIndex& index = checker.declarationIndex();
    assert(checker.declarationIndexMismatchCount() == 0);

    const DeclarationRecord* itemRecord = index.declaration(*forIn);
    assert(itemRecord);
    const auto itemRef = index.variableReference(itemRead);
    assert(itemRef);
    assert(itemRef->declarationId == itemRecord->declarationId);

    const BindingMetadataRecord* forInBinding = index.forInBindingMetadata(*forIn);
    const BindingMetadataRecord* readBinding = index.variableBindingMetadata(itemRead);
    assert(forInBinding && readBinding);
    assert(forInBinding->bindingId == readBinding->bindingId);
}

void test_import_reference_falls_back_to_name_resolution(const fs::path& root)
{
    fs::remove_all(root);
    const fs::path library = root / "lib.cd";
    const fs::path entry = root / "input.cd";
    writeModuleSource(library, "let zeta = 2;\nexport zeta;\n");
    writeModuleSource(entry, "import \"./lib.cd\";\nprint zeta;\n");

    FrontendSession frontend;
    const Program program = frontend.loadFiles({entry.string()});
    const ModuleStmt& module = entryModuleOf(program);
    const auto* printStmt = dynamic_cast<const PrintStmt*>(module.statements[1].get());
    assert(printStmt);
    const VariableExpr& zetaRead = asVariable(*printStmt->expression);

    TypeChecker checker;
    checker.check(program);
    assert(checker.declarationIndexMismatchCount() == 0);
    assert(checker.moduleInterfaceMismatchCount() == 0);

    const DeclarationIndex& index = checker.declarationIndex();
    assert(!index.variableReference(zetaRead).has_value());
    const BindingMetadataRecord* readBinding = index.variableBindingMetadata(zetaRead);
    assert(readBinding && readBinding->imported);
}

void test_undefined_variable_diagnostics()
{
    {
        const Program program = loadProgram("print missing;\n");
        TypeChecker checker;
        const std::string message = typeErrorMessage([&]() { checker.check(program); });
        assert(message.find("undefined variable `missing`") != std::string::npos);
    }
    {
        const Program program = loadProgram("missing = 1;\n");
        TypeChecker checker;
        const std::string message = typeErrorMessage([&]() { checker.check(program); });
        assert(message.find("undefined variable `missing`") != std::string::npos);
    }
    {
        const Program program = loadProgram("missing += 1;\n");
        TypeChecker checker;
        const std::string message = typeErrorMessage([&]() { checker.check(program); });
        assert(message.find("undefined variable `missing`") != std::string::npos);
    }
}

} // namespace

int main()
{
    test_shadowed_scope_resolution();
    test_parameter_capture_and_this_resolution();
    test_nested_function_capture_resolution();
    test_match_pattern_binding_resolution();
    test_or_pattern_shared_binding_resolution();
    test_for_in_binding_resolution();
    test_import_reference_falls_back_to_name_resolution(
        fs::temp_directory_path() / "compiler_index_resolution_import_fallback");
    test_undefined_variable_diagnostics();
    return 0;
}
