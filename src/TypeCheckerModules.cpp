#include "TypeChecker.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace {
Token interfaceToken(const Token& anchor, const std::string& lexeme)
{
    Token token = anchor;
    token.type = TokenType::Identifier;
    token.lexeme = lexeme;
    return token;
}

ModuleValueExports valueExportsFromInterface(const ModuleInterface& interfaceInfo)
{
    ModuleValueExports exports;
    for (const ModuleInterfaceValue& value : interfaceInfo.values) {
        TypeBinding binding;
        binding.type = value.type;
        binding.resolvedName = value.resolvedName;
        exports.emplace(value.name, std::move(binding));
    }
    return exports;
}

ModuleStructExports structExportsFromInterface(const ModuleInterface& interfaceInfo, const Token& anchor)
{
    ModuleStructExports exports;
    for (const ModuleInterfaceStruct& source : interfaceInfo.structs) {
        StructTypeDecl declaration;
        declaration.name = interfaceToken(anchor, source.name);
        declaration.genericParameters = source.genericParameters;
        declaration.genericParameterConstraints = source.genericParameterConstraints;
        declaration.hasPrivateFields = source.hasPrivateFields;
        declaration.definingModuleId = source.definingModuleId.value_or(interfaceInfo.moduleId);
        for (const ModuleInterfaceField& field : source.fields) {
            declaration.fields.push_back(StructFieldType{interfaceToken(anchor, field.name), field.type, false});
        }
        exports.emplace(source.name, std::move(declaration));
    }
    return exports;
}

ModuleEnumExports enumExportsFromInterface(const ModuleInterface& interfaceInfo, const Token& anchor)
{
    ModuleEnumExports exports;
    for (const ModuleInterfaceEnum& source : interfaceInfo.enums) {
        EnumTypeDecl declaration;
        declaration.name = interfaceToken(anchor, source.name);
        declaration.genericParameters = source.genericParameters;
        declaration.genericParameterConstraints = source.genericParameterConstraints;
        for (const ModuleInterfaceVariant& variant : source.variants) {
            EnumVariantType converted;
            converted.name = interfaceToken(anchor, variant.name);
            converted.payloadTypes = variant.payloadTypes;
            for (const std::optional<std::string>& payloadName : variant.payloadNames) {
                converted.payloadNames.push_back(
                    payloadName ? std::optional<Token>(interfaceToken(anchor, *payloadName)) : std::nullopt);
            }
            declaration.variants.push_back(std::move(converted));
        }
        exports.emplace(source.name, std::move(declaration));
    }
    return exports;
}

ModuleMethodExports methodExportsFromInterface(const ModuleInterface& interfaceInfo)
{
    ModuleMethodExports exports;
    for (const ModuleInterfaceStruct& structInfo : interfaceInfo.structs) {
        for (const ModuleInterfaceMethod& method : structInfo.methods) {
            exports[structInfo.name].emplace(
                method.name,
                MethodSignature{
                    method.receiverType,
                    method.parameterTypes,
                    method.returnType,
                    method.resolvedName,
                    method.genericParameters,
                    method.genericParameterConstraints});
        }
    }
    return exports;
}

ModuleOperatorExports operatorExportsFromInterface(const ModuleInterface& interfaceInfo)
{
    ModuleOperatorExports exports;
    for (const ModuleInterfaceStruct& structInfo : interfaceInfo.structs) {
        for (const ModuleInterfaceOperator& op : structInfo.operators) {
            exports[structInfo.name].emplace(
                op.symbol,
                OperatorSignature{
                    op.receiverType,
                    op.rightParameterType,
                    op.returnType,
                    op.genericParameters,
                    op.genericParameterConstraints,
                    op.resolvedName});
        }
    }
    return exports;
}

} // namespace

const ModuleStmt* TypeChecker::findModule(const Program& program, std::size_t moduleId) const
{
    for (const auto& statement : program.statements) {
        if (const auto* module = dynamic_cast<const ModuleStmt*>(statement.get())) {
            if (module->moduleId == moduleId) {
                return module;
            }
        }
    }
    return nullptr;
}

const ModuleInterface* TypeChecker::findModuleInterface(std::size_t moduleId) const
{
    const auto found = std::find_if(
        moduleInterfaces_.begin(),
        moduleInterfaces_.end(),
        [moduleId](const ModuleInterface& interfaceInfo) {
            return interfaceInfo.moduleId == moduleId;
        });
    return found == moduleInterfaces_.end() ? nullptr : &*found;
}


