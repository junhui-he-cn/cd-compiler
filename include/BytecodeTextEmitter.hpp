#pragma once

#include "Bytecode.hpp"

#include <ostream>

void writeBytecodeText(std::ostream& out, const BytecodeProgram& program);
void writeBytecodeModuleText(std::ostream& out, const BytecodeModuleArtifact& artifact);
