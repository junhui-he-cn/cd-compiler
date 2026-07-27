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
    interfaceInfo.structs.push_back(std::move(box));

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
    assert(loaded.artifact->interfaceInfo.values.size() == 1);
    assert(loaded.artifact->interfaceInfo.structs.size() == 1);
    assert(loaded.artifact->interfaceInfo.structs.front().hasPrivateFields);
    assert(loaded.artifact->interfaceInfo.enums.size() == 1);
    assert(loaded.artifact->interfaceInfo.dependencies.front().importedModuleId == 0);
    assert(loaded.artifact->interfaceInfo.values.front().resolvedName == "identity#4");
    assert(typeInfoName(loaded.artifact->interfaceInfo.structs.front().methods.front().receiverType)
        == "Box<T>");

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