void TypeChecker::buildModuleInterface(const Program& program, const ModuleStmt& module)
{
    ModuleInterface interfaceInfo;
    interfaceInfo.moduleId = module.moduleId;
    interfaceInfo.sourceId = module.sourceId;
    interfaceInfo.path = module.path;
    interfaceInfo.isEntry = module.isEntry;
    interfaceInfo.resolvedNameNext = nextResolvedName_;
    if (program.moduleGraph) {
        const auto graphNode = std::find_if(
            program.moduleGraph->nodes.begin(),
            program.moduleGraph->nodes.end(),
            [&module](const ModuleGraphNode& node) { return node.moduleId == module.moduleId; });
        if (graphNode != program.moduleGraph->nodes.end()) {
            interfaceInfo.sourceId = graphNode->sourceId;
            interfaceInfo.canonicalPath = graphNode->canonicalPath;
        }
        for (const ModuleGraphEdge& edge : program.moduleGraph->edges) {
            if (edge.importingModuleId == module.moduleId) {
                interfaceInfo.dependencies.push_back(ModuleInterfaceDependency{
                    edge.importedModuleId,
                    edge.kind,
                    edge.requestedPath});
            }
        }
    }

    if (const ModuleValueExports* exports = moduleSymbols_.valueExports(module.moduleId)) {
        for (const auto& entry : *exports) {
            interfaceInfo.values.push_back(ModuleInterfaceValue{
                entry.first,
                entry.second.type,
                entry.second.resolvedName});
        }
    }

    if (const ModuleStructExports* structExports = moduleSymbols_.structExports(module.moduleId)) {
        for (const auto& entry : *structExports) {
            ModuleInterfaceStruct structInfo;
            structInfo.name = entry.first;
            structInfo.genericParameters = entry.second.genericParameters;
            structInfo.genericParameterConstraints = entry.second.genericParameterConstraints;
            structInfo.hasPrivateFields = entry.second.hasPrivateFields;
            structInfo.definingModuleId = entry.second.definingModuleId.value_or(module.moduleId);
            for (const StructFieldType& field : entry.second.fields) {
                if (field.isPrivate) {
                    continue;
                }
                structInfo.fields.push_back(ModuleInterfaceField{field.name.lexeme, field.type});
            }

            if (const ModuleMethodExports* methodExports = moduleSymbols_.methodExports(module.moduleId)) {
                const auto methodsForStruct = methodExports->find(entry.first);
                if (methodsForStruct != methodExports->end()) {
                    for (const auto& methodEntry : methodsForStruct->second) {
                        structInfo.methods.push_back(ModuleInterfaceMethod{
                            methodEntry.first,
                            methodEntry.second.parameterTypes,
                            methodEntry.second.returnType,
                            methodEntry.second.genericParameters,
                            methodEntry.second.genericParameterConstraints,
                            methodEntry.second.receiverType,
                            methodEntry.second.resolvedName});
                    }
                }
            }

            if (const ModuleOperatorExports* operatorExports = moduleSymbols_.operatorExports(module.moduleId)) {
                const auto operatorsForStruct = operatorExports->find(entry.first);
                if (operatorsForStruct != operatorExports->end()) {
                    for (const auto& operatorEntry : operatorsForStruct->second) {
                        structInfo.operators.push_back(ModuleInterfaceOperator{
                            operatorEntry.first,
                            operatorEntry.second.receiverType,
                            operatorEntry.second.rightParameterType,
                            operatorEntry.second.returnType,
                            operatorEntry.second.genericParameters,
                            operatorEntry.second.genericParameterConstraints,
                            operatorEntry.second.resolvedName});
                    }
                }
            }

            interfaceInfo.structs.push_back(std::move(structInfo));
        }
    }

    if (const ModuleEnumExports* enumExports = moduleSymbols_.enumExports(module.moduleId)) {
        for (const auto& entry : *enumExports) {
            ModuleInterfaceEnum enumInfo;
            enumInfo.name = entry.first;
            enumInfo.genericParameters = entry.second.genericParameters;
            for (const EnumVariantType& variant : entry.second.variants) {
                std::vector<std::optional<std::string>> payloadNames;
                payloadNames.reserve(variant.payloadNames.size());
                for (const std::optional<Token>& payloadName : variant.payloadNames) {
                    payloadNames.push_back(payloadName
                        ? std::optional<std::string>(payloadName->lexeme)
                        : std::nullopt);
                }
                enumInfo.variants.push_back(ModuleInterfaceVariant{
                    variant.name.lexeme,
                    variant.payloadTypes,
                    std::move(payloadNames)});
            }
            enumInfo.genericParameterConstraints = entry.second.genericParameterConstraints;
            interfaceInfo.enums.push_back(std::move(enumInfo));
        }
    }

    std::sort(
        interfaceInfo.values.begin(),
        interfaceInfo.values.end(),
        [](const ModuleInterfaceValue& left, const ModuleInterfaceValue& right) {
            return left.name < right.name;
        });
    std::sort(
        interfaceInfo.structs.begin(),
        interfaceInfo.structs.end(),
        [](const ModuleInterfaceStruct& left, const ModuleInterfaceStruct& right) {
            return left.name < right.name;
        });
    for (ModuleInterfaceStruct& structInfo : interfaceInfo.structs) {
        std::sort(
            structInfo.methods.begin(),
            structInfo.methods.end(),
            [](const ModuleInterfaceMethod& left, const ModuleInterfaceMethod& right) {
                return left.name < right.name;
            });
        std::sort(
            structInfo.operators.begin(),
            structInfo.operators.end(),
            [](const ModuleInterfaceOperator& left, const ModuleInterfaceOperator& right) {
                return left.symbol < right.symbol;
            });
    }
    std::sort(
        interfaceInfo.enums.begin(),
        interfaceInfo.enums.end(),
        [](const ModuleInterfaceEnum& left, const ModuleInterfaceEnum& right) {
            return left.name < right.name;
        });

    const auto existing = std::find_if(
        moduleInterfaces_.begin(),
        moduleInterfaces_.end(),
        [&module](const ModuleInterface& current) {
            return current.moduleId == module.moduleId;
        });
    if (existing == moduleInterfaces_.end()) {
        moduleInterfaces_.push_back(std::move(interfaceInfo));
    } else {
        *existing = std::move(interfaceInfo);
    }
}

