#include "ModuleInterface.hpp"
#include "ModuleInterfaceEmitter.hpp"
#include "TypeUtils.hpp"

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expectEqual(const std::string& name, const std::string& actual, const std::string& expected)
{
    if (actual == expected) {
        return;
    }
    ++failures;
    std::cerr << "FAIL " << name << "\nexpected:\n" << expected << "\nactual:\n" << actual << "\n";
}

std::string emit(const std::vector<ModuleInterface>& interfaces)
{
    std::ostringstream out;
    writeModuleInterfaceText(out, interfaces);
    return out.str();
}

void testEmptyAndSortedModules()
{
    std::vector<ModuleInterface> interfaces;

    ModuleInterface later;
    later.moduleId = 2;
    later.path = "later.cd";

    ModuleInterface entry;
    entry.moduleId = 0;
    entry.path = "main.cd";
    entry.isEntry = true;

    interfaces.push_back(later);
    interfaces.push_back(entry);

    expectEqual(
        "empty modules sorted by id",
        emit(interfaces),
        "module 0 entry \"main.cd\"\n"
        "\n"
        "module 2 \"later.cd\"\n");
}

void testValuesStructsFieldsMethodsAndTypes()
{
    ModuleInterface module;
    module.moduleId = 0;
    module.path = "main.cd";
    module.isEntry = true;

    module.values.push_back(ModuleInterfaceValue{"zeta", nullableType(namedStructType("Point")), ""});
    module.values.push_back(ModuleInterfaceValue{
        "make",
        functionType(
            std::vector<TypeInfo>{simpleType(StaticType::Number), arrayType(nullableType(simpleType(StaticType::String)))},
            namedStructType("Point")),
            ""});
    module.values.push_back(ModuleInterfaceValue{
        "identity",
        functionType({typeParameterType("T")}, typeParameterType("T"), {"T"}),
        ""});
    module.values.push_back(ModuleInterfaceValue{"dynamic", unknownType(), ""});

    ModuleInterfaceStruct point;
    point.name = "Point";
    point.hasPrivateFields = true;
    point.fields.push_back(ModuleInterfaceField{"y", simpleType(StaticType::Number)});
    point.fields.push_back(ModuleInterfaceField{"x", nullableType(simpleType(StaticType::Number))});
    point.methods.push_back(ModuleInterfaceMethod{
        "translate",
        {simpleType(StaticType::Number), simpleType(StaticType::Number)},
        namedStructType("Point"),
        {},
        {},
        unknownType(),
        ""});
    point.methods.push_back(ModuleInterfaceMethod{
        "echo",
        {typeParameterType("T")},
        typeParameterType("T"),
        {"T"},
        {std::make_shared<TypeInfo>(simpleType(StaticType::Number))},
        unknownType(),
        ""});
    point.methods.push_back(ModuleInterfaceMethod{
        "length", {}, simpleType(StaticType::Number), {}, {}, unknownType(), ""});

    ModuleInterfaceStruct box;
    box.name = "Box";
    box.fields.push_back(ModuleInterfaceField{"items", arrayType(namedStructType("Point"))});

    module.structs.push_back(point);
    module.structs.push_back(box);

    ModuleInterfaceEnum result;
    result.name = "Result";
    result.variants.push_back(ModuleInterfaceVariant{"Ok", {simpleType(StaticType::Number)}, {}});
    result.variants.push_back(ModuleInterfaceVariant{"Empty", {}, {}});
    module.enums.push_back(result);

    expectEqual(
        "exports sorted and fields preserved",
        emit(std::vector<ModuleInterface>{module}),
        "module 0 entry \"main.cd\"\n"
        "  export value dynamic: unknown\n"
        "  export value identity: fun<T>(T): T\n"
        "  export value make: fun(number, [optional<string>]): Point\n"
        "  export value zeta: optional<Point>\n"
        "  export struct Box\n"
        "    field items: [Point]\n"
        "  export struct Point\n"
        "    private fields\n"
        "    field y: number\n"
        "    field x: optional<number>\n"
        "    method echo<T: number>(T): T\n"
        "    method length(): number\n"
        "    method translate(number, number): Point\n"
        "  export enum Result\n"
        "    variant Ok(number)\n"
        "    variant Empty\n");
}

} // namespace

int main()
{
    testEmptyAndSortedModules();
    testValuesStructsFieldsMethodsAndTypes();
    return failures == 0 ? 0 : 1;
}
