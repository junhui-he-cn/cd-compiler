#include "TypeChecker.hpp"

#include "NativeStdlib.hpp"
#include "TypeCheckerInternal.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

TypeInfo logicalResultType(const TypeInfo& left, const TypeInfo& right)
{
    if (!SemanticTypes::isKnown(left) || !SemanticTypes::isKnown(right)) {
        return unknownType();
    }
    if (left.kind == right.kind) {
        return left;
    }
    return unknownType();
}

TypeInfo copiedArrayType(const TypeInfo& source)
{
    if (source.kind == StaticType::Array && source.elementType) {
        return arrayType(*source.elementType);
    }
    return simpleType(StaticType::Array);
}

TypeInfo concatenatedArrayType(const TypeInfo& left, const TypeInfo& right)
{
    if (left.kind != StaticType::Array || right.kind != StaticType::Array
        || !left.elementType || !right.elementType) {
        return simpleType(StaticType::Array);
    }
    std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*left.elementType, *right.elementType);
    if (!merged) {
        return simpleType(StaticType::Array);
    }
    return arrayType(std::move(*merged));
}

TypeInfo mergedMapType(const TypeInfo& left, const TypeInfo& right)
{
    if (left.kind != StaticType::Map || right.kind != StaticType::Map
        || !left.keyType || !right.keyType || !left.valueType || !right.valueType) {
        return simpleType(StaticType::Map);
    }
    std::optional<TypeInfo> key = SemanticTypes::mergeArrayElementTypes(*left.keyType, *right.keyType);
    std::optional<TypeInfo> value = SemanticTypes::mergeArrayElementTypes(*left.valueType, *right.valueType);
    if (!key || !value) {
        return simpleType(StaticType::Map);
    }
    return mapType(std::move(*key), std::move(*value));
}

bool isPrimitiveMatchKind(StaticType kind)
{
    return kind == StaticType::Nil
        || kind == StaticType::Number
        || kind == StaticType::Bool
        || kind == StaticType::String;
}

const TypeInfo* primitiveMatchBaseType(const TypeInfo& type)
{
    const TypeInfo* base = SemanticTypes::isNullable(type) ? type.nullableOf.get() : &type;
    if (!base || !isPrimitiveMatchKind(base->kind)) {
        return nullptr;
    }
    return base;
}

TypeInfo literalPatternType(const Token& token)
{
    switch (token.type) {
    case TokenType::Nil:
        return simpleType(StaticType::Nil);
    case TokenType::True:
    case TokenType::False:
        return simpleType(StaticType::Bool);
    case TokenType::Number:
        return simpleType(StaticType::Number);
    case TokenType::String:
        return simpleType(StaticType::String);
    default:
        throw std::logic_error("unsupported literal pattern token");
    }
}

std::string recordPatternTypeName(const RecordPattern& pattern)
{
    if (pattern.qualifier) {
        return pattern.qualifier->lexeme + "." + pattern.name.lexeme;
    }
    return pattern.name.lexeme;
}

std::string unqualifiedStructName(const std::string& name)
{
    const std::size_t separator = name.rfind('.');
    return separator == std::string::npos ? name : name.substr(separator + 1);
}

std::string binaryTypesMessage(const BinaryExpr& expression, const TypeInfo& left, const TypeInfo& right)
{
    return "binary `" + expression.op.lexeme + "` expects numbers, got "
        + typeInfoName(left) + " and " + typeInfoName(right);
}

std::string orderingTypesMessage(const BinaryExpr& expression, const TypeInfo& left, const TypeInfo& right)
{
    return "binary `" + expression.op.lexeme + "` expects two numbers or two strings, got "
        + typeInfoName(left) + " and " + typeInfoName(right);
}

} // namespace

const TypeChecker::StructFieldType* TypeChecker::findStructField(
    const StructTypeDecl& structType,
    const std::string& name) const
{
    for (const StructFieldType& field : structType.fields) {
        if (field.name.lexeme == name) {
            return &field;
        }
    }
    return nullptr;
}

bool TypeChecker::canAccessPrivateFields(const StructTypeDecl& structType) const
{
    if (!structType.hasPrivateFields) {
        return true;
    }

    // A direct input (including the established ordered multi-file entry
    // path) is one compilation unit, so all of its declarations share the
    // same module visibility boundary.
    if (moduleStack_.empty()) {
        return true;
    }

    return structType.definingModuleId
        && *structType.definingModuleId == moduleStack_.back();
}

TypeInfo TypeChecker::structFieldTypeForValue(
    const TypeInfo& objectType,
    const StructTypeDecl& structType,
    const StructFieldType& field) const
{
    TypeSubstitutions substitutions;
    for (std::size_t i = 0; i < structType.genericParameters.size(); ++i) {
        if (i < objectType.typeArguments.size()) {
            substitutions.emplace(
                structType.genericParameters[i], objectType.typeArguments[i]);
        }
    }
    return SemanticTypes::substituteTypeParameters(field.type, substitutions);
}

TypeChecker::CheckedExpression TypeChecker::checkNamedStructFields(
    const Token& diagnosticToken,
    const TypeInfo& declared,
    const std::vector<StructField>& fields)
{
    const StructTypeDecl* structType = declared.structName ? findStructType(*declared.structName) : nullptr;
    if (!structType) {
        throw TypeError(diagnosticToken, "unknown struct type `" + typeInfoName(declared) + "`");
    }

    if (structType->hasPrivateFields && !canAccessPrivateFields(*structType)) {
        throw TypeError(diagnosticToken,
            "struct `" + unqualifiedStructName(structType->name.lexeme)
                + "` has private fields and cannot be constructed outside its defining module");
    }

    std::unordered_map<std::string, const StructField*> literalFields;
    for (const StructField& field : fields) {
        if (literalFields.find(field.name.lexeme) != literalFields.end()) {
            throw TypeError(field.name, "duplicate field `" + field.name.lexeme + "` in struct literal");
        }
        literalFields.emplace(field.name.lexeme, &field);
    }

    for (const StructFieldType& expectedField : structType->fields) {
        const auto found = literalFields.find(expectedField.name.lexeme);
        if (found == literalFields.end()) {
            throw TypeError(diagnosticToken,
                "missing field `" + expectedField.name.lexeme + "` for struct `" + structType->name.lexeme + "`");
        }
        const TypeInfo expectedFieldType = structFieldTypeForValue(
            declared, *structType, expectedField);
        const CheckedExpression actual = checkExpressionInfo(
            *found->second->value, &expectedFieldType);
        if (!SemanticTypes::compatible(expectedFieldType, actual.type)) {
            throw TypeError(found->second->name,
                "field `" + expectedField.name.lexeme + "` expects " + typeInfoName(expectedFieldType)
                    + ", got " + typeInfoName(actual.type));
        }
    }

    for (const StructField& field : fields) {
        if (!findStructField(*structType, field.name.lexeme)) {
            throw TypeError(field.name,
                "extra field `" + field.name.lexeme + "` for struct `" + structType->name.lexeme + "`");
        }
    }

    return CheckedExpression{declared};
}