void TypeChecker::buildModuleInterfaces(const Program& program)
{
    for (const auto& statement : program.statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (module && !findModuleInterface(module->moduleId)) {
            buildModuleInterface(program, *module);
        }
    }

    std::sort(
        moduleInterfaces_.begin(),
        moduleInterfaces_.end(),
        [](const ModuleInterface& left, const ModuleInterface& right) {
            return left.moduleId < right.moduleId;
        });
}

std::size_t TypeChecker::validateModuleInterfaces(const Program& program) const
{
    std::size_t mismatches = 0;
    const auto mismatch = [&mismatches]() { ++mismatches; };

    std::unordered_set<std::size_t> expectedModuleIds;
    for (const auto& statement : program.statements) {
        if (const auto* module = dynamic_cast<const ModuleStmt*>(statement.get())) {
            if (!expectedModuleIds.insert(module->moduleId).second) {
                mismatch();
            }
        }
    }

    if (moduleInterfaces_.size() != expectedModuleIds.size()) {
        mismatch();
    }
    for (std::size_t index = 1; index < moduleInterfaces_.size(); ++index) {
        if (moduleInterfaces_[index - 1].moduleId >= moduleInterfaces_[index].moduleId) {
            mismatch();
        }
    }

    if (program.moduleGraph) {
        if (program.moduleGraph->nodes.size() != expectedModuleIds.size()) {
            mismatch();
        }
        std::unordered_set<std::size_t> graphModuleIds;
        for (const ModuleGraphNode& node : program.moduleGraph->nodes) {
            if (!graphModuleIds.insert(node.moduleId).second
                || !expectedModuleIds.count(node.moduleId)) {
                mismatch();
            }
        }
    }

    const auto checkCanonicalNames = [&mismatch](const auto& records, const auto& nameOf) {
        for (std::size_t index = 1; index < records.size(); ++index) {
            if (nameOf(records[index - 1]) >= nameOf(records[index])) {
                mismatch();
            }
        }
    };

    const auto checkValueNames = [&mismatch](
        const std::vector<ModuleInterfaceValue>& actual,
        const ModuleValueExports* expected) {
        if (!expected) {
            if (!actual.empty()) {
                mismatch();
            }
            return;
        }
        if (actual.size() != expected->size()) {
            mismatch();
        }
        for (const ModuleInterfaceValue& value : actual) {
            if (expected->find(value.name) == expected->end()) {
                mismatch();
            }
        }
    };

    const auto checkStructNames = [&mismatch](
        const std::vector<ModuleInterfaceStruct>& actual,
        const ModuleStructExports* expected) {
        if (!expected) {
            if (!actual.empty()) {
                mismatch();
            }
            return;
        }
        if (actual.size() != expected->size()) {
            mismatch();
        }
        for (const ModuleInterfaceStruct& structure : actual) {
            if (expected->find(structure.name) == expected->end()) {
                mismatch();
            }
        }
    };

    const auto checkStructFieldVisibility = [&mismatch](
        const std::vector<ModuleInterfaceStruct>& actual,
        const ModuleStructExports* expected) {
        if (!expected) {
            return;
        }
        for (const ModuleInterfaceStruct& structure : actual) {
            const auto expectedStructure = expected->find(structure.name);
            if (expectedStructure == expected->end()) {
                continue;
            }
            const StructTypeDecl& declaration = expectedStructure->second;
            if (structure.hasPrivateFields != declaration.hasPrivateFields) {
                mismatch();
            }

            std::size_t publicFieldCount = 0;
            for (const StructFieldType& field : declaration.fields) {
                if (!field.isPrivate) {
                    ++publicFieldCount;
                }
            }
            if (structure.fields.size() != publicFieldCount) {
                mismatch();
            }
            for (const ModuleInterfaceField& field : structure.fields) {
                const auto expectedField = std::find_if(
                    declaration.fields.begin(),
                    declaration.fields.end(),
                    [&field](const StructFieldType& candidate) {
                        return candidate.name.lexeme == field.name;
                    });
                if (expectedField == declaration.fields.end() || expectedField->isPrivate) {
                    mismatch();
                }
            }
        }
    };

    const auto checkEnumNames = [&mismatch](
        const std::vector<ModuleInterfaceEnum>& actual,
        const ModuleEnumExports* expected) {
        if (!expected) {
            if (!actual.empty()) {
                mismatch();
            }
            return;
        }
        if (actual.size() != expected->size()) {
            mismatch();
        }
        for (const ModuleInterfaceEnum& enumeration : actual) {
            if (expected->find(enumeration.name) == expected->end()) {
                mismatch();
            }
        }
    };

    const auto checkMethodNames = [&mismatch](
        const std::vector<ModuleInterfaceStruct>& actual,
        const ModuleMethodExports* expected) {
        for (const ModuleInterfaceStruct& structure : actual) {
            const StructMethodTable* expectedMethods = nullptr;
            if (expected) {
                const auto found = expected->find(structure.name);
                if (found != expected->end()) {
                    expectedMethods = &found->second;
                }
            }
            if (!expectedMethods) {
                if (!structure.methods.empty()) {
                    mismatch();
                }
                continue;
            }
            if (structure.methods.size() != expectedMethods->size()) {
                mismatch();
            }
            for (const ModuleInterfaceMethod& method : structure.methods) {
                if (expectedMethods->find(method.name) == expectedMethods->end()) {
                    mismatch();
                }
            }
        }
        if (!expected) {
            return;
        }
        for (const auto& entry : *expected) {
            const auto structure = std::find_if(
                actual.begin(),
                actual.end(),
                [&entry](const ModuleInterfaceStruct& candidate) {
                    return candidate.name == entry.first;
                });
            if (structure == actual.end()) {
                mismatch();
            }
        }
    };

    const auto checkOperatorNames = [&mismatch](
        const std::vector<ModuleInterfaceStruct>& actual,
        const ModuleOperatorExports* expected) {
        for (const ModuleInterfaceStruct& structure : actual) {
            const StructOperatorTable* expectedOperators = nullptr;
            if (expected) {
                const auto found = expected->find(structure.name);
                if (found != expected->end()) {
                    expectedOperators = &found->second;
                }
            }
            if (!expectedOperators) {
                if (!structure.operators.empty()) {
                    mismatch();
                }
                continue;
            }
            if (structure.operators.size() != expectedOperators->size()) {
                mismatch();
            }
            for (const ModuleInterfaceOperator& op : structure.operators) {
                if (expectedOperators->find(op.symbol) == expectedOperators->end()) {
                    mismatch();
                }
            }
        }
        if (!expected) {
            return;
        }
        for (const auto& entry : *expected) {
            const auto structure = std::find_if(
                actual.begin(),
                actual.end(),
                [&entry](const ModuleInterfaceStruct& candidate) {
                    return candidate.name == entry.first;
                });
            if (structure == actual.end()) {
                mismatch();
            }
        }
    };

    for (const ModuleInterface& interfaceInfo : moduleInterfaces_) {
        if (!expectedModuleIds.count(interfaceInfo.moduleId)) {
            mismatch();
            continue;
        }

        const ModuleStmt* module = findModule(program, interfaceInfo.moduleId);
        if (!module) {
            mismatch();
            continue;
        }
        if (interfaceInfo.sourceId != module->sourceId
            || interfaceInfo.path != module->path
            || interfaceInfo.isEntry != module->isEntry) {
            mismatch();
        }

        if (program.moduleGraph) {
            const auto graphNode = std::find_if(
                program.moduleGraph->nodes.begin(),
                program.moduleGraph->nodes.end(),
                [&interfaceInfo](const ModuleGraphNode& node) {
                    return node.moduleId == interfaceInfo.moduleId;
                });
            if (graphNode == program.moduleGraph->nodes.end()) {
                mismatch();
            } else if (interfaceInfo.sourceId != graphNode->sourceId
                || interfaceInfo.path != graphNode->path
                || interfaceInfo.canonicalPath != graphNode->canonicalPath
                || interfaceInfo.isEntry != graphNode->isEntry) {
                mismatch();
            }

            std::vector<const ModuleGraphEdge*> expectedEdges;
            for (const ModuleGraphEdge& edge : program.moduleGraph->edges) {
                if (edge.importingModuleId == interfaceInfo.moduleId) {
                    expectedEdges.push_back(&edge);
                }
            }
            if (interfaceInfo.dependencies.size() != expectedEdges.size()) {
                mismatch();
            }
            const std::size_t edgeCount = std::min(
                interfaceInfo.dependencies.size(),
                expectedEdges.size());
            for (std::size_t index = 0; index < edgeCount; ++index) {
                const ModuleInterfaceDependency& actual = interfaceInfo.dependencies[index];
                const ModuleGraphEdge& expected = *expectedEdges[index];
                if (actual.importedModuleId != expected.importedModuleId
                    || actual.kind != expected.kind
                    || actual.requestedPath != expected.requestedPath) {
                    mismatch();
                }
            }
        } else if (!interfaceInfo.canonicalPath.empty() || !interfaceInfo.dependencies.empty()) {
            mismatch();
        }

        checkCanonicalNames(
            interfaceInfo.values,
            [](const ModuleInterfaceValue& value) { return value.name; });
        checkCanonicalNames(
            interfaceInfo.structs,
            [](const ModuleInterfaceStruct& structure) { return structure.name; });
        checkCanonicalNames(
            interfaceInfo.enums,
            [](const ModuleInterfaceEnum& enumeration) { return enumeration.name; });
        for (const ModuleInterfaceStruct& structure : interfaceInfo.structs) {
            checkCanonicalNames(
                structure.methods,
                [](const ModuleInterfaceMethod& method) { return method.name; });
            checkCanonicalNames(
                structure.operators,
                [](const ModuleInterfaceOperator& op) { return op.symbol; });
        }

        if (preloadedModuleIds_.find(interfaceInfo.moduleId) != preloadedModuleIds_.end()) {
            continue;
        }

        checkValueNames(interfaceInfo.values, moduleSymbols_.valueExports(interfaceInfo.moduleId));
        checkStructNames(interfaceInfo.structs, moduleSymbols_.structExports(interfaceInfo.moduleId));
        checkStructFieldVisibility(
            interfaceInfo.structs,
            moduleSymbols_.structExports(interfaceInfo.moduleId));
        checkEnumNames(interfaceInfo.enums, moduleSymbols_.enumExports(interfaceInfo.moduleId));
        checkMethodNames(interfaceInfo.structs, moduleSymbols_.methodExports(interfaceInfo.moduleId));
        checkOperatorNames(interfaceInfo.structs, moduleSymbols_.operatorExports(interfaceInfo.moduleId));
    }

    return mismatches;
}

