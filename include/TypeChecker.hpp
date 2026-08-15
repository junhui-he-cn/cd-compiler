#pragma once

#include "Ast.hpp"
#include "DeclarationIndex.hpp"
#include "Diagnostic.hpp"
#include "ModuleInterface.hpp"
#include "ModuleSymbols.hpp"
#include "Token.hpp"
#include "TypeUtils.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class TypeError final : public DiagnosticError {
public:
    explicit TypeError(std::string message);
    TypeError(const Token& token, std::string message);
};

class TypeChecker {
public:
    void setPreloadedModuleInterfaces(std::vector<ModuleInterface> interfaces);
    void check(const Program& program);
    const std::vector<ModuleInterface>& moduleInterfaces() const;
    // Snapshot-local module IDs whose source bodies completed semantic
    // checking. Preloaded interface modules are intentionally absent; this is
    // migration observability rather than a persistent identity.
    const std::vector<std::size_t>& checkedModuleBodyIds() const;
    const DeclarationIndex& declarationIndex() const;
    std::size_t declarationIndexMismatchCount() const;
    std::size_t moduleInterfaceMismatchCount() const;

private:
    struct CheckedExpression {
        TypeInfo type;
    };

    struct PatternBindingInfo {
        Token token;
        TypeInfo type;
        std::vector<const VariablePattern*> occurrences;
    };

    using PatternBindings = std::map<std::string, PatternBindingInfo>;

    using Binding = TypeBinding;
    using StructFieldType = ::StructFieldType;
    using StructTypeDecl = ::StructTypeDecl;
    using EnumVariantType = ::EnumVariantType;
    using EnumTypeDecl = ::EnumTypeDecl;
    struct FunctionReturnContext {
        bool sawReturn = false;
        TypeInfo returnType;
        std::optional<TypeInfo> expectedReturnType;
    };

    struct MethodInfo {
        const MethodDecl* declaration = nullptr;
        TypeInfo receiverType;
        std::vector<TypeInfo> parameterTypes;
        TypeInfo returnType;
        std::string resolvedName;
        std::vector<std::string> genericParameters;
        std::vector<std::shared_ptr<TypeInfo>> genericParameterConstraints;
    };

    struct IndexTargetTypes {
        TypeInfo collection;
        TypeInfo index;
    };

    using Scope = std::unordered_map<std::string, Binding>;
    using MethodTable = std::unordered_map<std::string, std::unordered_map<std::string, MethodInfo>>;

    enum class StructCheckState {
        Declared,
        Checking,
        Checked,
    };

    enum class EnumCheckState {
        Declared,
        Checking,
        Checked,
    };

    void beginScope();
    void endScope();
    void beginTypeParameterScope(const std::vector<TypeParameter>& parameters);
    void endTypeParameterScope();
    Scope& currentScope();
    const Scope& currentScope() const;
    Binding* findVariable(const std::string& name);
    Binding* findSimpleVariableBinding(const Expr& expression);
    const Binding* findSimpleVariableBinding(const Expr& expression) const;
    const Binding* findVariable(const std::string& name) const;
    Binding* resolveVariableReference(const VariableExpr& expression);
    const Binding* resolveVariableReference(const VariableExpr& expression) const;
    Binding* resolveAssignmentTarget(const AssignExpr& expression);
    Binding* resolveCompoundAssignmentTarget(const CompoundAssignExpr& expression);
    Binding* bindingById(DeclarationId id);
    const Binding* bindingById(DeclarationId id) const;
    Binding declareVariable(
        const Token& name,
        TypeInfo type,
        bool explicitType = false,
        bool mutableBinding = false,
        const DeclarationRecord* record = nullptr);
    Binding declareVariable(
        const LetStmt& statement,
        TypeInfo type,
        bool explicitType = false);
    Binding declareImportedVariable(const Token& name, const Binding& importedBinding);
    std::string makeResolvedName(const std::string& sourceName);
    const NamespaceImport* findNamespace(const std::string& alias) const;
    void declareNamespaceAlias(const ImportStmt& statement, NamespaceImport imported);
    std::string qualifiedStructName(const Token& qualifier, const Token& name) const;
    std::string structConstructorTypeName(const StructConstructExpr& expression) const;
    std::string enumConstructorTypeName(const MemberCallExpr& expression) const;
    const EnumTypeDecl* findEnumType(const std::string& name) const;
    const EnumVariantType* findEnumVariant(const EnumTypeDecl& enumType, const std::string& name) const;
    TypeInfo resolveNamedStructAnnotation(
        const TypeAnnotation& typeName,
        std::string structName,
        const StructTypeDecl& structType) const;
    TypeInfo resolveNamedEnumAnnotation(
        const TypeAnnotation& typeName,
        std::string enumName,
        const EnumTypeDecl& enumType) const;