TypeChecker::CheckedExpression TypeChecker::checkStructConstructor(
    const StructConstructExpr& expression,
    const TypeInfo* expectedType)
{
    if (expression.qualifier) {
        const NamespaceImport* namespaceImport = findNamespace(expression.qualifier->lexeme);
        if (!namespaceImport) {
            throw TypeError(*expression.qualifier, "unknown module namespace `" + expression.qualifier->lexeme + "`");
        }
        if (namespaceImport->structs.find(expression.name.lexeme) == namespaceImport->structs.end()) {
            throw TypeError(expression.name,
                "module namespace `" + expression.qualifier->lexeme + "` has no exported type `" + expression.name.lexeme + "`");
        }
    }

    const std::string typeName = structConstructorTypeName(expression);
    const StructTypeDecl* structType = findStructType(typeName);
    if (!structType) {
        throw TypeError(expression.name, "unknown struct type `" + typeName + "`");
    }

    const bool generic = !structType->genericParameters.empty();
    if (!generic && !expression.typeArguments.empty()) {
        throw TypeError(expression.name, "struct `" + typeName + "` is not generic");
    }

    const TypeInfo* expectedStructType = expectedType;
    if (expectedStructType && SemanticTypes::isNullable(*expectedStructType)) {
        expectedStructType = expectedStructType->nullableOf.get();
    }
    const bool expectedMatches = expectedStructType
        && expectedStructType->kind == StaticType::Struct
        && expectedStructType->structName
        && *expectedStructType->structName == typeName;

    TypeSubstitutions substitutions;
    if (generic && !expression.typeArguments.empty()) {
        if (expression.typeArguments.size() != structType->genericParameters.size()) {
            throw TypeError(expression.name,
                "struct `" + typeName + "` expects "
                    + std::to_string(structType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(expression.typeArguments.size()));
        }
        for (std::size_t i = 0; i < expression.typeArguments.size(); ++i) {
            substitutions.emplace(
                structType->genericParameters[i],
                resolveAnnotation(expression.typeArguments[i]));
        }
    }

    if (generic && expression.typeArguments.empty() && expectedMatches) {
        if (expectedStructType->typeArguments.size() != structType->genericParameters.size()) {
            throw TypeError(expression.name,
                "struct `" + typeName + "` expects "
                    + std::to_string(structType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(expectedStructType->typeArguments.size()));
        }
        for (std::size_t i = 0; i < structType->genericParameters.size(); ++i) {
            substitutions.emplace(
                structType->genericParameters[i], expectedStructType->typeArguments[i]);
        }
    }

    std::unordered_map<std::string, const StructField*> literalFields;
    for (const StructField& field : expression.fields) {
        if (!literalFields.emplace(field.name.lexeme, &field).second) {
            throw TypeError(field.name,
                "duplicate field `" + field.name.lexeme + "` in struct literal");
        }
    }

    for (const StructFieldType& field : structType->fields) {
        const auto found = literalFields.find(field.name.lexeme);
        if (found == literalFields.end()) {
            continue;
        }
        const TypeInfo expectedFieldType = SemanticTypes::substituteTypeParameters(field.type, substitutions);
        const CheckedExpression actual = checkExpressionInfo(
            *found->second->value, &expectedFieldType);
        if (!generic || !hasEscapingTypeParameter(expectedFieldType, {})) {
            if (!SemanticTypes::compatible(expectedFieldType, actual.type)) {
                throw TypeError(found->second->name,
                    "field `" + field.name.lexeme + "` expects "
                        + typeInfoName(expectedFieldType) + ", got "
                        + typeInfoName(actual.type));
            }
        }
        if (generic && expression.typeArguments.empty() && !expectedMatches) {
            inferTypeArguments(field.type, actual.type, substitutions, expression.name);
        }
    }

    std::vector<TypeInfo> typeArguments;
    if (generic) {
        for (const std::string& parameter : structType->genericParameters) {
            if (substitutions.find(parameter) == substitutions.end()) {
                throw TypeError(expression.name,
                    "cannot infer type parameter " + parameter + " for struct " + typeName);
            }
        }
        validateGenericTypeArguments(
            structType->genericParameters,
            structType->genericParameterConstraints,
            substitutions,
            expression.name,
            "struct " + typeName);
        typeArguments.reserve(structType->genericParameters.size());
        for (const std::string& parameter : structType->genericParameters) {
            typeArguments.push_back(substitutions.at(parameter));
        }
    }

    const TypeInfo declared = namedStructType(typeName, std::move(typeArguments));
    if (expectedMatches && !SemanticTypes::compatible(*expectedStructType, declared)) {
        throw TypeError(expression.name,
            "struct constructor produces " + typeInfoName(declared)
                + ", expected " + typeInfoName(*expectedStructType));
    }
    return checkNamedStructFields(expression.name, declared, expression.fields);
}

TypeChecker::CheckedExpression TypeChecker::checkVariantConstructor(
    const MemberCallExpr& expression,
    const TypeInfo* expectedType)
{
    const std::string enumName = enumConstructorTypeName(expression);
    const EnumTypeDecl* enumType = findEnumType(enumName);
    if (!enumType) {
        throw TypeError(expression.name, "unknown enum type " + enumName);
    }

    const EnumVariantType* variant = findEnumVariant(*enumType, expression.name.lexeme);
    if (!variant) {
        throw TypeError(expression.name,
            "enum " + enumName + " has no variant " + expression.name.lexeme);
    }
    if (variant->payloadTypes.size() != expression.arguments.size()) {
        throw TypeError(expression.paren,
            "variant " + enumName + "." + expression.name.lexeme + " expects "
                + std::to_string(variant->payloadTypes.size()) + " arguments but got "
                + std::to_string(expression.arguments.size()));
    }

    const bool generic = !enumType->genericParameters.empty();
    if (!generic && !expression.typeArguments.empty()) {
        throw TypeError(expression.paren, "function is not generic");
    }

    TypeSubstitutions substitutions;
    if (generic && !expression.typeArguments.empty()) {
        if (expression.typeArguments.size() != enumType->genericParameters.size()) {
            throw TypeError(expression.paren,
                "enum `" + enumName + "` expects "
                    + std::to_string(enumType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(expression.typeArguments.size()));
        }
        for (std::size_t i = 0; i < enumType->genericParameters.size(); ++i) {
            substitutions.emplace(
                enumType->genericParameters[i],
                resolveAnnotation(expression.typeArguments[i]));
        }
    }

    const TypeInfo* expectedEnumType = expectedType;
    if (expectedEnumType && SemanticTypes::isNullable(*expectedEnumType)) {
        expectedEnumType = expectedEnumType->nullableOf.get();
    }
    const bool expectedMatches = generic
        && expectedEnumType
        && expectedEnumType->kind == StaticType::Enum
        && expectedEnumType->enumName
        && *expectedEnumType->enumName == enumName;
    if (expectedMatches && expression.typeArguments.empty()) {
        if (expectedEnumType->typeArguments.size() != enumType->genericParameters.size()) {
            throw TypeError(expression.paren,
                "enum `" + enumName + "` expects "
                    + std::to_string(enumType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(expectedEnumType->typeArguments.size()));
        }
        for (std::size_t i = 0; i < enumType->genericParameters.size(); ++i) {
            substitutions.emplace(
                enumType->genericParameters[i], expectedEnumType->typeArguments[i]);
        }
    }

    std::vector<CheckedExpression> arguments;
    arguments.reserve(expression.arguments.size());
    for (std::size_t i = 0; i < expression.arguments.size(); ++i) {
        const TypeInfo payloadType = SemanticTypes::substituteTypeParameters(
            variant->payloadTypes[i], substitutions);
        const CheckedExpression argument = checkExpressionInfo(
            *expression.arguments[i],
            (!generic || !substitutions.empty()) ? &payloadType : nullptr);
        arguments.push_back(argument);
        if (generic && expression.typeArguments.empty() && !expectedMatches) {
            inferTypeArguments(
                variant->payloadTypes[i], argument.type, substitutions, expression.paren);
        }
    }

    if (generic) {
        for (const std::string& parameter : enumType->genericParameters) {
            if (substitutions.find(parameter) == substitutions.end()) {
                throw TypeError(expression.paren,
                    "cannot infer type parameter " + parameter + " for enum " + enumName);
            }
        }
        validateGenericTypeArguments(
            enumType->genericParameters,
            enumType->genericParameterConstraints,
            substitutions,
            expression.paren,
            "enum " + enumName);
    }

    std::vector<TypeInfo> typeArguments;
    typeArguments.reserve(enumType->genericParameters.size());
    for (const std::string& parameter : enumType->genericParameters) {
        typeArguments.push_back(substitutions.at(parameter));
    }

    std::vector<TypeInfo> resolvedPayloadTypes;
    resolvedPayloadTypes.reserve(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const TypeInfo payloadType = SemanticTypes::substituteTypeParameters(
            variant->payloadTypes[i], substitutions);
        resolvedPayloadTypes.push_back(payloadType);
        if (!SemanticTypes::compatible(payloadType, arguments[i].type)) {
            throw TypeError(expression.paren,
                "variant argument " + std::to_string(i + 1) + " expects "
                    + typeInfoName(payloadType) + ", got "
                    + typeInfoName(arguments[i].type));
        }
    }

    std::string runtimeEnumName = enumName;
    const std::size_t namespaceSeparator = enumName.find('.');
    if (namespaceSeparator != std::string::npos) {
        const std::string alias = enumName.substr(0, namespaceSeparator);
        const std::string localName = enumName.substr(namespaceSeparator + 1);
        if (const NamespaceImport* namespaceImport = findNamespace(alias)) {
            const auto found = namespaceImport->enums.find(localName);
            if (found != namespaceImport->enums.end()) {
                runtimeEnumName = found->second.name.lexeme;
            }
        }
    }
    TypeInfo resultType = namedEnumType(enumName, std::move(typeArguments));
    declarationIndex_.recordVariantConstructor(
        expression,
        runtimeEnumName,
        expression.name.lexeme,
        resultType,
        std::move(resolvedPayloadTypes));
    return CheckedExpression{std::move(resultType)};
}

bool TypeChecker::checkPattern(
    const Pattern& pattern,
    const TypeInfo& expectedType,
    std::unordered_set<std::string>& coveredVariants,
    std::unordered_set<std::string>& coveredLiterals,
    bool& coversNil,
    bool& coversStruct,
    PatternBindings* deferredBindings)
{
    if (dynamic_cast<const WildcardPattern*>(&pattern)) {
        coversStruct = true;
        return true;
    }

    if (const auto* variable = dynamic_cast<const VariablePattern*>(&pattern)) {
        const TypeInfo bindingType = SemanticTypes::isNullable(expectedType) && expectedType.nullableOf
            ? *expectedType.nullableOf
            : expectedType;
        if (deferredBindings) {
            if (deferredBindings->find(variable->name.lexeme) != deferredBindings->end()) {
                throw TypeError(variable->name,
                    "duplicate pattern binding `" + variable->name.lexeme + "` in OR pattern");
            }
            deferredBindings->emplace(
                variable->name.lexeme,
                PatternBindingInfo{variable->name, bindingType, {variable}});
            coversStruct = true;
            return true;
        }
        const Binding binding = declareVariable(
            variable->name,
            bindingType,
            false,
            declarationIndex_.declaration(*variable));
        declarationIndex_.recordPatternBinding(
            *variable,
            PatternBindingRecord{
                variable->name.lexeme,
                binding.resolvedName,
                binding.type,
                binding.range,
                binding.bindingId,
                ResolvedSymbol{binding.declarationId, binding.symbolId}});
        coversStruct = true;
        return true;
    }

    if (const auto* recordPattern = dynamic_cast<const RecordPattern*>(&pattern)) {
        const TypeInfo* structExpectedType = SemanticTypes::isNullable(expectedType)
            ? expectedType.nullableOf.get()
            : &expectedType;
        if (!structExpectedType
            || structExpectedType->kind != StaticType::Struct
            || !structExpectedType->structName) {
            throw TypeError(recordPattern->name, "record pattern expects struct value");
        }

        const std::string patternTypeName = recordPatternTypeName(*recordPattern);
        if (patternTypeName != *structExpectedType->structName) {
            throw TypeError(recordPattern->name,
                "record pattern belongs to struct " + patternTypeName
                    + ", expected " + *structExpectedType->structName);
        }

        const StructTypeDecl* structType = findStructType(patternTypeName);
        if (!structType) {
            throw TypeError(recordPattern->name, "unknown struct type " + patternTypeName);
        }

        std::unordered_set<std::string> usedFields;
        bool universal = true;
        std::vector<std::string> resolvedFieldNames;
        std::vector<TypeInfo> resolvedFieldTypes;
        resolvedFieldNames.reserve(recordPattern->fields.size());
        resolvedFieldTypes.reserve(recordPattern->fields.size());
        for (const RecordPatternField& field : recordPattern->fields) {
            if (!usedFields.insert(field.name.lexeme).second) {
                throw TypeError(field.name,
                    "duplicate field `" + field.name.lexeme
                        + "` in record pattern for struct `" + patternTypeName + "`");
            }
            const StructFieldType* structField = findStructField(*structType, field.name.lexeme);
            if (!structField) {
                throw TypeError(field.name,
                    "struct `" + unqualifiedStructName(patternTypeName)
                        + (structType->hasPrivateFields ? "` has no accessible field `" : "` has no field `")
                        + field.name.lexeme + "` in record pattern");
            }
            if (structField->isPrivate && !canAccessPrivateFields(*structType)) {
                throw TypeError(field.name,
                    "struct `" + unqualifiedStructName(patternTypeName) + "` has no accessible field `"
                        + field.name.lexeme + "` in record pattern");
            }

            std::unordered_set<std::string> nestedCoverage;
            std::unordered_set<std::string> nestedLiterals;
            bool nestedCoversNil = false;
            bool nestedCoversStruct = false;
            const TypeInfo fieldType = structFieldTypeForValue(
                *structExpectedType, *structType, *structField);
            resolvedFieldNames.push_back(field.name.lexeme);
            resolvedFieldTypes.push_back(fieldType);
            const bool fieldUniversal = checkPattern(
                *field.pattern,
                fieldType,
                nestedCoverage,
                nestedLiterals,
                nestedCoversNil,
                nestedCoversStruct,
                deferredBindings);
            universal = universal && fieldUniversal;
        }

        declarationIndex_.recordRecordPattern(
            *recordPattern,
            RecordPatternRecord{
                *structExpectedType,
                std::move(resolvedFieldNames),
                std::move(resolvedFieldTypes)});
        coversStruct = universal;
        return SemanticTypes::isNullable(expectedType) ? false : universal;
    }

    if (const auto* orPattern = dynamic_cast<const OrPattern*>(&pattern)) {
        if (orPattern->alternatives.size() < 2) {
            throw TypeError(orPattern->pipe, "OR pattern requires at least two alternatives");
        }

        PatternBindings mergedBindings;
        std::unordered_set<std::string> bindingNames;
        bool firstAlternative = true;
        bool mergedCoversNil = false;
        bool mergedCoversStruct = false;
        bool mergedCoversAll = false;
        for (const PatternPtr& alternative : orPattern->alternatives) {
            std::unordered_set<std::string> alternativeVariants;
            std::unordered_set<std::string> alternativeLiterals;
            bool alternativeCoversNil = false;
            PatternBindings alternativeBindings;
            bool alternativeCoversStruct = false;
            const bool alternativeCoversAll = checkPattern(
                *alternative,
                expectedType,
                alternativeVariants,
                alternativeLiterals,
                alternativeCoversNil,
                alternativeCoversStruct,
                &alternativeBindings);

            coveredVariants.insert(alternativeVariants.begin(), alternativeVariants.end());
            coveredLiterals.insert(alternativeLiterals.begin(), alternativeLiterals.end());
            mergedCoversNil = mergedCoversNil || alternativeCoversNil;
            mergedCoversStruct = mergedCoversStruct || alternativeCoversStruct;
            mergedCoversAll = mergedCoversAll || alternativeCoversAll;

            if (firstAlternative) {
                firstAlternative = false;
                for (auto& entry : alternativeBindings) {
                    bindingNames.insert(entry.first);
                    mergedBindings.emplace(entry.first, std::move(entry.second));
                }
                continue;
            }

            if (alternativeBindings.size() != bindingNames.size()) {
                throw TypeError(orPattern->pipe,
                    "OR pattern alternatives must bind the same names");
            }
            for (const std::string& name : bindingNames) {
                const auto alternativeBinding = alternativeBindings.find(name);
                if (alternativeBinding == alternativeBindings.end()) {
                    throw TypeError(orPattern->pipe,
                        "OR pattern alternatives must bind the same names");
                }
                PatternBindingInfo& merged = mergedBindings.at(name);
                if (!SemanticTypes::compatible(merged.type, alternativeBinding->second.type)
                    || !SemanticTypes::compatible(alternativeBinding->second.type, merged.type)) {
                    throw TypeError(orPattern->pipe,
                        "OR pattern binding `" + name + "` has incompatible types: "
                            + typeInfoName(merged.type) + " and "
                            + typeInfoName(alternativeBinding->second.type));
                }
                merged.occurrences.insert(
                    merged.occurrences.end(),
                    alternativeBinding->second.occurrences.begin(),
                    alternativeBinding->second.occurrences.end());
            }
        }

        OrPatternRecord patternRecord;
        patternRecord.bindingNames.reserve(mergedBindings.size());
        patternRecord.bindingTypes.reserve(mergedBindings.size());
        for (const auto& entry : mergedBindings) {
            patternRecord.bindingNames.push_back(entry.first);
            patternRecord.bindingTypes.push_back(entry.second.type);
        }
        declarationIndex_.recordOrPattern(*orPattern, std::move(patternRecord));

        if (deferredBindings) {
            for (auto& entry : mergedBindings) {
                if (deferredBindings->find(entry.first) != deferredBindings->end()) {
                    throw TypeError(orPattern->pipe,
                        "duplicate pattern binding `" + entry.first + "` in OR pattern");
                }
                deferredBindings->emplace(entry.first, std::move(entry.second));
            }
        } else {
            for (auto& entry : mergedBindings) {
                const Binding binding = declareVariable(
                    entry.second.token,
                    entry.second.type,
                    false,
                    declarationIndex_.declaration(*entry.second.occurrences.front()));
                for (const VariablePattern* occurrence : entry.second.occurrences) {
                    declarationIndex_.recordPatternBinding(
                        *occurrence,
                        PatternBindingRecord{
                            occurrence->name.lexeme,
                            binding.resolvedName,
                            binding.type,
                            binding.range,
                            binding.bindingId,
                            ResolvedSymbol{binding.declarationId, binding.symbolId}});
                }
            }
        }

        coversNil = coversNil || mergedCoversNil;
        coversStruct = coversStruct || mergedCoversStruct;
        if (SemanticTypes::isNullable(expectedType)
            && expectedType.nullableOf
            && expectedType.nullableOf->kind == StaticType::Struct
            && mergedCoversNil && mergedCoversStruct) {
            mergedCoversAll = true;
        }
        return mergedCoversAll;
    }

    if (const auto* literal = dynamic_cast<const LiteralPattern*>(&pattern)) {
        const TypeInfo literalType = literalPatternType(literal->value);
        if (literal->value.type == TokenType::Nil) {
            if (expectedType.kind == StaticType::Nil) {
                declarationIndex_.recordLiteralPattern(
                    *literal,
                    LiteralPatternRecord{literal->value.lexeme, literalType});
                return true;
            }
            if (SemanticTypes::isNullable(expectedType)) {
                coversNil = true;
                declarationIndex_.recordLiteralPattern(
                    *literal,
                    LiteralPatternRecord{literal->value.lexeme, literalType});
                return false;
            }
            if (expectedType.kind == StaticType::Enum) {
                throw TypeError(literal->value, "literal patterns are not valid for enum values");
            }
        }

        const TypeInfo* valueType = SemanticTypes::isNullable(expectedType)
            ? expectedType.nullableOf.get()
            : &expectedType;
        if (!valueType || !SemanticTypes::compatible(*valueType, literalType)) {
            throw TypeError(literal->value,
                "literal pattern expects " + typeInfoName(expectedType)
                    + ", got " + typeInfoName(literalType));
        }
        declarationIndex_.recordLiteralPattern(
            *literal,
            LiteralPatternRecord{literal->value.lexeme, literalType});
        if (literal->value.type == TokenType::True
            || literal->value.type == TokenType::False) {
            coveredLiterals.insert(literal->value.lexeme);
        }
        return false;
    }

    const auto* variantPattern = dynamic_cast<const VariantPattern*>(&pattern);
    if (!variantPattern) {
        throw TypeError("unsupported pattern node");
    }
    const TypeInfo* enumExpectedType = &expectedType;
    if (SemanticTypes::isNullable(expectedType)) {
        enumExpectedType = expectedType.nullableOf.get();
    }
    if (!enumExpectedType
        || enumExpectedType->kind != StaticType::Enum
        || !enumExpectedType->enumName) {
        throw TypeError(variantPattern->name, "variant pattern expects enum value");
    }
    if (!variantPattern->qualifier
        || variantPattern->qualifier->lexeme != *enumExpectedType->enumName) {
        throw TypeError(variantPattern->name,
            "variant pattern belongs to enum "
                + (variantPattern->qualifier ? variantPattern->qualifier->lexeme : std::string("<unknown>"))
                + ", expected " + *enumExpectedType->enumName);
    }

    const EnumTypeDecl* enumType = findEnumType(*enumExpectedType->enumName);
    const EnumVariantType* variant = enumType
        ? findEnumVariant(*enumType, variantPattern->name.lexeme)
        : nullptr;
    if (!variant) {
        throw TypeError(variantPattern->name,
            "enum " + *enumExpectedType->enumName + " has no variant "
                + variantPattern->name.lexeme);
    }
    TypeSubstitutions substitutions;
    if (!enumType->genericParameters.empty()) {
        if (enumExpectedType->typeArguments.size() != enumType->genericParameters.size()) {
            throw TypeError(variantPattern->name,
                "enum `" + *enumExpectedType->enumName + "` expects "
                    + std::to_string(enumType->genericParameters.size())
                    + " type arguments but got "
                    + std::to_string(enumExpectedType->typeArguments.size()));
        }
        for (std::size_t i = 0; i < enumType->genericParameters.size(); ++i) {
            substitutions.emplace(
                enumType->genericParameters[i], enumExpectedType->typeArguments[i]);
        }
    }
    if (variant->payloadTypes.size() != variantPattern->arguments.size()) {
        throw TypeError(variantPattern->name,
            "variant pattern " + *enumExpectedType->enumName + "." + variantPattern->name.lexeme
                + " expects " + std::to_string(variant->payloadTypes.size())
                + " patterns but got " + std::to_string(variantPattern->arguments.size()));
    }

    coveredVariants.insert(variantPattern->name.lexeme);

    std::string runtimeEnumName = *enumExpectedType->enumName;
    const std::size_t namespaceSeparator = runtimeEnumName.find('.');
    if (namespaceSeparator != std::string::npos) {
        const std::string alias = runtimeEnumName.substr(0, namespaceSeparator);
        const std::string localName = runtimeEnumName.substr(namespaceSeparator + 1);
        if (const NamespaceImport* namespaceImport = findNamespace(alias)) {
            const auto found = namespaceImport->enums.find(localName);
            if (found != namespaceImport->enums.end()) {
                runtimeEnumName = found->second.name.lexeme;
            }
        }
    }
    std::vector<TypeInfo> resolvedPayloadTypes;
    resolvedPayloadTypes.reserve(variantPattern->arguments.size());
    for (std::size_t i = 0; i < variantPattern->arguments.size(); ++i) {
        std::unordered_set<std::string> nestedCoverage;
        std::unordered_set<std::string> nestedLiterals;
        bool nestedCoversNil = false;
        bool nestedCoversStruct = false;
        const TypeInfo payloadType = SemanticTypes::substituteTypeParameters(
            variant->payloadTypes[i], substitutions);
        resolvedPayloadTypes.push_back(payloadType);
        checkPattern(
            *variantPattern->arguments[i],
            payloadType,
            nestedCoverage,
            nestedLiterals,
            nestedCoversNil,
            nestedCoversStruct,
            deferredBindings);
    }
    declarationIndex_.recordVariantPattern(
        *variantPattern,
        VariantPatternRecord{
            std::move(runtimeEnumName),
            variantPattern->name.lexeme,
            *enumExpectedType,
            std::move(resolvedPayloadTypes)});
    return false;
}

TypeInfo TypeChecker::checkMatch(const MatchExpr& statement)
{
    const TypeInfo scrutineeType = checkExpression(*statement.value);
    const bool nullableEnum = SemanticTypes::isNullable(scrutineeType)
        && scrutineeType.nullableOf->kind == StaticType::Enum
        && scrutineeType.nullableOf->enumName;
    const bool nullableStruct = SemanticTypes::isNullable(scrutineeType)
        && scrutineeType.nullableOf->kind == StaticType::Struct
        && scrutineeType.nullableOf->structName;
    const bool structValue = scrutineeType.kind == StaticType::Struct
        && scrutineeType.structName;
    const TypeInfo* primitiveType = primitiveMatchBaseType(scrutineeType);
    if ((scrutineeType.kind != StaticType::Enum || !scrutineeType.enumName)
        && !nullableEnum && !structValue && !nullableStruct && !primitiveType) {
        throw TypeError(statement.value && statement.value->span
                ? Token{TokenType::Match, "match", statement.value->span->line, statement.value->span->column}
                : Token{TokenType::Match, "match", 0, 0},
            "match expects enum, struct, bool, number, string, or nil value, got "
                + typeInfoName(scrutineeType));
    }

    std::string enumName;
    const EnumTypeDecl* enumType = nullptr;
    if (scrutineeType.kind == StaticType::Enum || nullableEnum) {
        enumName = nullableEnum
            ? *scrutineeType.nullableOf->enumName
            : *scrutineeType.enumName;
        enumType = findEnumType(enumName);
        if (!enumType) {
            throw TypeError("unknown enum type " + enumName);
        }
    }
    std::string structName;
    const StructTypeDecl* structType = nullptr;
    if (structValue || nullableStruct) {
        structName = nullableStruct
            ? *scrutineeType.nullableOf->structName
            : *scrutineeType.structName;
        structType = findStructType(structName);
        if (!structType) {
            throw TypeError("unknown struct type " + structName);
        }
    }

    std::unordered_set<std::string> coveredVariants;
    std::unordered_set<std::string> coveredLiterals;
    bool coveredNil = false;
    bool coveredStruct = false;
    bool coversAll = false;
    bool hasExpressionArm = false;
    bool hasBlockArm = false;
    std::optional<TypeInfo> resultType;
    for (const MatchArm& arm : statement.arms) {
        beginScope();
        std::unordered_set<std::string> armCoveredVariants;
        std::unordered_set<std::string> armCoveredLiterals;
        bool armCoversNil = false;
        bool armCoversStruct = false;
        const bool armCoversAll = checkPattern(
            *arm.pattern,
            scrutineeType,
            armCoveredVariants,
            armCoveredLiterals,
            armCoversNil,
            armCoversStruct);
        if (!arm.guard) {
            coveredVariants.insert(armCoveredVariants.begin(), armCoveredVariants.end());
            coveredLiterals.insert(armCoveredLiterals.begin(), armCoveredLiterals.end());
            coveredNil = coveredNil || armCoversNil;
            coveredStruct = coveredStruct || armCoversStruct;
            coversAll = coversAll || armCoversAll;
        }
        if (arm.guard) {
            const TypeInfo guardType = checkExpression(*arm.guard);
            declarationIndex_.recordPatternGuard(
                *arm.guard,
                PatternGuardRecord{guardType});
        }
        if (arm.expression) {
            hasExpressionArm = true;
            const TypeInfo armType = checkExpression(*arm.expression);
            if (!resultType) {
                resultType = armType;
            } else {
                const std::optional<TypeInfo> merged
                    = SemanticTypes::mergeArrayElementTypes(*resultType, armType);
                if (!merged) {
                    throw TypeError(
                        Token{TokenType::Match, "match",
                            statement.value->span ? statement.value->span->line : 0,
                            statement.value->span ? statement.value->span->column : 0},
                        "match arms have incompatible result types: "
                            + typeInfoName(*resultType) + " and " + typeInfoName(armType));
                }
                resultType = *merged;
            }
        } else if (arm.body) {
            hasBlockArm = true;
            checkStatement(*arm.body);
        } else {
            throw TypeError(
                Token{TokenType::Match, "match",
                    statement.value->span ? statement.value->span->line : 0,
                    statement.value->span ? statement.value->span->column : 0},
                "match arm is missing a body");
        }
        endScope();
    }
    if (hasExpressionArm && hasBlockArm) {
        throw TypeError(
            Token{TokenType::Match, "match",
                statement.value->span ? statement.value->span->line : 0,
                statement.value->span ? statement.value->span->column : 0},
            "match arms mix expressions and blocks");
    }

    if (!coversAll) {
        if (SemanticTypes::isNullable(scrutineeType) && !coveredNil) {
            throw TypeError(
                Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                    statement.value->span ? statement.value->span->column : 0},
                "non-exhaustive match: missing nil");
        }
        if (enumType) {
            for (const EnumVariantType& variant : enumType->variants) {
                if (coveredVariants.find(variant.name.lexeme) == coveredVariants.end()) {
                    throw TypeError(
                        Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                            statement.value->span ? statement.value->span->column : 0},
                        "non-exhaustive match: missing " + enumName + "."
                            + variant.name.lexeme);
                }
            }
        } else if (primitiveType && primitiveType->kind == StaticType::Bool) {
            if (coveredLiterals.find("true") == coveredLiterals.end()) {
                throw TypeError(
                    Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                        statement.value->span ? statement.value->span->column : 0},
                    "non-exhaustive match: missing true");
            }
            if (coveredLiterals.find("false") == coveredLiterals.end()) {
                throw TypeError(
                    Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                        statement.value->span ? statement.value->span->column : 0},
                    "non-exhaustive match: missing false");
            }
        } else if (primitiveType && primitiveType->kind == StaticType::Nil && !coveredNil) {
            throw TypeError(
                Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                    statement.value->span ? statement.value->span->column : 0},
                "non-exhaustive match: missing nil");
        } else if (primitiveType
            && (primitiveType->kind == StaticType::Number
            || primitiveType->kind == StaticType::String)) {
            throw TypeError(
                Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                    statement.value->span ? statement.value->span->column : 0},
                "non-exhaustive match: missing wildcard or binding pattern");
        } else if (structType && !coveredStruct) {
            throw TypeError(
                Token{TokenType::Match, "match", statement.value->span ? statement.value->span->line : 0,
                    statement.value->span ? statement.value->span->column : 0},
                "non-exhaustive match: missing wildcard, binding, or complete record pattern");
        }
    }

    MatchCoverageRecord coverage;
    coverage.scrutineeType = scrutineeType;
    coverage.nullable = SemanticTypes::isNullable(scrutineeType);
    coverage.coversNil = coveredNil;
    coverage.coversStruct = coveredStruct;
    coverage.coversAll = coversAll;
    coverage.exhaustive = true;
    coverage.coveredVariants.assign(coveredVariants.begin(), coveredVariants.end());
    coverage.coveredLiterals.assign(coveredLiterals.begin(), coveredLiterals.end());
    std::sort(coverage.coveredVariants.begin(), coverage.coveredVariants.end());
    std::sort(coverage.coveredLiterals.begin(), coverage.coveredLiterals.end());
    declarationIndex_.recordMatchCoverage(statement, std::move(coverage));
    return resultType.value_or(simpleType(StaticType::Nil));
}