void TypeChecker::checkModule(const ModuleStmt& module)
{
    if (checkedModules_.find(module.moduleId) != checkedModules_.end()) {
        return;
    }
    if (preloadedModuleIds_.find(module.moduleId) != preloadedModuleIds_.end()) {
        throw TypeError("internal error: preloaded module entered body-check path");
    }

    std::vector<Scope> savedScopes = std::move(scopes_);
    std::unordered_map<DeclarationId, Binding*, SnapshotIdHash<DeclarationIdTag>> savedBindingsById
        = std::move(bindingsById_);
    std::unordered_map<std::string, StructTypeDecl> savedStructTypes = std::move(structTypes_);
    std::unordered_map<std::string, const StructDeclStmt*> savedStructDeclarations = std::move(structDeclarations_);
    std::unordered_map<std::string, StructCheckState> savedStructCheckStates = std::move(structCheckStates_);
    std::unordered_map<std::string, EnumTypeDecl> savedEnumTypes = std::move(enumTypes_);
    std::unordered_map<std::string, const EnumDeclStmt*> savedEnumDeclarations = std::move(enumDeclarations_);
    std::unordered_map<std::string, EnumCheckState> savedEnumCheckStates = std::move(enumCheckStates_);
    MethodTable savedMethods = std::move(methods_);
    std::vector<std::unordered_map<std::string, TypeInfo>> savedTypeParameterScopes = std::move(typeParameterScopes_);
    const std::size_t savedFunctionDepth = functionDepth_;
    const std::size_t savedLoopDepth = loopDepth_;
    std::vector<FunctionReturnContext> savedReturnContexts = std::move(returnContexts_);
    ModuleSymbols savedModuleSymbols = moduleSymbols_;
    const FlowFacts savedFlowFacts = flowFacts_;
    const std::vector<std::size_t> savedModuleStack = moduleStack_;

    const auto restoreTransientState = [&]() {
        scopes_ = std::move(savedScopes);
        bindingsById_ = std::move(savedBindingsById);
        structTypes_ = std::move(savedStructTypes);
        structDeclarations_ = std::move(savedStructDeclarations);
        structCheckStates_ = std::move(savedStructCheckStates);
        enumTypes_ = std::move(savedEnumTypes);
        enumDeclarations_ = std::move(savedEnumDeclarations);
        enumCheckStates_ = std::move(savedEnumCheckStates);
        methods_ = std::move(savedMethods);
        typeParameterScopes_ = std::move(savedTypeParameterScopes);
        functionDepth_ = savedFunctionDepth;
        loopDepth_ = savedLoopDepth;
        returnContexts_ = std::move(savedReturnContexts);
        flowFacts_ = savedFlowFacts;
        moduleStack_ = savedModuleStack;
    };
    const auto restoreFailedState = [&]() {
        restoreTransientState();
        moduleSymbols_ = std::move(savedModuleSymbols);
    };

    scopes_.clear();
    bindingsById_.clear();
    structTypes_.clear();
    structDeclarations_.clear();
    structCheckStates_.clear();
    enumTypes_.clear();
    enumDeclarations_.clear();
    enumCheckStates_.clear();
    methods_.clear();
    typeParameterScopes_.clear();
    functionDepth_ = 0;
    loopDepth_ = 0;
    returnContexts_.clear();
    flowFacts_.clear();

    moduleStack_.push_back(module.moduleId);
    beginScope();
    try {
        checkStatementList(module.statements);
        endScope();
        moduleStack_.pop_back();
        buildModuleInterface(*currentProgram_, module);
        checkedModules_.insert(module.moduleId);
        checkedModuleBodyIds_.push_back(module.moduleId);
        restoreTransientState();
    } catch (const FileDiagnosticError&) {
        restoreFailedState();
        throw;
    } catch (const DiagnosticError& error) {
        if (error.location()) {
            FileDiagnosticError contextual(
                error,
                DiagnosticSourceContext{
                    module.path,
                    module.source,
                    false});
            restoreFailedState();
            throw contextual;
        }
        restoreFailedState();
        throw;
    } catch (...) {
        restoreFailedState();
        throw;
    }
}