    void checkStatement(const Stmt& statement);
    void predeclareStructDeclarations(const std::vector<StmtPtr>& statements);
    void resolvePredeclaredStructParameters(const std::vector<StmtPtr>& statements);
    void predeclareEnumDeclarations(const std::vector<StmtPtr>& statements);
    void resolvePredeclaredEnumParameters(const std::vector<StmtPtr>& statements);
    void checkStatementList(const std::vector<StmtPtr>& statements);
    void checkModule(const ModuleStmt& module);
    void checkImport(const ImportStmt& statement);
    void checkExport(const ExportStmt& statement);
    void checkModulesInDependencyOrder(const Program& program);
    std::string sourcePathLabel(const Token& path) const;
    void ensureExportNameAvailable(std::size_t moduleId, const Token& name) const;
    void forwardStructMethodExports(
        const ModuleMethodExports& targetExports,
        std::size_t currentModuleId,
        const std::string& structName);
    void checkReExport(const ExportStmt& statement);
    const ModuleStmt* findModule(const Program& program, std::size_t moduleId) const;
    const ModuleInterface* findModuleInterface(std::size_t moduleId) const;
    void buildModuleInterface(const Program& program, const ModuleStmt& module);
    void buildModuleInterfaces(const Program& program);
    std::size_t validateModuleInterfaces(const Program& program) const;
    void checkStructDeclaration(const StructDeclStmt& statement);
    void checkEnumDeclaration(const EnumDeclStmt& statement);
    void checkImpl(const ImplStmt& statement);
    std::vector<TypeInfo> resolveParameterTypes(const std::vector<Parameter>& parameters);
    std::optional<TypeInfo> resolveOptionalReturnType(const std::optional<TypeAnnotation>& returnTypeName);
    void checkMethodNameAvailable(const StructTypeDecl& structType, const ImplStmt& statement, const MethodDecl& method) const;
    void registerMethodSignature(const StructTypeDecl& structType, const ImplStmt& statement, const MethodDecl& method);
    void checkMethodBody(const std::string& structName, const MethodInfo& method);
    CheckedExpression checkStructMethodCall(const MemberCallExpr& expression, const TypeInfo& receiverType);
    const MethodInfo* findMethod(const std::string& structName, const std::string& methodName) const;
    MethodSignature methodSignatureFromInfo(const MethodInfo& method) const;
    MethodInfo methodInfoFromSignature(const MethodSignature& signature) const;
    TypeInfo qualifyNamespaceType(
        const TypeInfo& type,
        const std::string& alias,
        const ModuleStructExports& structs,
        const ModuleEnumExports& enums) const;
    MethodSignature qualifyNamespaceMethodSignature(
        const MethodSignature& signature,
        const std::string& alias,
        const ModuleStructExports& structs,
        const ModuleEnumExports& enums) const;
    void importMethodExports(
        const Token& diagnosticToken,
        const ModuleMethodExports& methodExports,
        const std::string* namespaceAlias = nullptr,
        const ModuleStructExports* namespaceStructs = nullptr,
        const ModuleEnumExports* namespaceEnums = nullptr);
    void recordStructMethodExports(std::size_t moduleId, const std::string& structName);
    bool isBuiltinMemberName(const std::string& name) const;
    const StructTypeDecl* findStructType(const std::string& name) const;
    void predeclareStructDeclaration(const StructDeclStmt& statement);
    TypeInfo resolveStructFieldAnnotation(const StructFieldDecl& field);
    TypeInfo resolveStructFieldAnnotation(const TypeAnnotation& typeName, const Token& fieldName);
    TypeInfo resolveSimpleStructFieldAnnotation(const TypeAnnotation& typeName, const Token& fieldName);
    void checkFunction(const FunctionStmt& statement);
    std::vector<std::string> typeParameterNames(const std::vector<TypeParameter>& parameters) const;
    std::vector<std::shared_ptr<TypeInfo>> typeParameterConstraints(
        const std::vector<TypeParameter>& parameters) const;
    const TypeInfo* findTypeParameter(const std::string& name) const;
    bool hasEscapingTypeParameter(
        const TypeInfo& type,
        const std::unordered_set<std::string>& allowed) const;
    void inferTypeArguments(
        const TypeInfo& expected,
        const TypeInfo& actual,
        TypeSubstitutions& substitutions,
        const Token& callToken) const;
    void validateGenericTypeArguments(
        const std::vector<std::string>& parameters,
        const std::vector<std::shared_ptr<TypeInfo>>& constraints,
        const TypeSubstitutions& substitutions,
        const Token& callToken,
        const std::string& context) const;
    bool satisfiesCapabilityWitness(const TypeInfo& actual, const TypeInfo& capability) const;
    TypeInfo specializeGenericCallback(
        const Token& callToken,
        const TypeInfo& callbackType,
        const std::vector<TypeInfo>& argumentTypes,
        const std::string& functionName) const;
    CheckedExpression checkFunctionCall(
        const Token& callToken,
        const TypeInfo& calleeType,
        const std::vector<TypeAnnotation>& typeArguments,
        const std::vector<ExprPtr>& arguments);
    TypeInfo checkFunctionBody(
        const std::vector<StmtPtr>& body,
        std::optional<TypeInfo> expectedReturnType,
        const Token& functionToken,
        const std::string& functionLabel);
    void recordReturn(const Token& keyword, TypeInfo type);
    bool bodyMayFallThrough(const std::vector<StmtPtr>& body) const;
    bool statementMayFallThrough(const Stmt& statement) const;
    void checkImplicitNilReturn(const Token& functionToken, const std::string& functionLabel, const TypeInfo& expectedReturnType) const;
    TypeInfo checkExpression(const Expr& expression);
    TypeInfo checkCondition(const Expr& expression, const Token& keyword);
    CheckedExpression checkExpressionInfo(const Expr& expression);
    CheckedExpression checkExpressionInfo(const Expr& expression, const TypeInfo* expectedType);
    TypeInfo variableType(const Binding& binding) const;
    CheckedExpression checkArrayLiteral(const ArrayExpr& expression, const TypeInfo* expectedType);
    TypeInfo inferArrayElementType(const ArrayExpr& expression);
    CheckedExpression checkMapLiteral(const MapExpr& expression, const TypeInfo* expectedType);
    TypeInfo inferMapType(const MapExpr& expression);
    void refineArrayBindingFromMutation(Binding& target, const TypeInfo& valueType);
    const TypeInfo* contextualFunctionType(const TypeInfo* expectedType) const;
    CheckedExpression checkFunctionExpression(const FunctionExpr& expression, const TypeInfo* expectedType);
    CheckedExpression checkCall(const CallExpr& expression);
    CheckedExpression checkMemberCall(
        const MemberCallExpr& expression,
        const TypeInfo* expectedType = nullptr);
    bool isBuiltinLenCall(const CallExpr& expression) const;
    CheckedExpression checkBuiltinLenCall(const CallExpr& expression);
    bool isNativeStdlibCall(const CallExpr& expression) const;
    CheckedExpression checkArrayMap(
        const Token& callToken,
        const TypeInfo& arrayTypeInfo,
        const Expr& callbackExpression);
    CheckedExpression checkArrayFilter(
        const Token& callToken,
        const TypeInfo& arrayTypeInfo,
        const Expr& predicateExpression);
    CheckedExpression checkArrayFlatMap(
        const Token& callToken,
        const TypeInfo& arrayTypeInfo,
        const Expr& callbackExpression);
    void checkArrayPredicate(
        const Token& callToken,
        const TypeInfo& arrayTypeInfo,
        const Expr& predicateExpression,
        const std::string& functionName);
    CheckedExpression checkArrayAnyAll(
        const Token& callToken,
        const TypeInfo& arrayTypeInfo,
        const Expr& predicateExpression,
        const std::string& functionName);
    CheckedExpression checkArrayCount(
        const Token& callToken,
        const TypeInfo& arrayTypeInfo,
        const Expr& predicateExpression);
    CheckedExpression checkArrayFind(
        const Token& callToken,
        const TypeInfo& arrayTypeInfo,
        const Expr& predicateExpression);
    CheckedExpression checkArrayFindIndex(
        const Token& callToken,
        const TypeInfo& arrayTypeInfo,
        const Expr& predicateExpression);
    CheckedExpression checkArrayReduce(
        const Token& callToken,
        const TypeInfo& arrayTypeInfo,
        const Expr& initialExpression,
        const Expr& callbackExpression);
    CheckedExpression checkMapMerge(
        const Token& callToken,
        const TypeInfo& leftType,
        const TypeInfo& rightType);
    CheckedExpression checkNativeStdlibCall(const CallExpr& expression);
    IndexTargetTypes checkIndexTarget(
        const Expr& collection,
        const Expr& index,
        const Token& bracket,
        const std::string& nonArrayMessage);
    TypeInfo checkIndex(const IndexExpr& expression);
    CheckedExpression checkIndexAssignment(const IndexAssignExpr& expression);
    CheckedExpression checkIndexCompoundAssignment(const IndexCompoundAssignExpr& expression);
    std::optional<TypeInfo> checkStructFieldTarget(
        const Expr& object,
        const Token& name,
        const std::string& nonStructMessage);
    CheckedExpression checkFieldAssignment(const FieldAssignExpr& expression);
    CheckedExpression checkFieldCompoundAssignment(const FieldCompoundAssignExpr& expression);
    void checkKnownNumber(const Token& token, const TypeInfo& type, const std::string& messagePrefix) const;
    CheckedExpression checkNamedStructFields(
        const Token& diagnosticToken,
        const TypeInfo& declared,
        const std::vector<StructField>& fields);
    CheckedExpression checkStructConstructor(
        const StructConstructExpr& expression,
        const TypeInfo* expectedType = nullptr);
    CheckedExpression checkVariantConstructor(
        const MemberCallExpr& expression,
        const TypeInfo* expectedType);
    TypeInfo checkMatch(const MatchExpr& statement);
    bool checkPattern(
        const Pattern& pattern,
        const TypeInfo& expectedType,
        std::unordered_set<std::string>& coveredVariants,
        std::unordered_set<std::string>& coveredLiterals,
        bool& coversNil,
        bool& coversStruct,
        PatternBindings* deferredBindings = nullptr);
    const StructFieldType* findStructField(const StructTypeDecl& structType, const std::string& name) const;
    bool canAccessPrivateFields(const StructTypeDecl& structType) const;
    TypeInfo structFieldTypeForValue(
        const TypeInfo& objectType,
        const StructTypeDecl& structType,
        const StructFieldType& field) const;
    CheckedExpression checkLetInitializer(const LetStmt& statement);
    TypeInfo resolveAnnotation(const TypeAnnotation& typeName) const;
    TypeInfo resolveTypeParameterConstraint(const TypeAnnotation& typeName) const;
    TypeInfo resolveTypeParameterConstraints(const TypeParameter& parameter) const;
    void checkAssignable(const Token& token, const std::string& context, const TypeInfo& expected, const TypeInfo& actual) const;
    TypeInfo checkUnary(const UnaryExpr& expression);
    TypeInfo checkBinary(const BinaryExpr& expression);
    bool isGlobalBinding(const Binding& binding) const;
    bool isCurrentFunctionBinding(const Binding& binding) const;