TypeInfo TypeChecker::checkExpression(const Expr& expression)
{
    return checkExpressionInfo(expression).type;
}

TypeChecker::CheckedExpression TypeChecker::checkExpressionInfo(const Expr& expression)
{
    return checkExpressionInfo(expression, nullptr);
}

TypeInfo TypeChecker::variableType(const Binding& binding) const
{
    return binding.type;
}

TypeInfo TypeChecker::inferArrayElementType(const ArrayExpr& expression)
{
    std::optional<TypeInfo> current;
    for (const auto& element : expression.elements) {
        TypeInfo elementType = checkExpression(*element);
        if (!SemanticTypes::isKnown(elementType)) {
            return unknownType();
        }
        if (!current) {
            current = std::move(elementType);
            continue;
        }
        std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*current, elementType);
        if (!merged) {
            return unknownType();
        }
        current = std::move(*merged);
    }
    return current ? *current : unknownType();
}

void TypeChecker::refineArrayBindingFromMutation(Binding& target, const TypeInfo& valueType)
{
    if (target.explicitType) {
        return;
    }

    if (!SemanticTypes::isKnown(valueType)) {
        target.type = simpleType(StaticType::Array);
        return;
    }

    if (!SemanticTypes::isKnown(target.type) || (target.type.kind == StaticType::Array && !target.type.elementType)) {
        target.type = arrayType(valueType);
        return;
    }

    if (target.type.kind != StaticType::Array) {
        return;
    }

    if (!target.type.elementType) {
        target.type = arrayType(valueType);
        return;
    }

    std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*target.type.elementType, valueType);
    if (!merged) {
        target.type = simpleType(StaticType::Array);
        return;
    }

    target.type = arrayType(std::move(*merged));
}

TypeChecker::CheckedExpression TypeChecker::checkArrayLiteral(const ArrayExpr& expression, const TypeInfo* expectedType)
{
    if (expectedType && expectedType->kind == StaticType::Array && expectedType->elementType) {
        for (const auto& element : expression.elements) {
            const CheckedExpression actual = checkExpressionInfo(*element, expectedType->elementType.get());
            if (!SemanticTypes::compatible(*expectedType->elementType, actual.type)) {
                throw TypeError(expression.bracket,
                    "array element expects " + typeInfoName(*expectedType->elementType)
                        + ", got " + typeInfoName(actual.type));
            }
        }
        return CheckedExpression{*expectedType};
    }

    const TypeInfo element = inferArrayElementType(expression);
    if (SemanticTypes::isKnown(element)) {
        return CheckedExpression{arrayType(element)};
    }
    return CheckedExpression{simpleType(StaticType::Array)};
}

TypeInfo TypeChecker::inferMapType(const MapExpr& expression)
{
    std::optional<TypeInfo> keyType;
    std::optional<TypeInfo> valueType;
    bool hasUnknownComponent = false;

    for (const MapEntry& entry : expression.entries) {
        const TypeInfo currentKey = checkExpression(*entry.key);
        if (SemanticTypes::isKnown(currentKey) && !mapKeyTypeAllowed(currentKey)) {
            throw TypeError(entry.colon, "map key must be nil, number, bool, or string");
        }
        const TypeInfo currentValue = checkExpression(*entry.value);

        if (!SemanticTypes::isKnown(currentKey) || !SemanticTypes::isKnown(currentValue)) {
            hasUnknownComponent = true;
            continue;
        }

        if (!keyType) {
            keyType = currentKey;
        } else {
            std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*keyType, currentKey);
            if (!merged) {
                hasUnknownComponent = true;
            } else {
                keyType = std::move(*merged);
            }
        }

        if (!valueType) {
            valueType = currentValue;
        } else {
            std::optional<TypeInfo> merged = SemanticTypes::mergeArrayElementTypes(*valueType, currentValue);
            if (!merged) {
                hasUnknownComponent = true;
            } else {
                valueType = std::move(*merged);
            }
        }
    }

    if (hasUnknownComponent || !keyType || !valueType) {
        return simpleType(StaticType::Map);
    }
    return mapType(std::move(*keyType), std::move(*valueType));
}

