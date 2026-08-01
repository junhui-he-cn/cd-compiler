#include "ModuleInterfaceArtifact.hpp"

#include "TypeUtils.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

ModuleInterface makeInterface()
{
    ModuleInterface interfaceInfo;
    interfaceInfo.moduleId = 91;
    interfaceInfo.sourceId = SourceFileId{17};
    interfaceInfo.path = "/tmp/lib.cd";
    interfaceInfo.canonicalPath = "/tmp/lib.cd";
    interfaceInfo.isEntry = false;
    interfaceInfo.resolvedNameNext = 6;
    interfaceInfo.values.push_back(ModuleInterfaceValue{
        "identity",
        functionType(
            {typeParameterType("T", simpleType(StaticType::Number))},
            typeParameterType("T"),
            {"T"},
            {std::make_shared<TypeInfo>(simpleType(StaticType::Number))}),
        "identity#4"});
    interfaceInfo.values.push_back(ModuleInterfaceValue{
        "compare",
        functionType(
            {typeParameterType("T"), typeParameterType("T")},
            simpleType(StaticType::Bool),
            {"T"},
            {std::make_shared<TypeInfo>(capabilityType("Ord"))}),
        "compare#6"});
    interfaceInfo.values.push_back(ModuleInterfaceValue{
        "hashCompare",
        functionType(
            {typeParameterType("T"), typeParameterType("T")},
            simpleType(StaticType::Number),
            {"T"},
            {std::make_shared<TypeInfo>(capabilitySetType({
                capabilityType("Hash"),
                capabilityType("Eq")}))}),
        "hashCompare#9"});

    ModuleInterfaceStruct box;
    box.name = "Box";
    box.hasPrivateFields = true;
    box.genericParameters = {"T"};
    box.genericParameterConstraints = {std::make_shared<TypeInfo>(simpleType(StaticType::Number))};
    box.fields.push_back(ModuleInterfaceField{"value", typeParameterType("T")});
    box.methods.push_back(ModuleInterfaceMethod{
        "echo",
        {typeParameterType("U")},
        typeParameterType("U"),
        {"U"},
        {nullptr},
        namedStructType("Box", {typeParameterType("T")}),
        "__method_Box_echo#5"});
    box.operators.push_back(ModuleInterfaceOperator{
        ">",
        namedStructType("Box", {typeParameterType("T")}),
        namedStructType("Box", {typeParameterType("T")}),
        simpleType(StaticType::Bool),
        {"T"},
        {std::make_shared<TypeInfo>(simpleType(StaticType::Number))},
        "__method_Box_operator_Greater#7"});
    box.operators.push_back(ModuleInterfaceOperator{
        "<",
        namedStructType("Box", {typeParameterType("T")}),
        namedStructType("Box", {typeParameterType("T")}),
        simpleType(StaticType::Bool),
        {"T"},
        {std::make_shared<TypeInfo>(simpleType(StaticType::Number))},
        "__method_Box_operator_Less#8"});
    interfaceInfo.structs.push_back(std::move(box));

    ModuleInterfaceStruct node;
    node.name = "Node";
    node.genericParameters = {"T"};
    node.genericParameterConstraints = {nullptr};
    node.fields.push_back(ModuleInterfaceField{
        "next",
        nullableType(namedStructType("Node", {typeParameterType("T")}))});
    interfaceInfo.structs.push_back(std::move(node));

    ModuleInterfaceEnum option;
    option.name = "Option";
    option.genericParameters = {"T"};
    option.genericParameterConstraints = {nullptr};
    option.variants.push_back(ModuleInterfaceVariant{
        "Some",
        {typeParameterType("T")},
        {std::optional<std::string>("value")}});
    option.variants.push_back(ModuleInterfaceVariant{"None", {}, {}});
    interfaceInfo.enums.push_back(std::move(option));

    return interfaceInfo;
}

ModuleInterfaceArtifact makeArtifact()
{
    ModuleInterfaceArtifact artifact;
    artifact.identity = "/tmp/lib.cd";
    artifact.path = "/tmp/lib.cd";
    artifact.canonicalPath = "/tmp/lib.cd";
    artifact.sourceHash = "fnv1a64-source";
    artifact.isEntry = false;
    artifact.interfaceInfo = makeInterface();
    artifact.dependencies.push_back(ModuleInterfaceArtifactDependency{
        "/tmp/base.cd",
        ModuleGraphEdgeKind::Import,
        "./base.cd",
        "fnv1a64-base-interface"});
    artifact.interfaceInfo.dependencies.push_back(ModuleInterfaceDependency{
        17,
        ModuleGraphEdgeKind::Import,
        "./base.cd"});
    return artifact;
}

} // namespace

