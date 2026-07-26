#include "ModuleInterfaceEmitter.hpp"

#include "TypeUtils.hpp"

#include <algorithm>
#include <cstddef>
#include <ostream>
#include <sstream>
#include <vector>

namespace {

template <typename T, typename NameGetter>
std::vector<T> sortedByName(std::vector<T> values, NameGetter name)
{
    std::sort(values.begin(), values.end(), [&](const T& left, const T& right) {
        return name(left) < name(right);
    });
    return values;
}

std::vector<ModuleInterface> sortedModules(std::vector<ModuleInterface> modules)
{
    std::sort(modules.begin(), modules.end(), [](const ModuleInterface& left, const ModuleInterface& right) {
        return left.moduleId < right.moduleId;
    });
    return modules;
}

void writeMethodSignature(std::ostream& out, const ModuleInterfaceMethod& method)
{
    out << method.name;
    if (!method.genericParameters.empty()) {
        out << '<';
        for (std::size_t i = 0; i < method.genericParameters.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << method.genericParameters[i];
            if (i < method.genericParameterConstraints.size()
                && method.genericParameterConstraints[i]) {
                out << ": " << typeInfoName(*method.genericParameterConstraints[i]);
            }
        }
        out << '>';
    }
    out << '(';
    for (std::size_t i = 0; i < method.parameterTypes.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << typeInfoName(method.parameterTypes[i]);
    }
    out << "): " << typeInfoName(method.returnType);
}

} // namespace

void writeModuleInterfaceText(std::ostream& out, const std::vector<ModuleInterface>& interfaces)
{
    std::vector<ModuleInterface> modules = sortedModules(interfaces);
    for (std::size_t moduleIndex = 0; moduleIndex < modules.size(); ++moduleIndex) {
        if (moduleIndex != 0) {
            out << '\n';
        }

        const ModuleInterface& module = modules[moduleIndex];
        out << "module " << module.moduleId;
        if (module.isEntry) {
            out << " entry";
        }
        out << " \"" << module.path << "\"\n";

        std::vector<ModuleInterfaceValue> values = sortedByName(module.values, [](const ModuleInterfaceValue& value) {
            return value.name;
        });
        for (const ModuleInterfaceValue& value : values) {
            out << "  export value " << value.name << ": " << typeInfoName(value.type) << "\n";
        }

        std::vector<ModuleInterfaceStruct> structs = sortedByName(module.structs, [](const ModuleInterfaceStruct& structInfo) {
            return structInfo.name;
        });
        for (ModuleInterfaceStruct structInfo : structs) {
            out << "  export struct " << structInfo.name;
            if (!structInfo.genericParameters.empty()) {
                out << '<';
                for (std::size_t i = 0; i < structInfo.genericParameters.size(); ++i) {
                    if (i != 0) {
                        out << ", ";
                    }
                    out << structInfo.genericParameters[i];
                    if (i < structInfo.genericParameterConstraints.size()
                        && structInfo.genericParameterConstraints[i]) {
                        out << ": " << typeInfoName(*structInfo.genericParameterConstraints[i]);
                    }
                }
                out << '>';
            }
            out << '\n';
            for (const ModuleInterfaceField& field : structInfo.fields) {
                out << "    field " << field.name << ": " << typeInfoName(field.type) << "\n";
            }

            std::vector<ModuleInterfaceMethod> methods = sortedByName(structInfo.methods, [](const ModuleInterfaceMethod& method) {
                return method.name;
            });
            for (const ModuleInterfaceMethod& method : methods) {
                out << "    method ";
                writeMethodSignature(out, method);
                out << "\n";
            }
        }

        std::vector<ModuleInterfaceEnum> enums = sortedByName(module.enums, [](const ModuleInterfaceEnum& enumInfo) {
            return enumInfo.name;
        });
        for (ModuleInterfaceEnum enumInfo : enums) {
            out << "  export enum " << enumInfo.name;
            if (!enumInfo.genericParameters.empty()) {
                out << '<';
                for (std::size_t i = 0; i < enumInfo.genericParameters.size(); ++i) {
                    if (i != 0) {
                        out << ", ";
                    }
                    out << enumInfo.genericParameters[i];
                    if (i < enumInfo.genericParameterConstraints.size()
                        && enumInfo.genericParameterConstraints[i]) {
                        out << ": " << typeInfoName(*enumInfo.genericParameterConstraints[i]);
                    }
                }
                out << '>';
            }
            out << "\n";
            for (const ModuleInterfaceVariant& variant : enumInfo.variants) {
                out << "    variant " << variant.name;
                if (!variant.payloadTypes.empty()) {
                    out << '(';
                    for (std::size_t i = 0; i < variant.payloadTypes.size(); ++i) {
                        if (i != 0) {
                            out << ", ";
                        }
                        if (i < variant.payloadNames.size() && variant.payloadNames[i]) {
                            out << *variant.payloadNames[i] << ": ";
                        }
                        out << typeInfoName(variant.payloadTypes[i]);
                    }
                    out << ')';
                }
                out << "\n";
            }
        }
    }
}

void writeModuleInterfaceShapeText(std::ostream& out, const ModuleInterface& interfaceInfo)
{
    std::ostringstream full;
    writeModuleInterfaceText(full, std::vector<ModuleInterface>{interfaceInfo});
    const std::string text = full.str();
    const std::size_t headerEnd = text.find('\n');
    if (headerEnd != std::string::npos) {
        out << text.substr(headerEnd + 1);
    }
}