TypeChecker::CheckedExpression TypeChecker::checkMapLiteral(
    const MapExpr& expression,
    const TypeInfo* expectedType)
{
    if (expectedType && expectedType->kind == StaticType::Map
        && expectedType->keyType && expectedType->valueType) {
        if (!mapKeyTypeAllowed(*expectedType->keyType)) {
            throw TypeError(expression.brace, "map key must be nil, number, bool, or string");
        }
        for (const MapEntry& entry : expression.entries) {
            const CheckedExpression key = checkExpressionInfo(*entry.key, expectedType->keyType.get());
            if (SemanticTypes::isKnown(key.type) && !mapKeyTypeAllowed(key.type)) {
                throw TypeError(entry.colon, "map key must be nil, number, bool, or string");
            }
            if (!SemanticTypes::compatible(*expectedType->keyType, key.type)) {
                throw TypeError(entry.colon, "map key is incompatible with map key type");
            }

            const CheckedExpression value = checkExpressionInfo(*entry.value, expectedType->valueType.get());
            if (!SemanticTypes::compatible(*expectedType->valueType, value.type)) {
                throw TypeError(entry.colon, "map value is incompatible with map value type");
            }
        }
        return CheckedExpression{*expectedType};
    }

    return CheckedExpression{inferMapType(expression)};
}

TypeChecker::CheckedExpression TypeChecker::checkExpressionInfo(const Expr& expression, const TypeInfo* expectedType)
{
    if (const auto* literal = dynamic_cast<const LiteralExpr*>(&expression)) {
        CheckedExpression result;
        if (literal->value == "nil") {
            result = CheckedExpression{simpleType(StaticType::Nil)};
        } else if (literal->value == "true" || literal->value == "false") {
            result = CheckedExpression{simpleType(StaticType::Bool)};
        } else if (literal->value.size() >= 2 && literal->value.front() == '"'
            && literal->value.back() == '"') {
            result = CheckedExpression{simpleType(StaticType::String)};
        } else {
            result = CheckedExpression{simpleType(StaticType::Number)};
        }
        declarationIndex_.recordTypedExpression(*literal, result.type);
        return result;
    }

    if (const auto* function = dynamic_cast<const FunctionExpr*>(&expression)) {
        CheckedExpression result = checkFunctionExpression(*function, expectedType);
        declarationIndex_.recordTypedExpression(*function, result.type);
        return result;
    }

    if (const auto* match = dynamic_cast<const MatchExpr*>(&expression)) {
        CheckedExpression result{checkMatch(*match)};
        declarationIndex_.recordTypedExpression(*match, result.type);
        return result;
    }

    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
        const Binding* binding = resolveVariableReference(*variable);
        if (!binding) {
            if (findNamespace(variable->name.lexeme)) {
                throw TypeError(variable->name, "namespace alias `" + variable->name.lexeme + "` is not a value");
            }
            throw TypeError(variable->name, "undefined variable `" + variable->name.lexeme + "`");
        }
        declarationIndex_.recordVariableBinding(
            *variable,
            BindingMetadataRecord{
                binding->resolvedName,
                binding->bindingId,
                ResolvedSymbol{binding->declarationId, binding->symbolId},
                binding->range,
                binding->imported});
        CheckedExpression result{variableType(*binding)};
        declarationIndex_.recordTypedExpression(*variable, result.type);
        return result;
    }

    if (const auto* assign = dynamic_cast<const AssignExpr*>(&expression)) {
        Binding* target = resolveAssignmentTarget(*assign);
        if (!target) {
            if (findNamespace(assign->name.lexeme)) {
                throw TypeError(assign->name, "cannot assign to namespace alias `" + assign->name.lexeme + "`");
            }
            throw TypeError(assign->name, "undefined variable `" + assign->name.lexeme + "`");
        }

        const CheckedExpression value = checkExpressionInfo(*assign->value, &target->type);

        if (target->type.kind == StaticType::Function && value.type.kind == StaticType::Function) {
            if (target->explicitType
                && target->type.genericParameters.empty()
                && !value.type.genericParameters.empty()) {
                throw TypeError(assign->name,
                    "cannot assign generic function to monomorphic function type");
            }
            if (SemanticTypes::hasFunctionSignature(target->type) && SemanticTypes::hasFunctionSignature(value.type)
                && target->type.parameterTypes.size() != value.type.parameterTypes.size()) {
                throw TypeError(assign->name,
                    "cannot assign function with " + std::to_string(value.type.parameterTypes.size())
                        + " parameters to `" + assign->name.lexeme
                        + "` of type function with " + std::to_string(target->type.parameterTypes.size()) + " parameters");
            }

            if (target->explicitType && !SemanticTypes::compatible(target->type, value.type)) {
                throw TypeError(assign->name, "cannot assign " + typeInfoName(value.type) + " to `" + assign->name.lexeme
                    + "` of type " + typeInfoName(target->type));
            }

            if (!target->explicitType) {
                target->type = value.type;
            }
        } else if (!SemanticTypes::compatible(target->type, value.type)) {
            const std::string targetTypeName = target->type.kind == StaticType::Function && !target->explicitType
                ? staticTypeName(StaticType::Function)
                : typeInfoName(target->type);
            throw TypeError(assign->name, "cannot assign " + typeInfoName(value.type) + " to `" + assign->name.lexeme
                + "` of type " + targetTypeName);
        } else if (!SemanticTypes::isKnown(target->type)) {
            target->type = value.type;
        }

        declarationIndex_.recordAssignmentBinding(
            *assign,
            BindingMetadataRecord{
                target->resolvedName,
                target->bindingId,
                ResolvedSymbol{target->declarationId, target->symbolId},
                target->range,
                target->imported});
        CheckedExpression result{target->type};
        declarationIndex_.recordTypedExpression(*assign, result.type);
        return result;
    }

    if (const auto* compound = dynamic_cast<const CompoundAssignExpr*>(&expression)) {
        Binding* target = resolveCompoundAssignmentTarget(*compound);
        if (!target) {
            if (findNamespace(compound->name.lexeme)) {
                throw TypeError(compound->name, "cannot assign to namespace alias `" + compound->name.lexeme + "`");
            }
            throw TypeError(compound->name, "undefined variable `" + compound->name.lexeme + "`");
        }

        const CheckedExpression value = checkExpressionInfo(*compound->value);
        checkKnownNumber(compound->op, target->type, "`" + compound->op.lexeme + "` expects number variable, got ");
        checkKnownNumber(compound->op, value.type, "`" + compound->op.lexeme + "` expects number value, got ");

        if (!SemanticTypes::isKnown(target->type)) {
            target->type = simpleType(StaticType::Number);
        }
        declarationIndex_.recordCompoundAssignmentBinding(
            *compound,
            BindingMetadataRecord{
                target->resolvedName,
                target->bindingId,
                ResolvedSymbol{target->declarationId, target->symbolId},
                target->range,
                target->imported});
        CheckedExpression result{simpleType(StaticType::Number)};
        declarationIndex_.recordTypedExpression(*compound, result.type);
        return result;
    }

    if (const auto* grouping = dynamic_cast<const GroupingExpr*>(&expression)) {
        CheckedExpression result = checkExpressionInfo(*grouping->expression);
        declarationIndex_.recordTypedExpression(*grouping, result.type);
        return result;
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        CheckedExpression result{checkUnary(*unary)};
        declarationIndex_.recordTypedExpression(*unary, result.type);
        return result;
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        CheckedExpression result{checkBinary(*binary)};
        declarationIndex_.recordTypedExpression(*binary, result.type);
        return result;
    }

    if (const auto* logical = dynamic_cast<const LogicalExpr*>(&expression)) {
        const TypeInfo left = checkExpression(*logical->left);
        const TypeInfo right = checkExpression(*logical->right);
        return CheckedExpression{logicalResultType(left, right)};
    }

    if (const auto* coalesce = dynamic_cast<const CoalesceExpr*>(&expression)) {
        const TypeInfo left = checkExpression(*coalesce->left);
        if (SemanticTypes::isKnown(left) && !SemanticTypes::isNullable(left)) {
            throw TypeError(coalesce->op,
                "`??` expects an optional left operand, got " + typeInfoName(left));
        }
        const TypeInfo unwrapped = SemanticTypes::isNullable(left) && left.nullableOf
            ? *left.nullableOf
            : unknownType();
        const CheckedExpression right = checkExpressionInfo(*coalesce->right, &unwrapped);
        if (SemanticTypes::isKnown(unwrapped) && SemanticTypes::isKnown(right.type)
            && !SemanticTypes::compatible(unwrapped, right.type)) {
            throw TypeError(coalesce->op,
                "`??` right operand expects " + typeInfoName(unwrapped)
                    + ", got " + typeInfoName(right.type));
        }
        const TypeInfo resultType = SemanticTypes::isKnown(unwrapped)
            ? unwrapped
            : (SemanticTypes::isKnown(right.type) ? right.type : unknownType());
        declarationIndex_.recordTypedExpression(expression, resultType);
        return CheckedExpression{resultType};
    }

    if (const auto* unwrap = dynamic_cast<const UnwrapOrReturnExpr*>(&expression)) {
        const TypeInfo valueType = checkExpression(*unwrap->value);
        if (SemanticTypes::isKnown(valueType) && !SemanticTypes::isNullable(valueType)) {
            throw TypeError(unwrap->op,
                "`?` expects an optional value, got " + typeInfoName(valueType));
        }
        if (returnContexts_.empty()) {
            throw TypeError(unwrap->op, "`?` requires an enclosing function");
        }
        const FunctionReturnContext& context = returnContexts_.back();
        if (!context.expectedReturnType
            || !SemanticTypes::isNullable(*context.expectedReturnType)) {
            throw TypeError(unwrap->op,
                "`?` requires the enclosing function to return optional<T>");
        }
        const TypeInfo resultType = SemanticTypes::isNullable(valueType) && valueType.nullableOf
            ? *valueType.nullableOf
            : unknownType();
        declarationIndex_.recordTypedExpression(expression, resultType);
        return CheckedExpression{resultType};
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
        CheckedExpression result = checkCall(*call);
        if (isNativeStdlibCall(*call)) {
            const auto* variable = dynamic_cast<const VariableExpr*>(call->callee.get());
            if (variable) {
                declarationIndex_.recordNativeCall(*call, variable->name.lexeme);
            }
        }
        declarationIndex_.recordTypedExpression(*call, result.type);
        return result;
    }

    if (const auto* memberCall = dynamic_cast<const MemberCallExpr*>(&expression)) {
        CheckedExpression result = checkMemberCall(*memberCall, expectedType);
        if (isNativeStdlibName(memberCall->name.lexeme)
            && !declarationIndex_.memberCallMetadata(*memberCall)
            && !declarationIndex_.variantConstructor(*memberCall)) {
            declarationIndex_.recordNativeCall(*memberCall, memberCall->name.lexeme);
        }
        declarationIndex_.recordTypedExpression(*memberCall, result.type);
        return result;
    }

    if (const auto* array = dynamic_cast<const ArrayExpr*>(&expression)) {
        CheckedExpression result = checkArrayLiteral(*array, expectedType);
        declarationIndex_.recordTypedExpression(*array, result.type);
        return result;
    }

    if (const auto* map = dynamic_cast<const MapExpr*>(&expression)) {
        CheckedExpression result = checkMapLiteral(*map, expectedType);
        declarationIndex_.recordTypedExpression(*map, result.type);
        return result;
    }

    if (const auto* construct = dynamic_cast<const StructConstructExpr*>(&expression)) {
        CheckedExpression result = checkStructConstructor(*construct, expectedType);
        declarationIndex_.recordTypedExpression(*construct, result.type);
        std::vector<std::string> fieldNames;
        fieldNames.reserve(construct->fields.size());
        for (const StructField& field : construct->fields) {
            fieldNames.push_back(field.name.lexeme);
        }
        declarationIndex_.recordStructConstructor(
            *construct,
            StructConstructorRecord{result.type, std::move(fieldNames)});
        return result;
    }

    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expression)) {
        if (const auto* variable = dynamic_cast<const VariableExpr*>(field->object.get())) {
            if (const NamespaceImport* namespaceImport = findNamespace(variable->name.lexeme)) {
                const auto found = namespaceImport->values.find(field->name.lexeme);
                if (found == namespaceImport->values.end()) {
                    throw TypeError(field->name,
                        "module namespace `" + variable->name.lexeme + "` has no exported member `" + field->name.lexeme + "`");
                }
                CheckedExpression result{found->second.type};
                declarationIndex_.recordTypedExpression(*field, result.type);
                declarationIndex_.recordFieldOperation(
                    *field,
                    FieldOperationRecord{
                        FieldOperationKind::Read,
                        field->name.lexeme,
                        result.type,
                        result.type,
                        found->second.resolvedName});
                return result;
            }
        }
        const TypeInfo object = checkExpression(*field->object);
        if (object.kind != StaticType::Unknown && object.kind != StaticType::Struct) {
            throw TypeError(field->name, "can only access fields on structs");
        }
        if (object.kind == StaticType::Struct && object.structName) {
            const StructTypeDecl* structType = findStructType(*object.structName);
            const StructFieldType* structField = structType ? findStructField(*structType, field->name.lexeme) : nullptr;
            if (!structField) {
                throw TypeError(field->name,
                    "struct `" + unqualifiedStructName(*object.structName)
                        + (structType && structType->hasPrivateFields
                                ? "` has no accessible field `"
                                : "` has no field `")
                        + field->name.lexeme + "`");
            }
            if (structField->isPrivate && !canAccessPrivateFields(*structType)) {
                throw TypeError(field->name,
                    "struct `" + unqualifiedStructName(*object.structName) + "` has no accessible field `"
                        + field->name.lexeme + "`");
            }
            const TypeInfo declaredFieldType = structFieldTypeForValue(object, *structType, *structField);
            const TypeInfo resultType = declaredFieldType;
            CheckedExpression result{resultType};
            declarationIndex_.recordTypedExpression(*field, result.type);
            declarationIndex_.recordFieldOperation(
                *field,
                FieldOperationRecord{
                    FieldOperationKind::Read,
                    field->name.lexeme,
                    declaredFieldType,
                    result.type,
                    std::nullopt});
            return result;
        }
        CheckedExpression result{unknownType()};
        declarationIndex_.recordTypedExpression(*field, result.type);
        declarationIndex_.recordFieldOperation(
            *field,
            FieldOperationRecord{
                FieldOperationKind::Read,
                field->name.lexeme,
                unknownType(),
                result.type,
                std::nullopt});
        return result;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignExpr*>(&expression)) {
        CheckedExpression result = checkFieldAssignment(*fieldAssign);
        declarationIndex_.recordTypedExpression(*fieldAssign, result.type);
        return result;
    }

    if (const auto* fieldCompound = dynamic_cast<const FieldCompoundAssignExpr*>(&expression)) {
        CheckedExpression result = checkFieldCompoundAssignment(*fieldCompound);
        declarationIndex_.recordTypedExpression(*fieldCompound, result.type);
        return result;
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
        CheckedExpression result{checkIndex(*index)};
        declarationIndex_.recordTypedExpression(*index, result.type);
        return result;
    }

    if (const auto* indexAssign = dynamic_cast<const IndexAssignExpr*>(&expression)) {
        CheckedExpression result = checkIndexAssignment(*indexAssign);
        declarationIndex_.recordTypedExpression(*indexAssign, result.type);
        return result;
    }

    if (const auto* indexCompound = dynamic_cast<const IndexCompoundAssignExpr*>(&expression)) {
        CheckedExpression result = checkIndexCompoundAssignment(*indexCompound);
        declarationIndex_.recordTypedExpression(*indexCompound, result.type);
        return result;
    }

    throw TypeError("unsupported expression node");
}

