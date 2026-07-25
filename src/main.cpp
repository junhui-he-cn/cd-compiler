#include "BytecodeCompiler.hpp"
#include "BytecodeTextEmitter.hpp"
#include "FrontendSession.hpp"
#include "IRCompiler.hpp"
#include "ModuleInterfaceEmitter.hpp"
#include "Parser.hpp"
#include "TypeChecker.hpp"

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void printUsage(const char* executable)
{
    std::cerr << "Usage: " << executable << " [--tokens] [--ir] [--bytecode] [--module-interface] [-I dir] [--import-path dir] [file ...]\n"
              << "       " << executable << " [--emit-bytecode output.cdbc] [-I dir] [--import-path dir] file [...]\n"
              << "If no file is provided, source is read from stdin except for --emit-bytecode, which requires at least one file.\n"
              << "Import search paths are used for non-explicit string imports after the importing file's directory.\n";
}

void printParseErrorList(const ParseErrorList& errors, const std::string& source)
{
    bool first = true;
    for (const ParseError& error : errors.errors()) {
        if (!first) {
            std::cerr << '\n';
        }
        first = false;
        std::cerr << formatDiagnosticWithSource(error, source);
    }
    std::cerr << '\n';
}

void printFileDiagnosticErrorList(const FileDiagnosticErrorList& errors)
{
    bool first = true;
    for (const FileDiagnosticError& error : errors.errors()) {
        if (!first) {
            std::cerr << '\n';
        }
        first = false;
        std::cerr << formatDiagnosticWithSourceContext(error);
    }
    std::cerr << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    bool showTokens = false;
    bool showIr = false;
    bool showBytecode = false;
    bool showModuleInterface = false;
    std::optional<std::string> emitBytecodePath;
    std::vector<std::string> inputPaths;
    std::vector<std::string> importSearchPaths;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--tokens") {
            showTokens = true;
        } else if (arg == "--ir") {
            showIr = true;
        } else if (arg == "--bytecode") {
            showBytecode = true;
        } else if (arg == "--module-interface") {
            showModuleInterface = true;
        } else if (arg == "-I" || arg == "--import-path") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            importSearchPaths.push_back(argv[++i]);
        } else if (arg == "--run") {
            printUsage(argv[0]);
            return 64;
        } else if (arg == "--emit-bytecode") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return 64;
            }
            emitBytecodePath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            inputPaths.push_back(arg);
        }
    }

    if (emitBytecodePath) {
        if (inputPaths.empty() || showTokens || showIr || showBytecode || showModuleInterface) {
            printUsage(argv[0]);
            return 64;
        }
    }

    FrontendSession frontend;
    frontend.setImportSearchPaths(importSearchPaths);
    try {
        Program program = inputPaths.empty()
            ? frontend.loadStdin(std::cin)
            : frontend.loadFiles(inputPaths);

        if (showTokens) {
            for (const Token& token : frontend.displayTokens()) {
                std::cout << token << '\n';
            }
            std::cout << '\n';
        }

        TypeChecker typeChecker;
        typeChecker.check(program);

        if (!emitBytecodePath && !showIr && !showBytecode && !showModuleInterface) {
            program.print(std::cout);
        }

        if (showModuleInterface) {
            writeModuleInterfaceText(std::cout, typeChecker.moduleInterfaces());
        }

        if (emitBytecodePath || showIr || showBytecode) {
            IRCompiler compiler;
            IRProgram ir = compiler.compile(program, typeChecker.declarationIndex());

            std::optional<BytecodeProgram> bytecode;
            if (emitBytecodePath || showBytecode) {
                BytecodeCompiler bytecodeCompiler;
                bytecode = bytecodeCompiler.compile(ir);
            }

            if (emitBytecodePath) {
                std::ostringstream artifact;
                writeBytecodeText(artifact, *bytecode);
                std::ofstream output(*emitBytecodePath);
                if (!output) {
                    throw std::runtime_error("failed to open bytecode output file: " + *emitBytecodePath);
                }
                output << artifact.str();
                if (!output) {
                    throw std::runtime_error("failed to write bytecode output file: " + *emitBytecodePath);
                }
                return 0;
            }

            bool emittedSection = false;
            const auto separateSection = [&emittedSection]() {
                if (emittedSection) {
                    std::cout << '\n';
                }
                emittedSection = true;
            };

            if (showIr) {
                separateSection();
                ir.print(std::cout);
            }

            if (showBytecode) {
                separateSection();
                bytecode->print(std::cout);
            }

        }
    } catch (const FileDiagnosticErrorList& errors) {
        printFileDiagnosticErrorList(errors);
        return 1;
    } catch (const ParseErrorList& errors) {
        printParseErrorList(errors, frontend.sourceForDiagnostics());
        return 1;
    } catch (const FileDiagnosticError& error) {
        std::cerr << formatDiagnosticWithSourceContext(error) << '\n';
        return 1;
    } catch (const DiagnosticError& error) {
        if (const std::optional<FileDiagnosticError> remapped = frontend.remapDirectDiagnostic(error)) {
            std::cerr << formatDiagnosticWithSourceContext(*remapped) << '\n';
            return 1;
        }
        std::cerr << formatDiagnosticWithSource(error, frontend.sourceForDiagnostics()) << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