void TypeChecker::checkModulesInDependencyOrder(const Program& program)
{
    if (!program.moduleGraph) {
        throw TypeError("internal error: module dependency graph is unavailable");
    }

    enum class VisitState {
        Visiting,
        Checked,
        Failed,
        Skipped,
    };
    std::unordered_map<std::size_t, VisitState> states;
    std::vector<FileDiagnosticError> errors;
    const std::function<VisitState(std::size_t)> visit = [&](std::size_t moduleId) -> VisitState {
        const auto existing = states.find(moduleId);
        if (existing != states.end()) {
            if (existing->second == VisitState::Visiting) {
                throw TypeError("internal error: module dependency graph contains a cycle");
            }
            return existing->second;
        }

        states.emplace(moduleId, VisitState::Visiting);
        bool dependencyFailed = false;
        for (const ModuleGraphEdge& edge : program.moduleGraph->edges) {
            if (edge.importingModuleId == moduleId) {
                const VisitState dependencyState = visit(edge.importedModuleId);
                dependencyFailed = dependencyFailed
                    || dependencyState == VisitState::Failed
                    || dependencyState == VisitState::Skipped;
            }
        }

        if (dependencyFailed) {
            states[moduleId] = VisitState::Skipped;
            return VisitState::Skipped;
        }

        if (preloadedModuleIds_.find(moduleId) != preloadedModuleIds_.end()) {
            if (!findModuleInterface(moduleId)) {
                throw TypeError("internal error: preloaded module interface is unavailable");
            }
            // A validated sidecar is the semantic boundary for this module:
            // its public interface is already available, so do not enter the
            // source-body checker or create an empty replacement interface.
            states[moduleId] = VisitState::Checked;
            return VisitState::Checked;
        }

        const ModuleStmt* module = findModule(program, moduleId);
        if (!module) {
            throw TypeError("internal error: unresolved module graph node");
        }

        try {
            checkModule(*module);
        } catch (const FileDiagnosticError& error) {
            if (error.kind() != DiagnosticKind::Type || !error.location()) {
                throw;
            }
            errors.push_back(error);
            states[moduleId] = VisitState::Failed;
            return VisitState::Failed;
        } catch (const DiagnosticError& error) {
            if (error.kind() != DiagnosticKind::Type || !error.location()) {
                throw;
            }
            errors.emplace_back(
                error,
                DiagnosticSourceContext{
                    module->path,
                    module->source,
                    false});
            states[moduleId] = VisitState::Failed;
            return VisitState::Failed;
        }
        states[moduleId] = VisitState::Checked;
        return VisitState::Checked;
    };

    for (const ModuleGraphNode& node : program.moduleGraph->nodes) {
        visit(node.moduleId);
    }

    // Keep the module graph as the scheduling authority while retaining a
    // defensive path for a snapshot whose statement set contains a module
    // that is not yet represented by a graph node.
    for (const auto& statement : program.statements) {
        const auto* module = dynamic_cast<const ModuleStmt*>(statement.get());
        if (!module) {
            continue;
        }
        if (states.find(module->moduleId) == states.end()) {
            static_cast<void>(visit(module->moduleId));
        }
    }

    if (!errors.empty()) {
        throw FileDiagnosticErrorList(std::move(errors));
    }
}