TypeChecker::CheckedExpression TypeChecker::checkArrayMap(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& callbackExpression)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken, "map expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const TypeInfo expectedCallback = functionType({elementType}, unknownType());
    const CheckedExpression callback = checkExpressionInfo(callbackExpression, &expectedCallback);
    if (callback.type.kind != StaticType::Unknown && callback.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            "map expects function as second argument, got " + typeInfoName(callback.type));
    }
    if (callback.type.kind != StaticType::Function || !SemanticTypes::hasFunctionSignature(callback.type)) {
        return CheckedExpression{simpleType(StaticType::Array)};
    }
    if (callback.type.parameterTypes.size() != 1) {
        throw TypeError(callToken, "map expects callback with 1 argument");
    }
    const TypeInfo callbackType = specializeGenericCallback(
        callToken, callback.type, {elementType}, "map");
    if (elementType.kind != StaticType::Unknown
        && !SemanticTypes::compatible(callbackType.parameterTypes.front(), elementType)) {
        throw TypeError(callToken,
            "map callback expects " + typeInfoName(elementType)
                + ", got " + typeInfoName(callbackType.parameterTypes.front()));
    }
    if (callbackType.returnType && SemanticTypes::isKnown(*callbackType.returnType)) {
        return CheckedExpression{arrayType(*callbackType.returnType)};
    }
    return CheckedExpression{simpleType(StaticType::Array)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayFilter(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken, "filter expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const TypeInfo expectedPredicate = functionType({elementType}, simpleType(StaticType::Bool));
    const CheckedExpression predicate = checkExpressionInfo(predicateExpression, &expectedPredicate);
    if (predicate.type.kind != StaticType::Unknown && predicate.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            "filter expects function as second argument, got " + typeInfoName(predicate.type));
    }
    if (predicate.type.kind == StaticType::Function && SemanticTypes::hasFunctionSignature(predicate.type)) {
        if (predicate.type.parameterTypes.size() != 1) {
            throw TypeError(callToken, "filter expects callback with 1 argument");
        }
        const TypeInfo predicateType = specializeGenericCallback(
            callToken, predicate.type, {elementType}, "filter");
        if (elementType.kind != StaticType::Unknown
            && !SemanticTypes::compatible(predicateType.parameterTypes.front(), elementType)) {
            throw TypeError(callToken,
                "filter callback expects " + typeInfoName(elementType)
                    + ", got " + typeInfoName(predicateType.parameterTypes.front()));
        }
        if (predicateType.returnType
            && SemanticTypes::isKnown(*predicateType.returnType)
            && !SemanticTypes::compatible(simpleType(StaticType::Bool), *predicateType.returnType)) {
            throw TypeError(callToken,
                "filter expects callback to return bool, got " + typeInfoName(*predicateType.returnType));
        }
    }

    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        return CheckedExpression{arrayType(*arrayTypeInfo.elementType)};
    }
    return CheckedExpression{simpleType(StaticType::Array)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayFlatMap(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& callbackExpression)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken, "flatMap expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const TypeInfo expectedCallback = functionType({elementType}, simpleType(StaticType::Array));
    const CheckedExpression callback = checkExpressionInfo(callbackExpression, &expectedCallback);
    if (callback.type.kind != StaticType::Unknown && callback.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            "flatMap expects function as second argument, got " + typeInfoName(callback.type));
    }
    if (callback.type.kind != StaticType::Function || !SemanticTypes::hasFunctionSignature(callback.type)) {
        return CheckedExpression{simpleType(StaticType::Array)};
    }
    if (callback.type.parameterTypes.size() != 1) {
        throw TypeError(callToken, "flatMap expects callback with 1 argument");
    }
    const TypeInfo callbackType = specializeGenericCallback(
        callToken, callback.type, {elementType}, "flatMap");
    if (elementType.kind != StaticType::Unknown
        && !SemanticTypes::compatible(callbackType.parameterTypes.front(), elementType)) {
        throw TypeError(callToken,
            "flatMap callback expects " + typeInfoName(elementType)
                + ", got " + typeInfoName(callbackType.parameterTypes.front()));
    }
    if (callbackType.returnType && SemanticTypes::isKnown(*callbackType.returnType)) {
        if (callbackType.returnType->kind != StaticType::Array) {
            throw TypeError(callToken,
                "flatMap expects callback to return array, got "
                    + typeInfoName(*callbackType.returnType));
        }
        if (callbackType.returnType->elementType) {
            return CheckedExpression{arrayType(*callbackType.returnType->elementType)};
        }
    }
    return CheckedExpression{simpleType(StaticType::Array)};
}

void TypeChecker::checkArrayPredicate(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression,
    const std::string& functionName)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken,
            functionName + " expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const TypeInfo expectedPredicate = functionType({elementType}, simpleType(StaticType::Bool));
    const CheckedExpression predicate = checkExpressionInfo(predicateExpression, &expectedPredicate);
    if (predicate.type.kind != StaticType::Unknown && predicate.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            functionName + " expects function as second argument, got " + typeInfoName(predicate.type));
    }
    if (predicate.type.kind == StaticType::Function && SemanticTypes::hasFunctionSignature(predicate.type)) {
        if (predicate.type.parameterTypes.size() != 1) {
            throw TypeError(callToken, functionName + " expects callback with 1 argument");
        }
        const TypeInfo predicateType = specializeGenericCallback(
            callToken, predicate.type, {elementType}, functionName);
        if (elementType.kind != StaticType::Unknown
            && !SemanticTypes::compatible(predicateType.parameterTypes.front(), elementType)) {
            throw TypeError(callToken,
                functionName + " callback expects " + typeInfoName(elementType)
                    + ", got " + typeInfoName(predicateType.parameterTypes.front()));
        }
        if (predicateType.returnType
            && SemanticTypes::isKnown(*predicateType.returnType)
            && !SemanticTypes::compatible(simpleType(StaticType::Bool), *predicateType.returnType)) {
            throw TypeError(callToken,
                functionName + " expects callback to return bool, got "
                    + typeInfoName(*predicateType.returnType));
        }
    }

}