int main()
{
    ModuleInterfaceArtifact artifact = makeArtifact();
    std::ostringstream output;
    writeModuleInterfaceArtifactText(output, artifact);
    const std::string text = output.str();
    assert(text.rfind("cdi 0.1\n", 0) == 0);
    assert(text.find("module_id") == std::string::npos);
    assert(text.find("source_id") == std::string::npos);
    assert(text.find("  operators = 2\n") != std::string::npos);
    assert(text.find("    right = ") != std::string::npos);

    const ModuleInterfaceArtifactLoadResult loaded = readModuleInterfaceArtifactText(text);
    assert(loaded.found);
    assert(loaded.error.empty());
    assert(loaded.artifact);
    assert(loaded.artifact->identity == artifact.identity);
    assert(loaded.artifact->resolvedNameNext == 6);
    assert(loaded.artifact->interfaceInfo.resolvedNameNext == 6);
    assert(loaded.artifact->interfaceHash == moduleInterfaceArtifactHash(artifact.interfaceInfo));
    assert(loaded.artifact->interfaceInfo.moduleId == 0);
    assert(!loaded.artifact->interfaceInfo.sourceId.valid());
    assert(loaded.artifact->interfaceInfo.values.size() == 3);
    assert(loaded.artifact->interfaceInfo.structs.size() == 2);
    assert(loaded.artifact->interfaceInfo.structs.front().hasPrivateFields);
    assert(loaded.artifact->interfaceInfo.enums.size() == 1);
    assert(loaded.artifact->interfaceInfo.dependencies.front().importedModuleId == 0);
    assert(loaded.artifact->interfaceInfo.values.front().resolvedName == "compare#6");
    assert(typeInfoName(loaded.artifact->interfaceInfo.values.front().type)
        == "fun<T: Ord>(T, T): bool");
    assert(typeInfoName(loaded.artifact->interfaceInfo.values[1].type)
        == "fun<T: Eq + Hash>(T, T): number");
    assert(loaded.artifact->interfaceInfo.values.back().resolvedName == "identity#4");
    assert(typeInfoName(loaded.artifact->interfaceInfo.structs.front().methods.front().receiverType)
        == "Box<T>");
    assert(loaded.artifact->interfaceInfo.structs.front().operators.size() == 2);
    assert(loaded.artifact->interfaceInfo.structs.front().operators.front().symbol == "<");
    assert(loaded.artifact->interfaceInfo.structs.front().operators.front().rightParameterType.structName
        == "Box");
    assert(loaded.artifact->interfaceInfo.structs.front().operators.front().genericParameters
        == std::vector<std::string>{"T"});
    assert(loaded.artifact->interfaceInfo.structs.front().operators.front().returnType.kind
        == StaticType::Bool);
    assert(loaded.artifact->interfaceInfo.structs.size() == 2);
    const ModuleInterfaceStruct& node = loaded.artifact->interfaceInfo.structs.back();
    assert(node.name == "Node");
    assert(node.genericParameters == std::vector<std::string>{"T"});
    assert(node.fields.size() == 1);
    assert(typeInfoName(node.fields.front().type) == "optional<Node<T>>");

    ModuleInterface withoutOperators = makeInterface();
    withoutOperators.structs.front().operators.clear();
    assert(moduleInterfaceArtifactHash(withoutOperators) != moduleInterfaceArtifactHash(artifact.interfaceInfo));
    ModuleInterface linkageChanged = makeInterface();
    linkageChanged.structs.front().operators.front().resolvedName += "-changed";
    assert(moduleInterfaceArtifactHash(linkageChanged) != moduleInterfaceArtifactHash(artifact.interfaceInfo));
    ModuleInterface recursiveShapeChanged = makeInterface();
    recursiveShapeChanged.structs.back().fields.front().type
        = arrayType(namedStructType("Node", {typeParameterType("T")}));
    assert(moduleInterfaceArtifactHash(recursiveShapeChanged)
        != moduleInterfaceArtifactHash(artifact.interfaceInfo));

    std::string legacyWithoutOperators = text;
    const std::size_t operatorStart = legacyWithoutOperators.find("  operators = 2\n");
    const std::size_t enumStart = legacyWithoutOperators.find("enums = ", operatorStart);
    assert(operatorStart != std::string::npos);
    assert(enumStart != std::string::npos);
    legacyWithoutOperators.erase(operatorStart, enumStart - operatorStart);
    const ModuleInterfaceArtifactLoadResult missingOperators
        = readModuleInterfaceArtifactText(legacyWithoutOperators);
    assert(missingOperators.found);
    assert(!missingOperators.artifact);
    assert(!missingOperators.error.empty());

    std::string legacy = text;
    const std::string allocatorLine = "resolved_name_next = 6\n";
    const std::size_t allocatorOffset = legacy.find(allocatorLine);
    assert(allocatorOffset != std::string::npos);
    legacy.erase(allocatorOffset, allocatorLine.size());
    const ModuleInterfaceArtifactLoadResult legacyLoaded = readModuleInterfaceArtifactText(legacy);
    assert(legacyLoaded.found);
    assert(legacyLoaded.artifact);
    assert(legacyLoaded.artifact->resolvedNameNext == 0);

    std::string malformed = text;
    malformed.replace(0, 7, "cdi 9.9");
    const ModuleInterfaceArtifactLoadResult badVersion = readModuleInterfaceArtifactText(malformed);
    assert(badVersion.found);
    assert(!badVersion.artifact);
    assert(!badVersion.error.empty());

    malformed = text;
    const std::size_t interfaceField = malformed.find("interface = ");
    assert(interfaceField != std::string::npos);
    const std::size_t valueStart = interfaceField + std::string("interface = \"").size();
    const std::size_t valueEnd = malformed.find('"', valueStart);
    assert(valueEnd != std::string::npos);
    malformed.replace(valueStart, valueEnd - valueStart, "fnv1a64-bad");
    const ModuleInterfaceArtifactLoadResult badHash = readModuleInterfaceArtifactText(malformed);
    assert(badHash.found);
    assert(!badHash.artifact);
    assert(!badHash.error.empty());

    std::cout << "module interface artifact tests: round trip, type fidelity, identity, and malformed input validated\n";
    return 0;
}