const NamespaceImport* TypeChecker::findNamespace(const std::string& alias) const
{
    if (moduleStack_.empty()) {
        return nullptr;
    }
    return moduleSymbols_.namespaceImport(moduleStack_.back(), alias);
}

std::string TypeChecker::qualifiedStructName(const Token& qualifier, const Token& name) const
{
    return qualifier.lexeme + "." + name.lexeme;
}

std::string TypeChecker::structConstructorTypeName(const StructConstructExpr& expression) const
{
    if (expression.qualifier) {
        return qualifiedStructName(*expression.qualifier, expression.name);
    }
    return expression.name.lexeme;
}

std::string TypeChecker::enumConstructorTypeName(const MemberCallExpr& expression) const
{
    const auto* receiver = dynamic_cast<const VariableExpr*>(expression.receiver.get());
    if (receiver && findEnumType(receiver->name.lexeme)) {
        return receiver->name.lexeme;
    }

    const auto* qualifiedReceiver = dynamic_cast<const FieldAccessExpr*>(expression.receiver.get());
    const auto* namespaceVariable = qualifiedReceiver
        ? dynamic_cast<const VariableExpr*>(qualifiedReceiver->object.get())
        : nullptr;
    if (namespaceVariable) {
        const NamespaceImport* namespaceImport = findNamespace(namespaceVariable->name.lexeme);
        if (namespaceImport
            && namespaceImport->enums.find(qualifiedReceiver->name.lexeme) != namespaceImport->enums.end()) {
            return namespaceVariable->name.lexeme + "." + qualifiedReceiver->name.lexeme;
        }
    }
    return {};
}

