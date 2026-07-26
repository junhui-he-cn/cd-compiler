#pragma once

#include "ModuleInterface.hpp"

#include <iosfwd>
#include <vector>

void writeModuleInterfaceText(std::ostream& out, const std::vector<ModuleInterface>& interfaces);
void writeModuleInterfaceShapeText(std::ostream& out, const ModuleInterface& interfaceInfo);
