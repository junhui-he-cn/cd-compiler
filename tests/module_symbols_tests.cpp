#include "ModuleSymbols.hpp"
#include "Token.hpp"
#include "TypeUtils.hpp"

#include <cassert>
#include <string>
#include <utility>

namespace {

Token token(std::string lexeme)
{
    return Token{TokenType::Identifier, std::move(lexeme), 1, 1};
}

TypeBinding binding(std::string resolvedName, StaticType kind)
{
    TypeBinding result;
    result.type = simpleType(kind);
    result.resolvedName = std::move(resolvedName);
    return result;
}

StructTypeDecl structDecl(std::string name)
{
    return StructTypeDecl{
        token(std::move(name)),
        {StructFieldType{token("field"), simpleType(StaticType::Number)}},
        {},
        {},
        false,
        std::nullopt};
}

MethodSignature methodSignature(std::string resolvedName, TypeInfo returnType)
{
    MethodSignature result;
    result.receiverType = namedStructType("Person");
    result.parameterTypes = {simpleType(StaticType::Number)};
    result.returnType = std::move(returnType);
    result.resolvedName = std::move(resolvedName);
    return result;
}

OperatorSignature operatorSignature(std::string resolvedName)
{
    OperatorSignature result;
    result.receiverType = namedStructType("Person");
    result.rightParameterType = namedStructType("Person");
    result.returnType = simpleType(StaticType::Bool);
    result.resolvedName = std::move(resolvedName);
    return result;
}

void test_direct_import_deduplicates_per_importing_module()
{
    ModuleSymbols symbols;

    assert(symbols.markDirectImport(1, 2));
    assert(!symbols.markDirectImport(1, 2));
    assert(symbols.markDirectImport(1, 3));
    assert(symbols.markDirectImport(2, 2));
}

void test_value_exports_are_recorded_and_missing_modules_return_null()
{
    ModuleSymbols symbols;

    assert(symbols.valueExports(7) == nullptr);
    symbols.recordValueExport(7, "answer", binding("answer#0", StaticType::Number));

    const ModuleValueExports* exports = symbols.valueExports(7);
    assert(exports != nullptr);
    assert(exports->size() == 1);
    assert(exports->at("answer").resolvedName == "answer#0");
    assert(exports->at("answer").type.kind == StaticType::Number);
}

void test_struct_exports_and_local_struct_markers_are_independent()
{
    ModuleSymbols symbols;

    assert(!symbols.isLocalStruct(4, "Person"));
    symbols.markLocalStruct(4, "Person");
    assert(symbols.isLocalStruct(4, "Person"));
    assert(symbols.structExports(4) == nullptr);

    symbols.recordStructExport(4, "Person", structDecl("Person"));
    const ModuleStructExports* exports = symbols.structExports(4);
    assert(exports != nullptr);
    assert(exports->size() == 1);
    assert(exports->at("Person").name.lexeme == "Person");
}

void test_namespace_aliases_are_recorded_and_queried()
{
    ModuleSymbols symbols;
    NamespaceImport imported;
    imported.values.emplace("value", binding("value#0", StaticType::String));
    imported.structs.emplace("Person", structDecl("Person"));
    imported.methods["Person"].emplace("ageNext", methodSignature("__method_Person_ageNext#0", simpleType(StaticType::Number)));

    assert(!symbols.hasNamespace(9, "lib"));
    assert(symbols.namespaceImport(9, "lib") == nullptr);

    symbols.recordNamespace(9, "lib", std::move(imported));

    assert(symbols.hasNamespace(9, "lib"));
    const NamespaceImport* found = symbols.namespaceImport(9, "lib");
    assert(found != nullptr);
    assert(found->values.at("value").type.kind == StaticType::String);
    assert(found->structs.at("Person").name.lexeme == "Person");
    assert(found->methods.at("Person").at("ageNext").resolvedName == "__method_Person_ageNext#0");
    assert(found->methods.at("Person").at("ageNext").returnType.kind == StaticType::Number);
}

void test_method_exports_are_recorded_with_struct_names()
{
    ModuleSymbols symbols;

    assert(symbols.methodExports(3) == nullptr);
    symbols.recordMethodExport(3, "Person", "ageNext", methodSignature("__method_Person_ageNext#0", simpleType(StaticType::Number)));

    const ModuleMethodExports* exports = symbols.methodExports(3);
    assert(exports != nullptr);
    assert(exports->size() == 1);
    assert(exports->at("Person").size() == 1);
    assert(exports->at("Person").at("ageNext").resolvedName == "__method_Person_ageNext#0");
    assert(exports->at("Person").at("ageNext").returnType.kind == StaticType::Number);
}

void test_operator_exports_are_recorded_with_struct_names()
{
    ModuleSymbols symbols;

    assert(symbols.operatorExports(3) == nullptr);
    symbols.recordOperatorExport(3, "Person", "<", operatorSignature("__method_Person_operator_Less#0"));

    const ModuleOperatorExports* exports = symbols.operatorExports(3);
    assert(exports != nullptr);
    assert(exports->size() == 1);
    assert(exports->at("Person").size() == 1);
    assert(exports->at("Person").at("<").resolvedName == "__method_Person_operator_Less#0");
    assert(exports->at("Person").at("<").returnType.kind == StaticType::Bool);
}


void test_export_lookup_and_method_table_forwarding()
{
    ModuleSymbols symbols;

    assert(!symbols.hasAnyExport(1, "Person"));
    symbols.recordStructExport(1, "Person", structDecl("Person"));
    assert(symbols.hasAnyExport(1, "Person"));
    assert(!symbols.hasAnyExport(1, "answer"));
    symbols.recordValueExport(1, "answer", binding("answer#0", StaticType::Number));
    assert(symbols.hasAnyExport(1, "answer"));

    StructMethodTable methods;
    methods.emplace("ageNext", methodSignature("__method_Person_ageNext#0", simpleType(StaticType::Number)));
    symbols.recordMethodExports(2, "Person", methods);

    const ModuleMethodExports* exports = symbols.methodExports(2);
    assert(exports != nullptr);
    assert(exports->at("Person").at("ageNext").resolvedName == "__method_Person_ageNext#0");

    StructOperatorTable operators;
    operators.emplace("<", operatorSignature("__method_Person_operator_Less#1"));
    symbols.recordOperatorExports(2, "Person", operators);
    assert(symbols.operatorExports(2) != nullptr);
    assert(symbols.operatorExports(2)->at("Person").at("<").resolvedName
        == "__method_Person_operator_Less#1");
}

void test_clear_removes_all_tables()
{
    ModuleSymbols symbols;
    symbols.markDirectImport(1, 2);
    symbols.recordValueExport(1, "value", binding("value#0", StaticType::Number));
    symbols.markLocalStruct(1, "Person");
    symbols.recordStructExport(1, "Person", structDecl("Person"));
    symbols.recordMethodExport(1, "Person", "ageNext", methodSignature("__method_Person_ageNext#0", simpleType(StaticType::Number)));
    symbols.recordOperatorExport(1, "Person", "<", operatorSignature("__method_Person_operator_Less#1"));
    NamespaceImport imported;
    imported.values.emplace("value", binding("value#1", StaticType::String));
    symbols.recordNamespace(1, "lib", std::move(imported));

    symbols.clear();

    assert(symbols.markDirectImport(1, 2));
    assert(symbols.valueExports(1) == nullptr);
    assert(!symbols.isLocalStruct(1, "Person"));
    assert(symbols.structExports(1) == nullptr);
    assert(symbols.methodExports(1) == nullptr);
    assert(symbols.operatorExports(1) == nullptr);
    assert(!symbols.hasNamespace(1, "lib"));
    assert(symbols.namespaceImport(1, "lib") == nullptr);
}

} // namespace

int main()
{
    test_direct_import_deduplicates_per_importing_module();
    test_value_exports_are_recorded_and_missing_modules_return_null();
    test_struct_exports_and_local_struct_markers_are_independent();
    test_namespace_aliases_are_recorded_and_queried();
    test_method_exports_are_recorded_with_struct_names();
    test_operator_exports_are_recorded_with_struct_names();
    test_export_lookup_and_method_table_forwarding();
    test_clear_removes_all_tables();
}