void TypeChecker::declareNamespaceAlias(const ImportStmt& statement, NamespaceImport imported)
{
    if (!statement.alias) {
        throw TypeError(statement.keyword, "internal error: namespace import without alias");
    }

    const Token& alias = *statement.alias;
    if (moduleStack_.empty()) {
        throw TypeError(statement.keyword, "namespace imports require a module context");
    }

    const std::size_t moduleId = moduleStack_.back();
    if (moduleSymbols_.hasNamespace(moduleId, alias.lexeme)
        || currentScope().find(alias.lexeme) != currentScope().end()
        || structTypes_.find(alias.lexeme) != structTypes_.end()
        || enumTypes_.find(alias.lexeme) != enumTypes_.end()) {
        throw TypeError(alias, "namespace alias `" + alias.lexeme + "` conflicts with an existing declaration");
    }

    for (const auto& entry : imported.structs) {
        StructTypeDecl qualified = entry.second;
        qualified.name.lexeme = alias.lexeme + "." + entry.first;
        for (std::shared_ptr<TypeInfo>& constraint : qualified.genericParameterConstraints) {
            if (constraint) {
                constraint = std::make_shared<TypeInfo>(
                    qualifyNamespaceType(*constraint, alias.lexeme, imported.structs, imported.enums));
            }
        }
        for (StructFieldType& field : qualified.fields) {
            field.type = qualifyNamespaceType(field.type, alias.lexeme, imported.structs, imported.enums);
        }
        structTypes_.emplace(qualified.name.lexeme, std::move(qualified));
    }

    for (const auto& entry : imported.enums) {
        EnumTypeDecl qualified = entry.second;
        qualified.name.lexeme = alias.lexeme + "." + entry.first;
        for (std::shared_ptr<TypeInfo>& constraint : qualified.genericParameterConstraints) {
            if (constraint) {
                constraint = std::make_shared<TypeInfo>(
                    qualifyNamespaceType(*constraint, alias.lexeme, imported.structs, imported.enums));
            }
        }
        for (EnumVariantType& variant : qualified.variants) {
            for (TypeInfo& payload : variant.payloadTypes) {
                payload = qualifyNamespaceType(payload, alias.lexeme, imported.structs, imported.enums);
            }
        }
        enumTypes_.emplace(qualified.name.lexeme, std::move(qualified));
    }

    for (auto& entry : imported.values) {
        entry.second.type = qualifyNamespaceType(entry.second.type, alias.lexeme, imported.structs, imported.enums);
    }

    importMethodExports(alias, imported.methods, &alias.lexeme, &imported.structs, &imported.enums);
    importOperatorExports(alias, imported.operators, &alias.lexeme, &imported.structs, &imported.enums);
    moduleSymbols_.recordNamespace(moduleId, alias.lexeme, std::move(imported));
}

void TypeChecker::checkImport(const ImportStmt& statement)
{
    if (moduleStack_.empty()) {
        throw TypeError(statement.keyword, "import declarations must be loaded before parsing");
    }

    const std::size_t currentModuleId = moduleStack_.back();
    if (!statement.alias && !moduleSymbols_.markDirectImport(currentModuleId, statement.resolvedModuleId)) {
        return;
    }

    const ModuleInterface* importedInterface = findModuleInterface(statement.resolvedModuleId);
    if (!importedInterface) {
        throw TypeError(statement.keyword, "internal error: unresolved imported module interface");
    }

    if (statement.alias) {
        NamespaceImport namespaceImport;
        namespaceImport.values = valueExportsFromInterface(*importedInterface);
        namespaceImport.structs = structExportsFromInterface(*importedInterface, statement.keyword);
        namespaceImport.enums = enumExportsFromInterface(*importedInterface, statement.keyword);
        namespaceImport.methods = methodExportsFromInterface(*importedInterface);
        namespaceImport.operators = operatorExportsFromInterface(*importedInterface);
        declareNamespaceAlias(statement, std::move(namespaceImport));
        return;
    }

    const ModuleValueExports values = valueExportsFromInterface(*importedInterface);
    for (const auto& entry : values) {
        Token name{TokenType::Identifier, entry.first, statement.keyword.line, statement.keyword.column};
        declareImportedVariable(name, entry.second);
    }

    const ModuleStructExports structs = structExportsFromInterface(*importedInterface, statement.keyword);
    for (const auto& entry : structs) {
        if (structTypes_.find(entry.first) != structTypes_.end()) {
            Token name{TokenType::Identifier, entry.first, statement.keyword.line, statement.keyword.column};
            throw TypeError(name, "duplicate struct `" + entry.first + "`");
        }
        structTypes_.emplace(entry.first, entry.second);
    }

    const ModuleEnumExports enums = enumExportsFromInterface(*importedInterface, statement.keyword);
    for (const auto& entry : enums) {
        if (enumTypes_.find(entry.first) != enumTypes_.end()
            || structTypes_.find(entry.first) != structTypes_.end()) {
            Token name{TokenType::Identifier, entry.first, statement.keyword.line, statement.keyword.column};
            throw TypeError(name, "duplicate type " + entry.first);
        }
        enumTypes_.emplace(entry.first, entry.second);
    }

    const ModuleMethodExports methods = methodExportsFromInterface(*importedInterface);
    importMethodExports(statement.keyword, methods);
    const ModuleOperatorExports operators = operatorExportsFromInterface(*importedInterface);
    importOperatorExports(statement.keyword, operators);
}

