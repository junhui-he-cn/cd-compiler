#include "BytecodeTextEmitter.hpp"

#include <cassert>
#include <sstream>
#include <string>

namespace {

void testLinkedArtifactRemainsUnchanged()
{
    BytecodeProgram program;
    program.setRegisterCount(0);

    std::ostringstream output;
    writeBytecodeText(output, program);
    assert(output.str() ==
        "cdbc 0.1\n\n"
        "constants:\n"
        "\n"
        "names:\n"
        "\n"
        "main registers=0:\n");
}

void testModuleEnvelopeAndDependencyMarker()
{
    BytecodeModuleArtifact artifact;
    artifact.identity = "/workspace/lib.cd";
    artifact.path = "lib.cd";
    artifact.canonicalPath = "/workspace/lib.cd";
    artifact.isEntry = true;
    artifact.entryOrder = 0;
    artifact.dependencies.push_back(BytecodeModuleDependency{
        "/workspace/shared.cd",
        ModuleGraphEdgeKind::Import,
        "./shared.cd",
        2});
    artifact.program.setRegisterCount(0);

    std::ostringstream output;
    writeBytecodeModuleText(output, artifact);
    assert(output.str() ==
        "cdbc 0.1\n\n"
        "artifact: module\n\n"
        "module:\n"
        "  identity = \"/workspace/lib.cd\"\n"
        "  path = \"lib.cd\"\n"
        "  canonical_path = \"/workspace/lib.cd\"\n"
        "  entry = true\n"
        "  entry_order = 0\n"
        "  dependencies:\n"
        "    d0 target=\"/workspace/shared.cd\" kind=import at=2 requested=\"./shared.cd\"\n"
        "\n"
        "constants:\n"
        "\n"
        "names:\n"
        "\n"
        "main registers=0:\n");
}

void testDebugSourceModuleIdentity()
{
    BytecodeProgram program;
    SourceFile source;
    source.path = "lib.cd";
    source.text = "print 1;\n";
    source.id = SourceFileId{0};
    source.moduleIdentity = "/workspace/lib.cd";
    program.setSources({source});

    std::ostringstream output;
    writeBytecodeText(output, program);
    assert(output.str() ==
        "cdbc 0.1\n\n"
        "constants:\n"
        "\n"
        "names:\n"
        "\n"
        "main registers=0:\n"
        "\n"
        "debug_sources:\n"
        "  s0 module=\"/workspace/lib.cd\" path=\"lib.cd\" text=\"print 1;\\n\"\n");
}

} // namespace

int main()
{
    testLinkedArtifactRemainsUnchanged();
    testModuleEnvelopeAndDependencyMarker();
    testDebugSourceModuleIdentity();
    return 0;
}