TypeChecker::CheckedExpression TypeChecker::checkArrayAnyAll(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression,
    const std::string& functionName)
{
    checkArrayPredicate(callToken, arrayTypeInfo, predicateExpression, functionName);
    return CheckedExpression{simpleType(StaticType::Bool)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayCount(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression)
{
    checkArrayPredicate(callToken, arrayTypeInfo, predicateExpression, "count");
    return CheckedExpression{simpleType(StaticType::Number)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayFind(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression)
{
    checkArrayPredicate(callToken, arrayTypeInfo, predicateExpression, "find");
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        if (SemanticTypes::isNullable(*arrayTypeInfo.elementType)) {
            return CheckedExpression{*arrayTypeInfo.elementType};
        }
        return CheckedExpression{nullableType(*arrayTypeInfo.elementType)};
    }
    return CheckedExpression{unknownType()};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayFindIndex(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& predicateExpression)
{
    checkArrayPredicate(callToken, arrayTypeInfo, predicateExpression, "findIndex");
    return CheckedExpression{simpleType(StaticType::Number)};
}

TypeChecker::CheckedExpression TypeChecker::checkArrayReduce(
    const Token& callToken,
    const TypeInfo& arrayTypeInfo,
    const Expr& initialExpression,
    const Expr& callbackExpression)
{
    if (arrayTypeInfo.kind != StaticType::Unknown && arrayTypeInfo.kind != StaticType::Array) {
        throw TypeError(callToken, "reduce expects array as first argument, got " + typeInfoName(arrayTypeInfo));
    }

    TypeInfo elementType = unknownType();
    if (arrayTypeInfo.kind == StaticType::Array && arrayTypeInfo.elementType) {
        elementType = *arrayTypeInfo.elementType;
    }
    const CheckedExpression initial = checkExpressionInfo(initialExpression);
    const TypeInfo expectedCallback = functionType(
        {initial.type, elementType}, initial.type);
    const CheckedExpression callback = checkExpressionInfo(callbackExpression, &expectedCallback);
    if (callback.type.kind != StaticType::Unknown && callback.type.kind != StaticType::Function) {
        throw TypeError(callToken,
            "reduce expects function as third argument, got " + typeInfoName(callback.type));
    }
    if (callback.type.kind == StaticType::Function && SemanticTypes::hasFunctionSignature(callback.type)) {
        if (callback.type.parameterTypes.size() != 2) {
            throw TypeError(callToken, "reduce expects callback with 2 arguments");
        }
        const TypeInfo callbackType = specializeGenericCallback(
            callToken, callback.type, {initial.type, elementType}, "reduce");
        if (initial.type.kind != StaticType::Unknown
            && !SemanticTypes::compatible(callbackType.parameterTypes.front(), initial.type)) {
            throw TypeError(callToken,
                "reduce callback accumulator expects " + typeInfoName(initial.type)
                    + ", got " + typeInfoName(callbackType.parameterTypes.front()));
        }
        if (elementType.kind != StaticType::Unknown
            && !SemanticTypes::compatible(callbackType.parameterTypes[1], elementType)) {
            throw TypeError(callToken,
                "reduce callback element expects " + typeInfoName(elementType)
                    + ", got " + typeInfoName(callbackType.parameterTypes[1]));
        }
        if (callbackType.returnType
            && SemanticTypes::isKnown(*callbackType.returnType)
            && !SemanticTypes::compatible(initial.type, *callbackType.returnType)) {
            throw TypeError(callToken,
                "reduce expects callback to return " + typeInfoName(initial.type)
                    + ", got " + typeInfoName(*callbackType.returnType));
        }
    }

    return CheckedExpression{initial.type};
}

TypeChecker::CheckedExpression TypeChecker::checkMapMerge(
    const Token& callToken,
    const TypeInfo& leftType,
    const TypeInfo& rightType)
{
    if (leftType.kind != StaticType::Unknown && leftType.kind != StaticType::Map) {
        throw TypeError(callToken, "merge expects map as first argument, got " + typeInfoName(leftType));
    }
    if (rightType.kind != StaticType::Unknown && rightType.kind != StaticType::Map) {
        throw TypeError(callToken, "merge expects map as second argument, got " + typeInfoName(rightType));
    }
    return CheckedExpression{mergedMapType(leftType, rightType)};
}

TypeChecker::CheckedExpression TypeChecker::checkNativeStdlibCall(const CallExpr& expression)
{
    if (!expression.typeArguments.empty()) {
        throw TypeError(expression.paren, "function is not generic");
    }
    const auto* variable = dynamic_cast<const VariableExpr*>(expression.callee.get());
    if (!variable) {
        throw TypeError("native stdlib call missing variable callee");
    }

    const NativeFunctionSignature* function = findNativeStdlibFunction(variable->name.lexeme);
    if (!function) {
        throw TypeError(variable->name, "unknown native stdlib function `" + variable->name.lexeme + "`");
    }
    const bool validArity = function->kind == NativeFunctionKind::Range
        ? expression.arguments.size() >= function->arity
            && expression.arguments.size() <= function->maxArity
        : expression.arguments.size() == function->arity;
    if (!validArity) {
        std::string expectedArity = std::to_string(function->arity);
        if (function->maxArity != 0) {
            expectedArity += " to " + std::to_string(function->maxArity);
        }
        throw TypeError(expression.paren,
            "expected " + expectedArity + " arguments but got " + std::to_string(expression.arguments.size()));
    }

    switch (function->kind) {
    case NativeFunctionKind::Push: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        if (arrayArgument.type.kind != StaticType::Unknown && arrayArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "push expects array as first argument, got " + typeInfoName(arrayArgument.type));
        }

        Binding* target = findSimpleVariableBinding(*expression.arguments[0]);
        const bool strictElementCheck = target == nullptr || target->explicitType;
        const TypeInfo* expectedElement = strictElementCheck ? arrayArgument.type.elementType.get() : nullptr;
        const CheckedExpression valueArgument = checkExpressionInfo(*expression.arguments[1], expectedElement);
        if (strictElementCheck && expectedElement && !SemanticTypes::compatible(*expectedElement, valueArgument.type)) {
            throw TypeError(expression.paren,
                "push value expects " + typeInfoName(*expectedElement)
                    + ", got " + typeInfoName(valueArgument.type));
        }
        if (target && target->type.kind == StaticType::Array) {
            refineArrayBindingFromMutation(*target, valueArgument.type);
        }
        return CheckedExpression{simpleType(StaticType::Nil)};
    }
    case NativeFunctionKind::Pop: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        if (arrayArgument.type.kind != StaticType::Unknown && arrayArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "pop expects array as first argument, got " + typeInfoName(arrayArgument.type));
        }
        if (arrayArgument.type.kind == StaticType::Array && arrayArgument.type.elementType) {
            return CheckedExpression{*arrayArgument.type.elementType};
        }
        return CheckedExpression{unknownType()};
    }
    case NativeFunctionKind::Remove: {
        const CheckedExpression mapArgument = checkExpressionInfo(*expression.arguments[0]);
        if (mapArgument.type.kind != StaticType::Unknown && mapArgument.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                "remove expects map as first argument, got " + typeInfoName(mapArgument.type));
        }

        const TypeInfo* expectedKey = mapArgument.type.kind == StaticType::Map
            ? mapArgument.type.keyType.get()
            : nullptr;
        const CheckedExpression keyArgument = checkExpressionInfo(*expression.arguments[1], expectedKey);
        if (mapArgument.type.kind == StaticType::Map
            && SemanticTypes::isKnown(keyArgument.type)
            && !mapKeyTypeAllowed(keyArgument.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !SemanticTypes::compatible(*expectedKey, keyArgument.type)) {
            throw TypeError(expression.paren, "map key is incompatible with map key type");
        }
        if (mapArgument.type.kind == StaticType::Map && mapArgument.type.valueType) {
            return CheckedExpression{*mapArgument.type.valueType};
        }
        return CheckedExpression{unknownType()};
    }
    case NativeFunctionKind::Clear: {
        const CheckedExpression mapArgument = checkExpressionInfo(*expression.arguments[0]);
        if (mapArgument.type.kind != StaticType::Unknown && mapArgument.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                "clear expects map as first argument, got " + typeInfoName(mapArgument.type));
        }
        return CheckedExpression{simpleType(StaticType::Nil)};
    }
    case NativeFunctionKind::Merge: {
        const CheckedExpression leftArgument = checkExpressionInfo(*expression.arguments[0]);
        const CheckedExpression rightArgument = checkExpressionInfo(*expression.arguments[1]);
        return checkMapMerge(expression.paren, leftArgument.type, rightArgument.type);
    }
    case NativeFunctionKind::Keys:
    case NativeFunctionKind::Values: {
        const CheckedExpression mapArgument = checkExpressionInfo(*expression.arguments[0]);
        if (mapArgument.type.kind != StaticType::Unknown && mapArgument.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                std::string(function->name) + " expects map as first argument, got "
                    + typeInfoName(mapArgument.type));
        }
        const TypeInfo* elementType = mapArgument.type.kind == StaticType::Map
            ? (function->kind == NativeFunctionKind::Keys
                    ? mapArgument.type.keyType.get()
                    : mapArgument.type.valueType.get())
            : nullptr;
        return CheckedExpression{elementType ? arrayType(*elementType) : simpleType(StaticType::Array)};
    }
    case NativeFunctionKind::Floor:
    case NativeFunctionKind::Ceil:
    case NativeFunctionKind::Sqrt: {
        const CheckedExpression argument = checkExpressionInfo(*expression.arguments[0]);
        if (argument.type.kind != StaticType::Unknown && argument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                std::string(function->name) + " expects number, got " + typeInfoName(argument.type));
        }
        return CheckedExpression{simpleType(StaticType::Number)};
    }
    case NativeFunctionKind::Str:
        checkExpressionInfo(*expression.arguments[0]);
        return CheckedExpression{simpleType(StaticType::String)};
    case NativeFunctionKind::Substr: {
        const CheckedExpression stringArgument = checkExpressionInfo(*expression.arguments[0]);
        if (stringArgument.type.kind != StaticType::Unknown && stringArgument.type.kind != StaticType::String) {
            throw TypeError(expression.paren,
                "substr expects string as first argument, got " + typeInfoName(stringArgument.type));
        }
        const CheckedExpression startArgument = checkExpressionInfo(*expression.arguments[1]);
        if (startArgument.type.kind != StaticType::Unknown && startArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "substr expects number as second argument, got " + typeInfoName(startArgument.type));
        }
        const CheckedExpression lengthArgument = checkExpressionInfo(*expression.arguments[2]);
        if (lengthArgument.type.kind != StaticType::Unknown && lengthArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "substr expects number as third argument, got " + typeInfoName(lengthArgument.type));
        }
        return CheckedExpression{simpleType(StaticType::String)};
    }
    case NativeFunctionKind::CharAt: {
        const CheckedExpression stringArgument = checkExpressionInfo(*expression.arguments[0]);
        if (stringArgument.type.kind != StaticType::Unknown && stringArgument.type.kind != StaticType::String) {
            throw TypeError(expression.paren,
                "charAt expects string as first argument, got " + typeInfoName(stringArgument.type));
        }
        const CheckedExpression indexArgument = checkExpressionInfo(*expression.arguments[1]);
        if (indexArgument.type.kind != StaticType::Unknown && indexArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "charAt expects number as second argument, got " + typeInfoName(indexArgument.type));
        }
        return CheckedExpression{simpleType(StaticType::String)};
    }
    case NativeFunctionKind::TypeOf:
        checkExpressionInfo(*expression.arguments[0]);
        return CheckedExpression{simpleType(StaticType::String)};
    case NativeFunctionKind::Hash: {
        const CheckedExpression argument = checkExpressionInfo(*expression.arguments[0]);
        if (SemanticTypes::isKnown(argument.type)
            && !SemanticTypes::satisfiesCapability(argument.type, capabilityType("Hash"))) {
            if (argument.type.kind == StaticType::TypeParameter
                && argument.type.typeParameterName) {
                throw TypeError(expression.paren,
                    "hash requires type parameter `"
                        + *argument.type.typeParameterName
                        + "` to satisfy Hash");
            }
            throw TypeError(expression.paren,
                "hash expects a value satisfying Hash, got " + typeInfoName(argument.type));
        }
        return CheckedExpression{simpleType(StaticType::Number)};
    }
    case NativeFunctionKind::Contains: {
        const CheckedExpression collectionArgument = checkExpressionInfo(*expression.arguments[0]);
        if (collectionArgument.type.kind != StaticType::Unknown
            && collectionArgument.type.kind != StaticType::Array
            && collectionArgument.type.kind != StaticType::Map
            && collectionArgument.type.kind != StaticType::Range) {
            throw TypeError(expression.paren,
                "contains expects array, map, or range as first argument, got " + typeInfoName(collectionArgument.type));
        }
        const TypeInfo* expectedKey = nullptr;
        if (collectionArgument.type.kind == StaticType::Array) {
            expectedKey = collectionArgument.type.elementType.get();
        } else if (collectionArgument.type.kind == StaticType::Map) {
            expectedKey = collectionArgument.type.keyType.get();
        } else if (collectionArgument.type.kind == StaticType::Range) {
            expectedKey = nullptr;
        }
        const CheckedExpression keyArgument = checkExpressionInfo(*expression.arguments[1], expectedKey);
        if (collectionArgument.type.kind == StaticType::Map
            && SemanticTypes::isKnown(keyArgument.type)
            && !mapKeyTypeAllowed(keyArgument.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !SemanticTypes::compatible(*expectedKey, keyArgument.type)) {
            if (collectionArgument.type.kind == StaticType::Map) {
                throw TypeError(expression.paren, "map key is incompatible with map key type");
            }
            throw TypeError(expression.paren,
                "contains value expects " + typeInfoName(*expectedKey)
                    + ", got " + typeInfoName(keyArgument.type));
        }
        if (collectionArgument.type.kind == StaticType::Range
            && keyArgument.type.kind != StaticType::Unknown
            && keyArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "contains expects number as range value, got " + typeInfoName(keyArgument.type));
        }
        return CheckedExpression{simpleType(StaticType::Bool)};
    }
    case NativeFunctionKind::Slice: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        if (arrayArgument.type.kind != StaticType::Unknown && arrayArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "slice expects array as first argument, got " + typeInfoName(arrayArgument.type));
        }
        const CheckedExpression startArgument = checkExpressionInfo(*expression.arguments[1]);
        if (startArgument.type.kind != StaticType::Unknown && startArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "slice expects number as second argument, got " + typeInfoName(startArgument.type));
        }
        const CheckedExpression lengthArgument = checkExpressionInfo(*expression.arguments[2]);
        if (lengthArgument.type.kind != StaticType::Unknown && lengthArgument.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "slice expects number as third argument, got " + typeInfoName(lengthArgument.type));
        }
        return CheckedExpression{copiedArrayType(arrayArgument.type)};
    }
    case NativeFunctionKind::Copy: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        if (arrayArgument.type.kind != StaticType::Unknown && arrayArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "copy expects array as first argument, got " + typeInfoName(arrayArgument.type));
        }
        return CheckedExpression{copiedArrayType(arrayArgument.type)};
    }
    case NativeFunctionKind::Concat: {
        const CheckedExpression leftArgument = checkExpressionInfo(*expression.arguments[0]);
        if (leftArgument.type.kind != StaticType::Unknown && leftArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "concat expects array as first argument, got " + typeInfoName(leftArgument.type));
        }
        const CheckedExpression rightArgument = checkExpressionInfo(*expression.arguments[1]);
        if (rightArgument.type.kind != StaticType::Unknown && rightArgument.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "concat expects array as second argument, got " + typeInfoName(rightArgument.type));
        }
        return CheckedExpression{concatenatedArrayType(leftArgument.type, rightArgument.type)};
    }
    case NativeFunctionKind::Map: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayMap(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::Filter: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayFilter(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::FlatMap: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayFlatMap(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::Any:
    case NativeFunctionKind::All: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayAnyAll(
            expression.paren,
            arrayArgument.type,
            *expression.arguments[1],
            function->name);
    }
    case NativeFunctionKind::Count: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayCount(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::Find: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayFind(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::FindIndex: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayFindIndex(expression.paren, arrayArgument.type, *expression.arguments[1]);
    }
    case NativeFunctionKind::Reduce: {
        const CheckedExpression arrayArgument = checkExpressionInfo(*expression.arguments[0]);
        return checkArrayReduce(
            expression.paren,
            arrayArgument.type,
            *expression.arguments[1],
            *expression.arguments[2]);
    }
    case NativeFunctionKind::Range: {
        for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
            const CheckedExpression argument = checkExpressionInfo(*expression.arguments[index]);
            if (argument.type.kind != StaticType::Unknown && argument.type.kind != StaticType::Number) {
                const char* ordinal = index == 0 ? "first" : (index == 1 ? "second" : "third");
                throw TypeError(expression.paren,
                    std::string("range expects number as ") + ordinal + " argument, got "
                        + typeInfoName(argument.type));
            }
        }
        return CheckedExpression{simpleType(StaticType::Range)};
    }
    }

    throw TypeError(variable->name, "unknown native stdlib function `" + variable->name.lexeme + "`");
}

TypeChecker::CheckedExpression TypeChecker::checkStructMethodCall(const MemberCallExpr& expression, const TypeInfo& receiverType)
{
    const std::string& name = expression.name.lexeme;
    const MethodInfo* method = findMethod(*receiverType.structName, name);
    if (!method) {
        throw TypeError(expression.paren, "struct `" + *receiverType.structName + "` has no method `" + name + "`");
    }

    TypeSubstitutions receiverSubstitutions;
    if (!method->receiverType.typeArguments.empty()) {
        if (method->receiverType.typeArguments.size() != receiverType.typeArguments.size()) {
            throw TypeError(expression.paren,
                "method receiver expects "
                    + std::to_string(method->receiverType.typeArguments.size())
                    + " type arguments but got "
                    + std::to_string(receiverType.typeArguments.size()));
        }
        for (std::size_t i = 0; i < method->receiverType.typeArguments.size(); ++i) {
            inferTypeArguments(
                method->receiverType.typeArguments[i],
                receiverType.typeArguments[i],
                receiverSubstitutions,
                expression.paren);
        }
        const StructTypeDecl* structType = findStructType(*receiverType.structName);
        if (structType) {
            for (const std::string& parameter : structType->genericParameters) {
                if (receiverSubstitutions.find(parameter) == receiverSubstitutions.end()) {
                    throw TypeError(expression.paren,
                        "cannot specialize method receiver type parameter " + parameter);
                }
            }
            validateGenericTypeArguments(
                structType->genericParameters,
                structType->genericParameterConstraints,
                receiverSubstitutions,
                expression.paren,
                "method receiver");
        }
    }

    std::vector<TypeInfo> parameterTypes;
    parameterTypes.reserve(method->parameterTypes.size());
    for (const TypeInfo& parameter : method->parameterTypes) {
        parameterTypes.push_back(
            SemanticTypes::substituteTypeParameters(parameter, receiverSubstitutions));
    }
    const TypeInfo returnType = SemanticTypes::substituteTypeParameters(
        method->returnType, receiverSubstitutions);
    const TypeInfo signature = functionType(
        std::move(parameterTypes),
        returnType,
        method->genericParameters,
        method->genericParameterConstraints);
    const CheckedExpression result = checkFunctionCall(
        expression.paren,
        signature,
        expression.typeArguments,
        expression.arguments);
    declarationIndex_.recordMemberCallMetadata(
        expression,
        MemberCallMetadataRecord{
            method->resolvedName,
            true,
            method->declaration != nullptr});
    if (method->declaration) {
        if (const DeclarationRecord* target = declarationIndex_.declaration(*method->declaration)) {
            declarationIndex_.recordMemberCallTarget(
                expression,
                CallTargetRecord{
                    CallTargetKind::StructMethod,
                    ResolvedSymbol{target->declarationId, target->symbolId}});
        }
    }
    return result;
}