std::string TypeChecker::sourcePathLabel(const Token& path) const
{
    if (path.lexeme.size() >= 2 && path.lexeme.front() == '"' && path.lexeme.back() == '"') {
        return path.lexeme.substr(1, path.lexeme.size() - 2);
    }
    return path.lexeme;
}

void TypeChecker::ensureExportNameAvailable(std::size_t moduleId, const Token& name) const
{
    if (moduleSymbols_.hasAnyExport(moduleId, name.lexeme)) {
        throw TypeError(name, "duplicate export `" + name.lexeme + "`");
    }
}

void TypeChecker::forwardStructMethodExports(
    const ModuleMethodExports& targetExports,
    std::size_t currentModuleId,
    const std::string& structName)
{
    const auto found = targetExports.find(structName);
    if (found == targetExports.end()) {
        return;
    }
    moduleSymbols_.recordMethodExports(currentModuleId, structName, found->second);
}

void TypeChecker::forwardStructOperatorExports(
    const ModuleOperatorExports& targetExports,
    std::size_t currentModuleId,
    const std::string& structName)
{
    const auto found = targetExports.find(structName);
    if (found == targetExports.end()) {
        return;
    }
    moduleSymbols_.recordOperatorExports(currentModuleId, structName, found->second);
}

void TypeChecker::checkReExport(const ExportStmt& statement)
{
    if (moduleStack_.empty()) {
        throw TypeError(statement.keyword, "re-export declarations require a module context");
    }
    if (!currentProgram_) {
        throw TypeError(statement.keyword, "internal error: re-export without program");
    }
    if (!statement.sourcePath) {
        throw TypeError(statement.keyword, "internal error: re-export without source path");
    }

    const std::size_t currentModuleId = moduleStack_.back();
    const ModuleInterface* targetInterface = findModuleInterface(statement.resolvedModuleId);
    if (!targetInterface) {
        throw TypeError(statement.keyword, "internal error: unresolved re-export module interface");
    }

    const ModuleValueExports valueExports = valueExportsFromInterface(*targetInterface);
    const ModuleStructExports structExports = structExportsFromInterface(*targetInterface, statement.keyword);
    const ModuleEnumExports enumExports = enumExportsFromInterface(*targetInterface, statement.keyword);
    const ModuleMethodExports methodExports = methodExportsFromInterface(*targetInterface);
    const ModuleOperatorExports operatorExports = operatorExportsFromInterface(*targetInterface);

    for (const Token& name : statement.names) {
        ensureExportNameAvailable(currentModuleId, name);

        bool exported = false;
        const auto value = valueExports.find(name.lexeme);
        if (value != valueExports.end()) {
            moduleSymbols_.recordValueExport(currentModuleId, name.lexeme, value->second);
            exported = true;
        }

        const auto structure = structExports.find(name.lexeme);
        if (structure != structExports.end()) {
            moduleSymbols_.recordStructExport(currentModuleId, name.lexeme, structure->second);
            forwardStructMethodExports(methodExports, currentModuleId, name.lexeme);
            forwardStructOperatorExports(operatorExports, currentModuleId, name.lexeme);
            exported = true;
        }

        const auto enumeration = enumExports.find(name.lexeme);
        if (enumeration != enumExports.end()) {
            moduleSymbols_.recordEnumExport(currentModuleId, name.lexeme, enumeration->second);
            exported = true;
        }

        if (!exported) {
            throw TypeError(name,
                "module `" + sourcePathLabel(*statement.sourcePath) + "` has no exported name `" + name.lexeme + "`");
        }
    }
}

void TypeChecker::checkExport(const ExportStmt& statement)
{
    if (statement.sourcePath) {
        checkReExport(statement);
        return;
    }

    const bool inModule = !moduleStack_.empty();
    const std::size_t moduleId = inModule ? moduleStack_.back() : 0;

    for (const auto& name : statement.names) {
        bool exported = false;

        if (inModule) {
            ensureExportNameAvailable(moduleId, name);
        }

        if (Binding* binding = findVariable(name.lexeme)) {
            if (binding->scopeDepth == 0 && !binding->imported) {
                if (inModule) {
                    moduleSymbols_.recordValueExport(moduleId, name.lexeme, *binding);
                }
                exported = true;
            }
        }

        if (inModule) {
            if (moduleSymbols_.isLocalStruct(moduleId, name.lexeme)) {
                if (const StructTypeDecl* structType = findStructType(name.lexeme)) {
                    moduleSymbols_.recordStructExport(moduleId, name.lexeme, *structType);
                    recordStructMethodExports(moduleId, name.lexeme);
                    exported = true;
                }
            }
            if (moduleSymbols_.isLocalEnum(moduleId, name.lexeme)) {
                if (const EnumTypeDecl* enumType = findEnumType(name.lexeme)) {
                    moduleSymbols_.recordEnumExport(moduleId, name.lexeme, *enumType);
                    exported = true;
                }
            }
        } else if (findStructType(name.lexeme)) {
            exported = true;
        } else if (findEnumType(name.lexeme)) {
            exported = true;
        }

        if (!exported) {
            throw TypeError(name, "cannot export undefined name `" + name.lexeme + "`");
        }
    }
}
