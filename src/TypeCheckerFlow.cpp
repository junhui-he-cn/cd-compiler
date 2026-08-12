#include "TypeChecker.hpp"

#include "TypeCheckerInternal.hpp"

#include <string>
#include <utility>

std::optional<FlowNarrowing> TypeChecker::nonNilNarrowingForVariable(const VariableExpr& variable) const
{
    const Binding* binding = findVariable(variable.name.lexeme);
    if (!binding || !SemanticTypes::isNullable(binding->type)) {
        return std::nullopt;
    }
    return FlowNarrowing{binding->resolvedName, *binding->type.nullableOf};
}

std::optional<std::string> TypeChecker::fieldFlowFactName(const Expr& object, const Token& name) const
{
    std::optional<std::string> parentFactName;
    TypeInfo objectType = unknownType();

    const auto* variable = dynamic_cast<const VariableExpr*>(&object);
    if (variable) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            return std::nullopt;
        }
        parentFactName = binding->resolvedName;
        objectType = variableType(*binding);
    } else {
        const auto* field = dynamic_cast<const FieldAccessExpr*>(&object);
        if (!field) {
            return std::nullopt;
        }
        parentFactName = fieldFlowFactName(*field->object, field->name);
        if (!parentFactName) {
            return std::nullopt;
        }
        const TypedExpressionRecord* typedObject = declarationIndex_.typedExpression(object);
        if (!typedObject) {
            return std::nullopt;
        }
        objectType = typedObject->type;
    }

    if (objectType.kind != StaticType::Struct || !objectType.structName) {
        return std::nullopt;
    }

    const StructTypeDecl* structType = findStructType(*objectType.structName);
    if (!structType || !findStructField(*structType, name.lexeme)) {
        return std::nullopt;
    }

    return *parentFactName + "." + name.lexeme;
}

std::optional<FlowNarrowing> TypeChecker::nonNilNarrowingForField(const FieldAccessExpr& field) const
{
    const std::optional<std::string> factName = fieldFlowFactName(*field.object, field.name);
    if (!factName) {
        return std::nullopt;
    }

    TypeInfo objectType = unknownType();
    if (const auto* variable = dynamic_cast<const VariableExpr*>(field.object.get())) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            return std::nullopt;
        }
        objectType = variableType(*binding);
    } else if (const TypedExpressionRecord* typedObject = declarationIndex_.typedExpression(*field.object)) {
        objectType = typedObject->type;
    } else {
        return std::nullopt;
    }

    const StructTypeDecl* structType = objectType.structName
        ? findStructType(*objectType.structName)
        : nullptr;
    const StructFieldType* structField = structType
        ? findStructField(*structType, field.name.lexeme)
        : nullptr;
    if (!structType || !structField) {
        return std::nullopt;
    }

    const TypeInfo fieldType = structFieldTypeForValue(objectType, *structType, *structField);
    if (!SemanticTypes::isNullable(fieldType)) {
        return std::nullopt;
    }
    return FlowNarrowing{*factName, *fieldType.nullableOf};
}

std::optional<std::string> TypeChecker::indexFlowFactName(
    const Expr& collection,
    const Expr& index) const
{
    std::optional<std::string> indexFactName = normalizedIntegerLiteral(index);
    if (!indexFactName) {
        const auto* variable = dynamic_cast<const VariableExpr*>(&index);
        if (!variable) {
            return std::nullopt;
        }

        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding || variableType(*binding).kind != StaticType::Number) {
            return std::nullopt;
        }
        indexFactName = binding->resolvedName;
    }

    if (indexFactName->empty()) {
        return std::nullopt;
    }

    std::optional<std::string> parentFactName;
    TypeInfo collectionType = unknownType();
    if (const auto* variable = dynamic_cast<const VariableExpr*>(&collection)) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            return std::nullopt;
        }
        parentFactName = binding->resolvedName;
        collectionType = variableType(*binding);
    } else if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&collection)) {
        parentFactName = fieldFlowFactName(*field->object, field->name);
        const TypedExpressionRecord* typedCollection = declarationIndex_.typedExpression(collection);
        if (!parentFactName || !typedCollection) {
            return std::nullopt;
        }
        collectionType = typedCollection->type;
    } else if (const auto* nestedIndex = dynamic_cast<const IndexExpr*>(&collection)) {
        parentFactName = indexFlowFactName(*nestedIndex->collection, *nestedIndex->index);
        const TypedExpressionRecord* typedCollection = declarationIndex_.typedExpression(collection);
        if (!parentFactName || !typedCollection) {
            return std::nullopt;
        }
        collectionType = typedCollection->type;
    } else {
        return std::nullopt;
    }

    if (collectionType.kind != StaticType::Array || !collectionType.elementType) {
        return std::nullopt;
    }

    return *parentFactName + "[" + *indexFactName + "]";
}

std::optional<FlowNarrowing> TypeChecker::nonNilNarrowingForIndex(const IndexExpr& index) const
{
    const std::optional<std::string> factName = indexFlowFactName(
        *index.collection,
        *index.index);
    if (!factName) {
        return std::nullopt;
    }

    TypeInfo collectionType = unknownType();
    if (const auto* variable = dynamic_cast<const VariableExpr*>(index.collection.get())) {
        const Binding* binding = findVariable(variable->name.lexeme);
        if (!binding) {
            return std::nullopt;
        }
        collectionType = variableType(*binding);
    } else if (const TypedExpressionRecord* typedCollection = declarationIndex_.typedExpression(*index.collection)) {
        collectionType = typedCollection->type;
    } else {
        return std::nullopt;
    }

    if (collectionType.kind != StaticType::Array || !collectionType.elementType
        || !SemanticTypes::isNullable(*collectionType.elementType)) {
        return std::nullopt;
    }

    return FlowNarrowing{*factName, *collectionType.elementType->nullableOf};
}

std::optional<FlowNarrowing> TypeChecker::nonNilNarrowingForTarget(const Expr& target) const
{
    if (const auto* variable = dynamic_cast<const VariableExpr*>(&target)) {
        return nonNilNarrowingForVariable(*variable);
    }
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&target)) {
        return nonNilNarrowingForField(*field);
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(&target)) {
        return nonNilNarrowingForIndex(*index);
    }
    return std::nullopt;
}