TypeChecker::CheckedExpression TypeChecker::checkMemberCall(
    const MemberCallExpr& expression,
    const TypeInfo* expectedType)
{
    const std::string& name = expression.name.lexeme;
    const std::size_t arity = expression.arguments.size();

    if (!enumConstructorTypeName(expression).empty()) {
        return checkVariantConstructor(expression, expectedType);
    }

    if (const auto* variable = dynamic_cast<const VariableExpr*>(expression.receiver.get())) {
        if (const NamespaceImport* namespaceImport = findNamespace(variable->name.lexeme)) {
            const auto found = namespaceImport->values.find(name);
            if (found == namespaceImport->values.end()) {
                throw TypeError(expression.name,
                    "module namespace `" + variable->name.lexeme + "` has no exported member `" + name + "`");
            }
            declarationIndex_.recordMemberCallMetadata(
                expression,
                MemberCallMetadataRecord{found->second.resolvedName, false});
            return checkFunctionCall(
                expression.paren,
                found->second.type,
                expression.typeArguments,
                expression.arguments);
        }
    }

    std::optional<CheckedExpression> builtinReceiver;
    if (isBuiltinMemberName(name)) {
        CheckedExpression receiver = checkExpressionInfo(*expression.receiver);
        if (receiver.type.kind == StaticType::Struct
            && receiver.type.structName
            && findMethod(*receiver.type.structName, name)) {
            return checkStructMethodCall(expression, receiver.type);
        }
        builtinReceiver = std::move(receiver);
    }

    if (!expression.typeArguments.empty() && isBuiltinMemberName(name)) {
        throw TypeError(expression.paren, "function is not generic");
    }

    auto expectArity = [&](std::size_t expected) {
        if (arity != expected) {
            throw TypeError(expression.paren,
                "expected " + std::to_string(expected) + " arguments but got " + std::to_string(arity));
        }
    };

    auto checkReceiver = [&]() {
        if (builtinReceiver) {
            return *builtinReceiver;
        }
        return checkExpressionInfo(*expression.receiver);
    };

    if (name == "push") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "push expects array receiver, got " + typeInfoName(receiver.type));
        }

        Binding* target = findSimpleVariableBinding(*expression.receiver);
        const bool strictElementCheck = target == nullptr || target->explicitType;
        const TypeInfo* expectedElement = strictElementCheck ? receiver.type.elementType.get() : nullptr;
        const CheckedExpression value = checkExpressionInfo(*expression.arguments[0], expectedElement);
        if (strictElementCheck && expectedElement && !SemanticTypes::compatible(*expectedElement, value.type)) {
            throw TypeError(expression.paren,
                "push value expects " + typeInfoName(*expectedElement) + ", got " + typeInfoName(value.type));
        }
        if (target && target->type.kind == StaticType::Array) {
            refineArrayBindingFromMutation(*target, value.type);
        }
        return CheckedExpression{simpleType(StaticType::Nil)};
    }

    if (name == "pop") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "pop expects array receiver, got " + typeInfoName(receiver.type));
        }
        if (receiver.type.kind == StaticType::Array && receiver.type.elementType) {
            return CheckedExpression{*receiver.type.elementType};
        }
        return CheckedExpression{unknownType()};
    }

    if (name == "contains") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown
            && receiver.type.kind != StaticType::Array
            && receiver.type.kind != StaticType::Map
            && receiver.type.kind != StaticType::Range) {
            throw TypeError(expression.paren, "contains expects array, map, or range receiver, got " + typeInfoName(receiver.type));
        }
        const TypeInfo* expectedKey = nullptr;
        if (receiver.type.kind == StaticType::Array) {
            expectedKey = receiver.type.elementType.get();
        } else if (receiver.type.kind == StaticType::Map) {
            expectedKey = receiver.type.keyType.get();
        }
        const CheckedExpression value = checkExpressionInfo(*expression.arguments[0], expectedKey);
        if (receiver.type.kind == StaticType::Map
            && SemanticTypes::isKnown(value.type)
            && !mapKeyTypeAllowed(value.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !SemanticTypes::compatible(*expectedKey, value.type)) {
            if (receiver.type.kind == StaticType::Map) {
                throw TypeError(expression.paren, "map key is incompatible with map key type");
            }
            throw TypeError(expression.paren,
                "contains value expects " + typeInfoName(*expectedKey) + ", got " + typeInfoName(value.type));
        }
        if (receiver.type.kind == StaticType::Range
            && value.type.kind != StaticType::Unknown
            && value.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "contains expects number as range value, got " + typeInfoName(value.type));
        }
        return CheckedExpression{simpleType(StaticType::Bool)};
    }

    if (name == "remove") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                "remove expects map receiver, got " + typeInfoName(receiver.type));
        }
        const TypeInfo* expectedKey = receiver.type.kind == StaticType::Map
            ? receiver.type.keyType.get()
            : nullptr;
        const CheckedExpression key = checkExpressionInfo(*expression.arguments[0], expectedKey);
        if (receiver.type.kind == StaticType::Map
            && SemanticTypes::isKnown(key.type)
            && !mapKeyTypeAllowed(key.type)) {
            throw TypeError(expression.paren, "map key must be nil, number, bool, or string");
        }
        if (expectedKey && !SemanticTypes::compatible(*expectedKey, key.type)) {
            throw TypeError(expression.paren, "map key is incompatible with map key type");
        }
        if (receiver.type.kind == StaticType::Map && receiver.type.valueType) {
            return CheckedExpression{*receiver.type.valueType};
        }
        return CheckedExpression{unknownType()};
    }

    if (name == "clear") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                "clear expects map receiver, got " + typeInfoName(receiver.type));
        }
        return CheckedExpression{simpleType(StaticType::Nil)};
    }

    if (name == "merge") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        const CheckedExpression right = checkExpressionInfo(*expression.arguments[0]);
        return checkMapMerge(expression.paren, receiver.type, right.type);
    }

    if (name == "keys" || name == "values") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Map) {
            throw TypeError(expression.paren,
                name + " expects map receiver, got " + typeInfoName(receiver.type));
        }
        const TypeInfo* elementType = receiver.type.kind == StaticType::Map
            ? (name == "keys" ? receiver.type.keyType.get() : receiver.type.valueType.get())
            : nullptr;
        return CheckedExpression{elementType ? arrayType(*elementType) : simpleType(StaticType::Array)};
    }

    if (name == "slice") {
        expectArity(2);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "slice expects array receiver, got " + typeInfoName(receiver.type));
        }
        const CheckedExpression start = checkExpressionInfo(*expression.arguments[0]);
        if (start.type.kind != StaticType::Unknown && start.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "slice expects number as first argument, got " + typeInfoName(start.type));
        }
        const CheckedExpression length = checkExpressionInfo(*expression.arguments[1]);
        if (length.type.kind != StaticType::Unknown && length.type.kind != StaticType::Number) {
            throw TypeError(expression.paren,
                "slice expects number as second argument, got " + typeInfoName(length.type));
        }
        return CheckedExpression{copiedArrayType(receiver.type)};
    }

    if (name == "copy") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "copy expects array receiver, got " + typeInfoName(receiver.type));
        }
        return CheckedExpression{copiedArrayType(receiver.type)};
    }

    if (name == "concat") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::Array) {
            throw TypeError(expression.paren, "concat expects array receiver, got " + typeInfoName(receiver.type));
        }
        const CheckedExpression right = checkExpressionInfo(*expression.arguments[0]);
        if (right.type.kind != StaticType::Unknown && right.type.kind != StaticType::Array) {
            throw TypeError(expression.paren,
                "concat expects array as first argument, got " + typeInfoName(right.type));
        }
        return CheckedExpression{concatenatedArrayType(receiver.type, right.type)};
    }

    if (name == "map") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayMap(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "filter") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayFilter(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "flatMap") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayFlatMap(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "any" || name == "all") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayAnyAll(expression.paren, receiver.type, *expression.arguments[0], name);
    }

    if (name == "count") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayCount(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "find") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayFind(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "findIndex") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayFindIndex(expression.paren, receiver.type, *expression.arguments[0]);
    }

    if (name == "reduce") {
        expectArity(2);
        const CheckedExpression receiver = checkReceiver();
        return checkArrayReduce(
            expression.paren,
            receiver.type,
            *expression.arguments[0],
            *expression.arguments[1]);
    }

    if (name == "len") {
        expectArity(0);
        const CheckedExpression receiver = checkReceiver();
        if (SemanticTypes::isKnown(receiver.type)
            && receiver.type.kind != StaticType::Array
            && receiver.type.kind != StaticType::String
            && receiver.type.kind != StaticType::Map
            && receiver.type.kind != StaticType::Range) {
            throw TypeError(expression.paren, "len expects array, string, map, or range receiver, got " + typeInfoName(receiver.type));
        }
        return CheckedExpression{simpleType(StaticType::Number)};
    }

    if (name == "substr") {
        expectArity(2);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::String) {
            throw TypeError(expression.paren, "substr expects string receiver, got " + typeInfoName(receiver.type));
        }
        const CheckedExpression start = checkExpressionInfo(*expression.arguments[0]);
        if (start.type.kind != StaticType::Unknown && start.type.kind != StaticType::Number) {
            throw TypeError(expression.paren, "substr expects number as first argument, got " + typeInfoName(start.type));
        }
        const CheckedExpression length = checkExpressionInfo(*expression.arguments[1]);
        if (length.type.kind != StaticType::Unknown && length.type.kind != StaticType::Number) {
            throw TypeError(expression.paren, "substr expects number as second argument, got " + typeInfoName(length.type));
        }
        return CheckedExpression{simpleType(StaticType::String)};
    }

    if (name == "charAt") {
        expectArity(1);
        const CheckedExpression receiver = checkReceiver();
        if (receiver.type.kind != StaticType::Unknown && receiver.type.kind != StaticType::String) {
            throw TypeError(expression.paren, "charAt expects string receiver, got " + typeInfoName(receiver.type));
        }
        const CheckedExpression index = checkExpressionInfo(*expression.arguments[0]);
        if (index.type.kind != StaticType::Unknown && index.type.kind != StaticType::Number) {
            throw TypeError(expression.paren, "charAt expects number as first argument, got " + typeInfoName(index.type));
        }
        return CheckedExpression{simpleType(StaticType::String)};
    }

    const CheckedExpression receiver = checkExpressionInfo(*expression.receiver);
    if (receiver.type.kind == StaticType::Struct && receiver.type.structName) {
        return checkStructMethodCall(expression, receiver.type);
    }

    if (receiver.type.kind == StaticType::Unknown
        || (receiver.type.kind != StaticType::Array && receiver.type.kind != StaticType::String)) {
        throw TypeError(expression.paren, "can only call methods on known named structs");
    }

    throw TypeError(expression.paren, "unknown member call `" + name + "`");
}

TypeChecker::IndexTargetTypes TypeChecker::checkIndexTarget(
    const Expr& collectionExpression,
    const Expr& indexExpression,
    const Token& bracket,
    const std::string& nonArrayMessage)
{
    IndexTargetTypes result {
        checkExpression(collectionExpression),
        checkExpression(indexExpression),
    };

    if (result.collection.kind != StaticType::Unknown
        && result.collection.kind != StaticType::Array
        && result.collection.kind != StaticType::Map
        && result.collection.kind != StaticType::Range) {
        throw TypeError(bracket, nonArrayMessage);
    }

    if (result.collection.kind == StaticType::Map) {
        if (SemanticTypes::isKnown(result.index) && !mapKeyTypeAllowed(result.index)) {
            throw TypeError(bracket, "map key must be nil, number, bool, or string");
        }
        if (result.collection.keyType && !SemanticTypes::compatible(*result.collection.keyType, result.index)) {
            throw TypeError(bracket, "map key is incompatible with map key type");
        }
    } else if (result.collection.kind == StaticType::Array
        && result.index.kind != StaticType::Unknown
        && result.index.kind != StaticType::Number) {
        throw TypeError(bracket, "array index must be number");
    } else if (result.collection.kind == StaticType::Range
        && result.index.kind != StaticType::Unknown
        && result.index.kind != StaticType::Number) {
        throw TypeError(bracket, "range index must be number");
    }

    return result;
}

TypeInfo TypeChecker::checkIndex(const IndexExpr& expression)
{
    const IndexTargetTypes target = checkIndexTarget(
        *expression.collection, *expression.index, expression.bracket, "can only index arrays, maps, or ranges");

    TypeInfo result = unknownType();
    if (target.collection.kind == StaticType::Array && target.collection.elementType) {
        result = *target.collection.elementType;
    } else if (target.collection.kind == StaticType::Map && target.collection.valueType) {
        result = *target.collection.valueType;
    } else if (target.collection.kind == StaticType::Range) {
        result = simpleType(StaticType::Number);
    }
    declarationIndex_.recordIndexOperation(
        expression,
        IndexOperationRecord{
            IndexOperationKind::Read,
            target.collection,
            target.index,
            result});
    return result;
}

TypeChecker::CheckedExpression TypeChecker::checkIndexAssignment(const IndexAssignExpr& expression)
{
    const IndexTargetTypes target = checkIndexTarget(
        *expression.collection, *expression.index, expression.bracket, "can only assign array elements, map entries, or range elements");

    if (target.collection.kind == StaticType::Range) {
        throw TypeError(expression.bracket, "cannot assign range elements");
    }

    Binding* binding = findSimpleVariableBinding(*expression.collection);
    if (target.collection.kind == StaticType::Map) {
        const CheckedExpression value = checkExpressionInfo(
            *expression.value,
            target.collection.valueType ? target.collection.valueType.get() : nullptr);
        if (target.collection.valueType && !SemanticTypes::compatible(*target.collection.valueType, value.type)) {
            throw TypeError(expression.bracket, "map value is incompatible with map value type");
        }
        declarationIndex_.recordIndexOperation(
            expression,
            IndexOperationRecord{
                IndexOperationKind::Assign,
                target.collection,
                target.index,
                value.type});
        return value;
    }

    const bool strictElementCheck = binding == nullptr || binding->explicitType;
    const TypeInfo* expectedElement = strictElementCheck ? target.collection.elementType.get() : nullptr;
    const CheckedExpression value = checkExpressionInfo(*expression.value, expectedElement);
    if (strictElementCheck && expectedElement && !SemanticTypes::compatible(*expectedElement, value.type)) {
        throw TypeError(expression.bracket,
            "array index assignment expects " + typeInfoName(*expectedElement)
                + ", got " + typeInfoName(value.type));
    }

    if (binding && binding->type.kind == StaticType::Array) {
        refineArrayBindingFromMutation(*binding, value.type);
        declarationIndex_.recordIndexOperation(
            expression,
            IndexOperationRecord{
                IndexOperationKind::Assign,
                target.collection,
                target.index,
                value.type});
        return CheckedExpression{value.type};
    }

    declarationIndex_.recordIndexOperation(
        expression,
        IndexOperationRecord{
            IndexOperationKind::Assign,
            target.collection,
            target.index,
            value.type});
    return value;
}