    std::vector<Scope> scopes_;
    std::unordered_map<DeclarationId, Binding*, SnapshotIdHash<DeclarationIdTag>> bindingsById_;
    std::vector<std::unordered_map<std::string, TypeInfo>> typeParameterScopes_;
    std::unordered_map<std::string, StructTypeDecl> structTypes_;
    std::unordered_map<std::string, const StructDeclStmt*> structDeclarations_;
    std::unordered_map<std::string, StructCheckState> structCheckStates_;
    std::unordered_map<std::string, EnumTypeDecl> enumTypes_;
    std::unordered_map<std::string, const EnumDeclStmt*> enumDeclarations_;
    std::unordered_map<std::string, EnumCheckState> enumCheckStates_;
    MethodTable methods_;
    ModuleSymbols moduleSymbols_;
    std::vector<ModuleInterface> moduleInterfaces_;
    std::vector<ModuleInterface> preloadedModuleInterfaces_;
    std::unordered_set<std::size_t> preloadedModuleIds_;
    std::unordered_set<std::size_t> checkedModules_;
    std::vector<std::size_t> checkedModuleBodyIds_;
    std::vector<std::size_t> moduleStack_;
    DeclarationIndex declarationIndex_;
    std::size_t declarationIndexMismatchCount_ = 0;
    std::size_t moduleInterfaceMismatchCount_ = 0;
    const Program* currentProgram_ = nullptr;
    std::size_t nextResolvedName_ = 0;
    std::size_t nextBindingId_ = 0;
    std::size_t functionDepth_ = 0;
    std::size_t loopDepth_ = 0;
    std::vector<FunctionReturnContext> returnContexts_;
};