TypeChecker::CheckedExpression TypeChecker::checkIndexCompoundAssignment(const IndexCompoundAssignExpr& expression)
{
    const IndexTargetTypes target = checkIndexTarget(
        *expression.collection, *expression.index, expression.bracket, "can only assign array elements, map entries, or range elements");

    if (target.collection.kind == StaticType::Map) {
        throw TypeError(expression.bracket, "compound assignment is not supported for map entries");
    }

    if (target.collection.kind == StaticType::Range) {
        throw TypeError(expression.bracket, "cannot assign range elements");
    }

    if (target.collection.kind == StaticType::Array && target.collection.elementType) {
        checkKnownNumber(expression.op, *target.collection.elementType, "compound assignment target must be number, got ");
    }

    const CheckedExpression value = checkExpressionInfo(*expression.value);
    checkKnownNumber(expression.op, value.type, "compound assignment value must be number, got ");

    const TypeInfo result = simpleType(StaticType::Number);
    declarationIndex_.recordIndexOperation(
        expression,
        IndexOperationRecord{
            IndexOperationKind::CompoundAssign,
            target.collection,
            target.index,
            result});
    return CheckedExpression{result};
}

std::optional<TypeInfo> TypeChecker::checkStructFieldTarget(
    const Expr& objectExpression,
    const Token& name,
    const std::string& nonStructMessage)
{
    const TypeInfo object = checkExpression(objectExpression);

    if (object.kind != StaticType::Unknown && object.kind != StaticType::Struct) {
        throw TypeError(name, nonStructMessage);
    }

    if (object.kind == StaticType::Struct && object.structName) {
        const StructTypeDecl* structType = findStructType(*object.structName);
        const StructFieldType* structField = structType ? findStructField(*structType, name.lexeme) : nullptr;
        if (!structField) {
            throw TypeError(name,
                "struct `" + unqualifiedStructName(*object.structName)
                    + (structType && structType->hasPrivateFields
                            ? "` has no accessible field `"
                            : "` has no field `")
                    + name.lexeme + "`");
        }
        if (structField->isPrivate && !canAccessPrivateFields(*structType)) {
            throw TypeError(name,
                "struct `" + unqualifiedStructName(*object.structName) + "` has no accessible field `"
                    + name.lexeme + "`");
        }
        return structFieldTypeForValue(object, *structType, *structField);
    }

    return std::nullopt;
}

TypeChecker::CheckedExpression TypeChecker::checkFieldAssignment(const FieldAssignExpr& expression)
{
    const std::optional<TypeInfo> structField = checkStructFieldTarget(
        *expression.object, expression.name, "can only assign fields on structs");
    const TypeInfo* expectedFieldType = structField ? &*structField : nullptr;
    const CheckedExpression value = checkExpressionInfo(*expression.value, expectedFieldType);

    if (structField) {
        if (!SemanticTypes::compatible(*structField, value.type)) {
            throw TypeError(expression.name,
                "field `" + expression.name.lexeme + "` expects " + typeInfoName(*structField)
                    + ", got " + typeInfoName(value.type));
        }
        declarationIndex_.recordFieldOperation(
            expression,
            FieldOperationRecord{
                FieldOperationKind::Assign,
                expression.name.lexeme,
                *structField,
                *structField,
                std::nullopt});
        return CheckedExpression{*structField};
    }

    declarationIndex_.recordFieldOperation(
        expression,
        FieldOperationRecord{
            FieldOperationKind::Assign,
            expression.name.lexeme,
            unknownType(),
            value.type,
            std::nullopt});
    return value;
}

TypeChecker::CheckedExpression TypeChecker::checkFieldCompoundAssignment(const FieldCompoundAssignExpr& expression)
{
    const std::optional<TypeInfo> structField = checkStructFieldTarget(
        *expression.object, expression.name, "can only assign fields on structs");
    if (structField) {
        checkKnownNumber(expression.op, *structField, "compound assignment target must be number, got ");
    }

    const CheckedExpression value = checkExpressionInfo(*expression.value);
    checkKnownNumber(expression.op, value.type, "compound assignment value must be number, got ");

    const TypeInfo result = simpleType(StaticType::Number);
    declarationIndex_.recordFieldOperation(
        expression,
        FieldOperationRecord{
            FieldOperationKind::CompoundAssign,
            expression.name.lexeme,
            structField ? *structField : unknownType(),
            result,
            std::nullopt});
    return CheckedExpression{result};
}

void TypeChecker::checkKnownNumber(const Token& token, const TypeInfo& type, const std::string& messagePrefix) const
{
    if (type.kind != StaticType::Unknown && type.kind != StaticType::Number) {
        throw TypeError(token, messagePrefix + typeInfoName(type));
    }
}

TypeInfo TypeChecker::resolveTypeParameterConstraint(const TypeAnnotation& typeName) const
{
    if (typeName.kind == TypeAnnotation::Kind::Simple
        && typeName.typeArguments.empty()
        && (typeName.token.lexeme == "Eq"
            || typeName.token.lexeme == "Ord"
            || typeName.token.lexeme == "Hash")) {
        return capabilityType(typeName.token.lexeme);
    }
    return resolveAnnotation(typeName);
}

TypeInfo TypeChecker::resolveTypeParameterConstraints(const TypeParameter& parameter) const
{
    std::vector<TypeInfo> constraints;
    constraints.reserve(parameter.constraints.size());
    for (const TypeAnnotation& annotation : parameter.constraints) {
        constraints.push_back(resolveTypeParameterConstraint(annotation));
    }
    if (constraints.size() == 1) {
        return std::move(constraints.front());
    }
    for (const TypeInfo& constraint : constraints) {
        if (constraint.kind != StaticType::Capability) {
            throw TypeError(parameter.name,
                "combined type parameter constraints must be capabilities");
        }
    }
    return capabilitySetType(std::move(constraints));
}

TypeInfo TypeChecker::resolveAnnotation(const TypeAnnotation& typeName) const
{
    if (typeName.kind == TypeAnnotation::Kind::Nullable
        || typeName.kind == TypeAnnotation::Kind::Optional) {
        return nullableType(resolveAnnotation(*typeName.innerType));
    }

    if (typeName.kind == TypeAnnotation::Kind::Array) {
        return arrayType(resolveAnnotation(*typeName.elementType));
    }

    if (typeName.kind == TypeAnnotation::Kind::Map) {
        TypeInfo keyType = resolveAnnotation(*typeName.keyType);
        if (!mapKeyTypeAllowed(keyType)) {
            throw TypeError(typeName.token, "map key must be nil, number, bool, or string");
        }
        return mapType(std::move(keyType), resolveAnnotation(*typeName.valueType));
    }

    if (typeName.kind == TypeAnnotation::Kind::Function) {
        std::vector<TypeInfo> parameterTypes;
        parameterTypes.reserve(typeName.parameterTypes.size());
        for (const TypeAnnotation& parameter : typeName.parameterTypes) {
            parameterTypes.push_back(resolveAnnotation(parameter));
        }
        return functionType(std::move(parameterTypes), resolveAnnotation(*typeName.returnType));
    }

    if (typeName.kind == TypeAnnotation::Kind::Qualified) {
        const NamespaceImport* namespaceImport = findNamespace(typeName.qualifier.lexeme);
        if (!namespaceImport) {
            throw TypeError(typeName.qualifier, "unknown module namespace `" + typeName.qualifier.lexeme + "`");
        }
        const auto structFound = namespaceImport->structs.find(typeName.token.lexeme);
        if (structFound != namespaceImport->structs.end()) {
            return resolveNamedStructAnnotation(
                typeName,
                qualifiedStructName(typeName.qualifier, typeName.token),
                structFound->second);
        }
        const auto enumFound = namespaceImport->enums.find(typeName.token.lexeme);
        if (enumFound != namespaceImport->enums.end()) {
            return resolveNamedEnumAnnotation(
                typeName,
                qualifiedStructName(typeName.qualifier, typeName.token),
                enumFound->second);
        }
        throw TypeError(typeName.token,
            "module namespace `" + typeName.qualifier.lexeme + "` has no exported type `"
                + typeName.token.lexeme + "`");
    }

   if (const TypeInfo* typeParameter = findTypeParameter(typeName.token.lexeme)) {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type parameter `" + typeName.token.lexeme + "` is not generic");
        }
        return *typeParameter;
    }

    if (typeName.token.lexeme == "number") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `number` is not generic");
        }
        return simpleType(StaticType::Number);
    }
    if (typeName.token.lexeme == "bool") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `bool` is not generic");
        }
        return simpleType(StaticType::Bool);
    }
    if (typeName.token.lexeme == "string") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `string` is not generic");
        }
        return simpleType(StaticType::String);
    }
    if (typeName.token.lexeme == "nil") {
        if (!typeName.typeArguments.empty()) {
            throw TypeError(typeName.token, "type `nil` is not generic");
        }
        return simpleType(StaticType::Nil);
    }
    if (const StructTypeDecl* structType = findStructType(typeName.token.lexeme)) {
        return resolveNamedStructAnnotation(
            typeName, typeName.token.lexeme, *structType);
    }
    if (const EnumTypeDecl* enumType = findEnumType(typeName.token.lexeme)) {
        return resolveNamedEnumAnnotation(typeName, typeName.token.lexeme, *enumType);
    }
   throw TypeError(typeName.token, "unknown type `" + typeName.token.lexeme + "`");
}

void TypeChecker::checkAssignable(const Token& token, const std::string& context, const TypeInfo& expected, const TypeInfo& actual) const
{
    if (!SemanticTypes::compatible(expected, actual)) {
        throw TypeError(token, context);
    }
}

TypeInfo TypeChecker::checkUnary(const UnaryExpr& expression)
{
    const TypeInfo right = checkExpression(*expression.right);
    switch (expression.op.type) {
    case TokenType::Minus:
        if (SemanticTypes::isKnown(right) && right.kind != StaticType::Number) {
            throw TypeError(expression.op, "unary `-` expects number, got " + typeInfoName(right));
        }
        return simpleType(StaticType::Number);
    case TokenType::Bang:
        return simpleType(StaticType::Bool);
    default:
        throw TypeError(expression.op, "unsupported unary operator `" + expression.op.lexeme + "`");
    }
}

TypeInfo TypeChecker::checkBinary(const BinaryExpr& expression)
{
    const TypeInfo left = checkExpression(*expression.left);
    const TypeInfo right = checkExpression(*expression.right);

    const auto requireCapability = [&](const TypeInfo& operand, const std::string& capability) {
        if (operand.kind != StaticType::TypeParameter
            || SemanticTypes::satisfiesCapability(operand, capabilityType(capability))) {
            return;
        }
        const std::string parameter = operand.typeParameterName.value_or("<unknown>");
        throw TypeError(expression.op,
            "binary `" + expression.op.lexeme + "` requires type parameter `"
                + parameter + "` to satisfy " + capability);
    };

    switch (expression.op.type) {
    case TokenType::Plus:
        if (!SemanticTypes::isKnown(left) || !SemanticTypes::isKnown(right)) {
            return unknownType();
        }
        if (left.kind == StaticType::Number && right.kind == StaticType::Number) {
            return simpleType(StaticType::Number);
        }
        if (left.kind == StaticType::String && right.kind == StaticType::String) {
            return simpleType(StaticType::String);
        }
        throw TypeError(expression.op, "binary `+` expects two numbers or two strings, got "
            + typeInfoName(left) + " and " + typeInfoName(right));
    case TokenType::Minus:
    case TokenType::Star:
    case TokenType::Slash:
        if (!SemanticTypes::isKnown(left) || !SemanticTypes::isKnown(right)) {
            return simpleType(StaticType::Number);
        }
        if (left.kind != StaticType::Number || right.kind != StaticType::Number) {
            throw TypeError(expression.op, binaryTypesMessage(expression, left, right));
        }
        return simpleType(StaticType::Number);
    case TokenType::Greater:
    case TokenType::GreaterEqual:
    case TokenType::Less:
    case TokenType::LessEqual: {
        requireCapability(left, "Ord");
        requireCapability(right, "Ord");
        if (!SemanticTypes::isKnown(left) || !SemanticTypes::isKnown(right)) {
            return simpleType(StaticType::Bool);
        }
        const bool leftIsOrderedTypeParameter
            = left.kind == StaticType::TypeParameter
            && SemanticTypes::satisfiesCapability(left, capabilityType("Ord"));
        const bool rightIsOrderedTypeParameter
            = right.kind == StaticType::TypeParameter
            && SemanticTypes::satisfiesCapability(right, capabilityType("Ord"));
        if ((!leftIsOrderedTypeParameter
                && left.kind != StaticType::Number
                && left.kind != StaticType::String)
            || (!rightIsOrderedTypeParameter
                && right.kind != StaticType::Number
                && right.kind != StaticType::String)) {
            throw TypeError(expression.op, orderingTypesMessage(expression, left, right));
        }
        return simpleType(StaticType::Bool);
    }
    case TokenType::EqualEqual:
    case TokenType::BangEqual:
        requireCapability(left, "Eq");
        requireCapability(right, "Eq");
        return simpleType(StaticType::Bool);
    default:
        throw TypeError(expression.op, "unsupported binary operator `" + expression.op.lexeme + "`");
    }
}

bool TypeChecker::isGlobalBinding(const Binding& binding) const
{
    return binding.scopeDepth == 0;
}

bool TypeChecker::isCurrentFunctionBinding(const Binding& binding) const
{
    return binding.functionDepth == functionDepth_;
}
