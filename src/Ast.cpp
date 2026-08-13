#include "Ast.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace {

void writeTypeArguments(std::ostream& out, const std::vector<TypeAnnotation>& arguments);

void writeIndent(std::ostream& out, int indent)
{
    for (int i = 0; i < indent; ++i) {
        out << "  ";
    }
}

void writeExpr(std::ostream& out, const ExprPtr& expr)
{
    if (expr) {
        expr->print(out);
    } else {
        out << "nil";
    }
}

void writeTypeAnnotation(std::ostream& out, const TypeAnnotation& annotation)
{
    if (annotation.kind == TypeAnnotation::Kind::Simple) {
        out << annotation.token.lexeme;
        writeTypeArguments(out, annotation.typeArguments);
        return;
    }

    if (annotation.kind == TypeAnnotation::Kind::Qualified) {
        out << annotation.qualifier.lexeme << '.' << annotation.token.lexeme;
        writeTypeArguments(out, annotation.typeArguments);
        return;
    }

    if (annotation.kind == TypeAnnotation::Kind::Array) {
        out << '[';
        writeTypeAnnotation(out, *annotation.elementType);
        out << ']';
        return;
    }

    if (annotation.kind == TypeAnnotation::Kind::Map) {
        out << "map<";
        writeTypeAnnotation(out, *annotation.keyType);
        out << ", ";
        writeTypeAnnotation(out, *annotation.valueType);
        out << '>';
        return;
    }

    if (annotation.kind == TypeAnnotation::Kind::Nullable) {
        out << "optional<";
        writeTypeAnnotation(out, *annotation.innerType);
        out << '>';
        return;
    }

    if (annotation.kind == TypeAnnotation::Kind::Optional) {
        out << "optional<";
        writeTypeAnnotation(out, *annotation.innerType);
        out << '>';
        return;
    }

    out << "fun(";
    for (std::size_t i = 0; i < annotation.parameterTypes.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        writeTypeAnnotation(out, annotation.parameterTypes[i]);
    }
    out << "): ";
    writeTypeAnnotation(out, *annotation.returnType);
}

void writeTypeArguments(std::ostream& out, const std::vector<TypeAnnotation>& arguments)
{
    if (arguments.empty()) {
        return;
    }

    out << '<';
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        writeTypeAnnotation(out, arguments[i]);
    }
    out << '>';
}

void writeTypeParameterList(std::ostream& out, const std::vector<TypeParameter>& parameters)
{
    if (parameters.empty()) {
        return;
    }

    out << '<';
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << parameters[i].name.lexeme;
        if (!parameters[i].constraints.empty()) {
            out << ": ";
            for (std::size_t constraint = 0; constraint < parameters[i].constraints.size(); ++constraint) {
                if (constraint != 0) {
                    out << " + ";
                }
                writeTypeAnnotation(out, parameters[i].constraints[constraint]);
            }
        }
    }
    out << '>';
}

void writeOptionalTypeAnnotation(std::ostream& out, const std::optional<TypeAnnotation>& annotation)
{
    if (annotation) {
        out << ": ";
        writeTypeAnnotation(out, *annotation);
    }
}

void writeParameterList(std::ostream& out, const std::vector<Parameter>& parameters)
{
    out << '(';
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << parameters[i].name.lexeme;
        writeOptionalTypeAnnotation(out, parameters[i].typeName);
    }
    out << ')';
}

void writeReturnAnnotation(std::ostream& out, const std::optional<TypeAnnotation>& returnTypeName)
{
    writeOptionalTypeAnnotation(out, returnTypeName);
}

void writePattern(std::ostream& out, const Pattern& pattern)
{
    pattern.print(out);
}

void writeInlineStmt(std::ostream& out, const Stmt& stmt)
{
    if (const auto* module = dynamic_cast<const ModuleStmt*>(&stmt)) {
        out << "(module " << module->moduleId;
        for (const auto& child : module->statements) {
            out << ' ';
            writeInlineStmt(out, *child);
        }
        out << ')';
        return;
    }

    if (const auto* import = dynamic_cast<const ImportStmt*>(&stmt)) {
        out << "(import " << import->path.lexeme;
        if (import->alias) {
            out << " as " << import->alias->lexeme;
        }
        out << ')';
        return;
    }

    if (const auto* exportStmt = dynamic_cast<const ExportStmt*>(&stmt)) {
        out << "(export";
        for (const auto& name : exportStmt->names) {
            out << ' ' << name.lexeme;
        }
        out << ')';
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt)) {
        out << "(return ";
        writeExpr(out, returnStmt->value);
        out << ')';
        return;
    }

    if (const auto* expressionStmt = dynamic_cast<const ExpressionStmt*>(&stmt)) {
        out << "(expr ";
        writeExpr(out, expressionStmt->expression);
        out << ')';
        return;
    }

    if (const auto* printStmt = dynamic_cast<const PrintStmt*>(&stmt)) {
        out << "(print ";
        writeExpr(out, printStmt->expression);
        out << ')';
        return;
    }

    if (const auto* letStmt = dynamic_cast<const LetStmt*>(&stmt)) {
        out << "(let " << letStmt->name.lexeme;
        writeOptionalTypeAnnotation(out, letStmt->typeName);
        out << " = ";
        writeExpr(out, letStmt->initializer);
        out << ')';
        return;
    }

    if (const auto* blockStmt = dynamic_cast<const BlockStmt*>(&stmt)) {
        out << "(block";
        for (const auto& child : blockStmt->statements) {
            out << ' ';
            writeInlineStmt(out, *child);
        }
        out << ')';
        return;
    }

    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
        out << "(if ";
        writeExpr(out, ifStmt->condition);
        out << ' ';
        writeInlineStmt(out, *ifStmt->thenBranch);
        if (ifStmt->elseBranch) {
            out << ' ';
            writeInlineStmt(out, *ifStmt->elseBranch);
        }
        out << ')';
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
        out << "(while ";
        writeExpr(out, whileStmt->condition);
        out << ' ';
        writeInlineStmt(out, *whileStmt->body);
        out << ')';
        return;
    }

    if (const auto* forStmt = dynamic_cast<const ForStmt*>(&stmt)) {
        out << "(for ";
        if (forStmt->initializer) {
            writeInlineStmt(out, *forStmt->initializer);
        } else {
            out << "nil";
        }
        out << ' ';
        writeExpr(out, forStmt->condition);
        out << ' ';
        writeExpr(out, forStmt->increment);
        out << ' ';
        writeInlineStmt(out, *forStmt->body);
        out << ')';
        return;
    }

    if (dynamic_cast<const BreakStmt*>(&stmt)) {
        out << "(break)";
        return;
    }

    if (dynamic_cast<const ContinueStmt*>(&stmt)) {
        out << "(continue)";
        return;
    }

    if (const auto* functionStmt = dynamic_cast<const FunctionStmt*>(&stmt)) {
        out << "(fun " << functionStmt->name.lexeme;
        writeTypeParameterList(out, functionStmt->typeParameters);
        out << ' ';
        writeParameterList(out, functionStmt->parameters);
        writeReturnAnnotation(out, functionStmt->returnTypeName);
        for (const auto& child : functionStmt->body) {
            out << ' ';
            writeInlineStmt(out, *child);
        }
        out << ')';
        return;
    }

    if (const auto* implStmt = dynamic_cast<const ImplStmt*>(&stmt)) {
        out << "(impl " << implStmt->typeName.lexeme;
        writeTypeParameterList(out, implStmt->typeParameters);
        for (const MethodDecl& method : implStmt->methods) {
            out << (method.isOperator ? " (operator " : " (method ") << method.name.lexeme;
            writeTypeParameterList(out, method.typeParameters);
            writeParameterList(out, method.parameters);
            writeReturnAnnotation(out, method.returnTypeName);
            for (const auto& child : method.body) {
                out << ' ';
                writeInlineStmt(out, *child);
            }
            out << ')';
        }
        out << ')';
        return;
    }

    if (const auto* structDecl = dynamic_cast<const StructDeclStmt*>(&stmt)) {
        out << "(struct " << structDecl->name.lexeme;
        writeTypeParameterList(out, structDecl->typeParameters);
        for (const StructFieldDecl& field : structDecl->fields) {
            out << ' ';
            if (field.isPrivate) {
                out << "private ";
            }
            out << field.name.lexeme << ": ";
            writeTypeAnnotation(out, field.typeName);
        }
        out << ')';
        return;
    }

    if (const auto* enumDecl = dynamic_cast<const EnumDeclStmt*>(&stmt)) {
        out << "(enum " << enumDecl->name.lexeme;
        writeTypeParameterList(out, enumDecl->typeParameters);
        for (const EnumVariantDecl& variant : enumDecl->variants) {
            out << " (variant " << variant.name.lexeme;
            for (std::size_t i = 0; i < variant.payloadTypes.size(); ++i) {
                out << ' ';
                if (i < variant.payloadNames.size() && variant.payloadNames[i]) {
                    out << variant.payloadNames[i]->lexeme << ": ";
                }
                writeTypeAnnotation(out, variant.payloadTypes[i]);
            }
            out << ')';
        }
        out << ')';
        return;
    }

    if (const auto* match = dynamic_cast<const MatchStmt*>(&stmt)) {
        out << "(match ";
        writeExpr(out, match->value);
        for (const MatchArm& arm : match->arms) {
            out << " (arm ";
            writePattern(out, *arm.pattern);
            if (arm.guard) {
                out << " if ";
                writeExpr(out, arm.guard);
            }
            out << ' ';
            writeInlineStmt(out, *arm.body);
            out << ')';
        }
        out << ')';
        return;
    }

    out << "(stmt)";
}

} // namespace

TypeAnnotation TypeAnnotation::simple(Token token)
{
    TypeAnnotation result;
    result.kind = Kind::Simple;
    result.token = std::move(token);
    return result;
}

TypeAnnotation TypeAnnotation::qualified(Token qualifier, Token name)
{
    TypeAnnotation result;
    result.kind = Kind::Qualified;
    result.qualifier = std::move(qualifier);
    result.token = std::move(name);
    return result;
}

TypeAnnotation TypeAnnotation::function(Token token, std::vector<TypeAnnotation> parameterTypes, TypeAnnotation returnType)
{
    TypeAnnotation result;
    result.kind = Kind::Function;
    result.token = std::move(token);
    result.parameterTypes = std::move(parameterTypes);
    result.returnType = std::make_shared<TypeAnnotation>(std::move(returnType));
    return result;
}

TypeAnnotation TypeAnnotation::array(Token token, TypeAnnotation elementType)
{
    TypeAnnotation result;
    result.kind = Kind::Array;
    result.token = std::move(token);
    result.elementType = std::make_shared<TypeAnnotation>(std::move(elementType));
    return result;
}

TypeAnnotation TypeAnnotation::map(Token token, TypeAnnotation keyType, TypeAnnotation valueType)
{
    TypeAnnotation result;
    result.kind = Kind::Map;
    result.token = std::move(token);
    result.keyType = std::make_shared<TypeAnnotation>(std::move(keyType));
    result.valueType = std::make_shared<TypeAnnotation>(std::move(valueType));
    return result;
}

TypeAnnotation TypeAnnotation::nullable(Token token, TypeAnnotation innerType)
{
    TypeAnnotation result;
    result.kind = Kind::Nullable;
    result.token = std::move(token);
    result.innerType = std::make_shared<TypeAnnotation>(std::move(innerType));
    return result;
}

TypeAnnotation TypeAnnotation::optional(Token token, TypeAnnotation innerType)
{
    TypeAnnotation result;
    result.kind = Kind::Optional;
    result.token = std::move(token);
    result.innerType = std::make_shared<TypeAnnotation>(std::move(innerType));
    return result;
}

MethodDecl::MethodDecl(
    Token name,
    std::vector<TypeParameter> typeParameters,
    std::vector<Parameter> parameters,
    std::optional<TypeAnnotation> returnTypeName,
    std::vector<StmtPtr> body,
    bool isOperator)
    : name(std::move(name))
    , typeParameters(std::move(typeParameters))
    , parameters(std::move(parameters))
    , returnTypeName(std::move(returnTypeName))
    , body(std::move(body))
    , isOperator(isOperator)
{
}

LiteralExpr::LiteralExpr(std::string value)
    : value(std::move(value))
{
}

void LiteralExpr::print(std::ostream& out) const
{
    out << value;
}

VariableExpr::VariableExpr(Token name)
    : name(std::move(name))
{
}

void VariableExpr::print(std::ostream& out) const
{
    out << name.lexeme;
}

AssignExpr::AssignExpr(Token name, ExprPtr value)
    : name(std::move(name))
    , value(std::move(value))
{
}

void AssignExpr::print(std::ostream& out) const
{
    out << "(= " << name.lexeme << ' ';
    writeExpr(out, value);
    out << ')';
}

CompoundAssignExpr::CompoundAssignExpr(Token name, Token op, ExprPtr value)
    : name(std::move(name))
    , op(std::move(op))
    , value(std::move(value))
{
}

void CompoundAssignExpr::print(std::ostream& out) const
{
    out << '(' << op.lexeme << ' ' << name.lexeme << ' ';
    writeExpr(out, value);
    out << ')';
}

IndexAssignExpr::IndexAssignExpr(ExprPtr collection, Token bracket, ExprPtr index, ExprPtr value)
    : collection(std::move(collection))
    , bracket(std::move(bracket))
    , index(std::move(index))
    , value(std::move(value))
{
}

void IndexAssignExpr::print(std::ostream& out) const
{
    out << "(= (index ";
    writeExpr(out, collection);
    out << ' ';
    writeExpr(out, index);
    out << ") ";
    writeExpr(out, value);
    out << ')';
}

IndexCompoundAssignExpr::IndexCompoundAssignExpr(ExprPtr collection, Token bracket, ExprPtr index, Token op, ExprPtr value)
    : collection(std::move(collection))
    , bracket(std::move(bracket))
    , index(std::move(index))
    , op(std::move(op))
    , value(std::move(value))
{
}

void IndexCompoundAssignExpr::print(std::ostream& out) const
{
    out << '(' << op.lexeme << " (index ";
    writeExpr(out, collection);
    out << ' ';
    writeExpr(out, index);
    out << ") ";
    writeExpr(out, value);
    out << ')';
}

UnaryExpr::UnaryExpr(Token op, ExprPtr right)
    : op(std::move(op))
    , right(std::move(right))
{
}

void UnaryExpr::print(std::ostream& out) const
{
    out << '(' << op.lexeme << ' ';
    writeExpr(out, right);
    out << ')';
}

BinaryExpr::BinaryExpr(ExprPtr left, Token op, ExprPtr right)
    : left(std::move(left))
    , op(std::move(op))
    , right(std::move(right))
{
}

void BinaryExpr::print(std::ostream& out) const
{
    out << '(' << op.lexeme << ' ';
    writeExpr(out, left);
    out << ' ';
    writeExpr(out, right);
    out << ')';
}

LogicalExpr::LogicalExpr(ExprPtr left, Token op, ExprPtr right)
    : left(std::move(left))
    , op(std::move(op))
    , right(std::move(right))
{
}

void LogicalExpr::print(std::ostream& out) const
{
    out << '(' << op.lexeme << ' ';
    writeExpr(out, left);
    out << ' ';
    writeExpr(out, right);
    out << ')';
}

GroupingExpr::GroupingExpr(ExprPtr expression)
    : expression(std::move(expression))
{
}

void GroupingExpr::print(std::ostream& out) const
{
    out << "(group ";
    writeExpr(out, expression);
    out << ')';
}

CallExpr::CallExpr(
    ExprPtr callee,
    Token paren,
    std::vector<TypeAnnotation> typeArguments,
    std::vector<ExprPtr> arguments)
    : callee(std::move(callee))
    , paren(std::move(paren))
    , typeArguments(std::move(typeArguments))
    , arguments(std::move(arguments))
{
}

void CallExpr::print(std::ostream& out) const
{
    out << "(call ";
    writeExpr(out, callee);
    writeTypeArguments(out, typeArguments);
    for (const auto& argument : arguments) {
        out << ' ';
        writeExpr(out, argument);
    }
    out << ')';
}

MemberCallExpr::MemberCallExpr(
    ExprPtr receiver,
    Token name,
    Token paren,
    std::vector<TypeAnnotation> typeArguments,
    std::vector<ExprPtr> arguments)
    : receiver(std::move(receiver))
    , name(std::move(name))
    , paren(std::move(paren))
    , typeArguments(std::move(typeArguments))
    , arguments(std::move(arguments))
{
}

void MemberCallExpr::print(std::ostream& out) const
{
    out << "(member-call ";
    writeExpr(out, receiver);
    out << ' ' << name.lexeme;
    writeTypeArguments(out, typeArguments);
    for (const auto& argument : arguments) {
        out << ' ';
        writeExpr(out, argument);
    }
    out << ')';
}

ArrayExpr::ArrayExpr(Token bracket, std::vector<ExprPtr> elements)
    : bracket(std::move(bracket))
    , elements(std::move(elements))
{
}

void ArrayExpr::print(std::ostream& out) const
{
    out << "(array";
    for (const auto& element : elements) {
        out << ' ';
        writeExpr(out, element);
    }
    out << ')';
}

MapExpr::MapExpr(Token brace, std::vector<MapEntry> entries)
    : brace(std::move(brace))
    , entries(std::move(entries))
{
}

void MapExpr::print(std::ostream& out) const
{
    out << "(map";
    for (const MapEntry& entry : entries) {
        out << " (entry ";
        writeExpr(out, entry.key);
        out << ' ';
        writeExpr(out, entry.value);
        out << ')';
    }
    out << ')';
}

StructConstructExpr::StructConstructExpr(
    std::optional<Token> qualifier,
    Token name,
    std::vector<TypeAnnotation> typeArguments,
    std::vector<StructField> fields)
    : qualifier(std::move(qualifier))
    , name(std::move(name))
    , typeArguments(std::move(typeArguments))
    , fields(std::move(fields))
{
}

void StructConstructExpr::print(std::ostream& out) const
{
    out << "(construct ";
    if (qualifier) {
        out << qualifier->lexeme << '.';
    }
    out << name.lexeme;
    writeTypeArguments(out, typeArguments);
    for (const StructField& field : fields) {
        out << ' ' << field.name.lexeme << ": ";
        writeExpr(out, field.value);
    }
    out << ')';
}

IndexExpr::IndexExpr(ExprPtr collection, Token bracket, ExprPtr index)
    : collection(std::move(collection))
    , bracket(std::move(bracket))
    , index(std::move(index))
{
}

void IndexExpr::print(std::ostream& out) const
{
    out << "(index ";
    writeExpr(out, collection);
    out << ' ';
    writeExpr(out, index);
    out << ')';
}

FieldAccessExpr::FieldAccessExpr(ExprPtr object, Token name)
    : object(std::move(object))
    , name(std::move(name))
{
}

void FieldAccessExpr::print(std::ostream& out) const
{
    out << "(field ";
    writeExpr(out, object);
    out << ' ' << name.lexeme << ')';
}

FieldAssignExpr::FieldAssignExpr(ExprPtr object, Token name, ExprPtr value)
    : object(std::move(object))
    , name(std::move(name))
    , value(std::move(value))
{
}

void FieldAssignExpr::print(std::ostream& out) const
{
    out << "(= (field ";
    writeExpr(out, object);
    out << ' ' << name.lexeme << ") ";
    writeExpr(out, value);
    out << ')';
}

FieldCompoundAssignExpr::FieldCompoundAssignExpr(ExprPtr object, Token name, Token op, ExprPtr value)
    : object(std::move(object))
    , name(std::move(name))
    , op(std::move(op))
    , value(std::move(value))
{
}

void FieldCompoundAssignExpr::print(std::ostream& out) const
{
    out << '(' << op.lexeme << " (field ";
    writeExpr(out, object);
    out << ' ' << name.lexeme << ") ";
    writeExpr(out, value);
    out << ')';
}

FunctionExpr::FunctionExpr(
    Token keyword,
    std::vector<TypeParameter> typeParameters,
    std::vector<Parameter> parameters,
    std::optional<TypeAnnotation> returnTypeName,
    std::vector<StmtPtr> body)
    : keyword(std::move(keyword))
    , typeParameters(std::move(typeParameters))
    , parameters(std::move(parameters))
    , returnTypeName(std::move(returnTypeName))
    , body(std::move(body))
{
}

void FunctionExpr::print(std::ostream& out) const
{
    out << "(fun ";
    writeTypeParameterList(out, typeParameters);
    writeParameterList(out, parameters);
    writeReturnAnnotation(out, returnTypeName);
    for (const auto& statement : body) {
        out << ' ';
        writeInlineStmt(out, *statement);
    }
    out << ')';
}

WildcardPattern::WildcardPattern(Token name)
    : name(std::move(name))
{
}

void WildcardPattern::print(std::ostream& out) const
{
    out << '_';
}

VariablePattern::VariablePattern(Token name)
    : name(std::move(name))
{
}

void VariablePattern::print(std::ostream& out) const
{
    out << name.lexeme;
}

LiteralPattern::LiteralPattern(Token value)
    : value(std::move(value))
{
}

void LiteralPattern::print(std::ostream& out) const
{
    out << value.lexeme;
}

OrPattern::OrPattern(Token pipe, std::vector<PatternPtr> alternatives)
    : pipe(std::move(pipe))
    , alternatives(std::move(alternatives))
{
}

void OrPattern::print(std::ostream& out) const
{
    out << "(or";
    for (const PatternPtr& alternative : alternatives) {
        out << ' ';
        writePattern(out, *alternative);
    }
    out << ')';
}

RecordPattern::RecordPattern(
    std::optional<Token> qualifier,
    Token name,
    std::vector<RecordPatternField> fields)
    : qualifier(std::move(qualifier))
    , name(std::move(name))
    , fields(std::move(fields))
{
}

void RecordPattern::print(std::ostream& out) const
{
    if (qualifier) {
        out << qualifier->lexeme << '.';
    }
    out << name.lexeme << " {";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << fields[i].name.lexeme << ": ";
        writePattern(out, *fields[i].pattern);
    }
    out << '}';
}

VariantPattern::VariantPattern(
    std::optional<Token> qualifier,
    Token name,
    std::vector<PatternPtr> arguments,
    std::vector<std::optional<Token>> argumentNames)
    : qualifier(std::move(qualifier))
    , name(std::move(name))
    , arguments(std::move(arguments))
    , argumentNames(std::move(argumentNames))
{
}

void VariantPattern::print(std::ostream& out) const
{
    if (qualifier) {
        out << qualifier->lexeme << '.';
    }
    out << name.lexeme;
    if (!arguments.empty()) {
        out << '(';
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            if (i < argumentNames.size() && argumentNames[i]) {
                out << argumentNames[i]->lexeme << ": ";
            }
            writePattern(out, *arguments[i]);
        }
        out << ')';
    }
}

EnumDeclStmt::EnumDeclStmt(Token name, std::vector<TypeParameter> typeParameters, std::vector<EnumVariantDecl> variants)
    : name(std::move(name))
    , typeParameters(std::move(typeParameters))
    , variants(std::move(variants))
{
}

void EnumDeclStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Enum " << name.lexeme;
    writeTypeParameterList(out, typeParameters);
    out << " {";
    for (std::size_t i = 0; i < variants.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << variants[i].name.lexeme;
        if (!variants[i].payloadTypes.empty()) {
            out << '(';
            for (std::size_t j = 0; j < variants[i].payloadTypes.size(); ++j) {
                if (j != 0) {
                    out << ", ";
                }
                if (j < variants[i].payloadNames.size() && variants[i].payloadNames[j]) {
                    out << variants[i].payloadNames[j]->lexeme << ": ";
                }
                writeTypeAnnotation(out, variants[i].payloadTypes[j]);
            }
            out << ')';
        }
    }
    out << "}\n";
}

MatchStmt::MatchStmt(ExprPtr value, std::vector<MatchArm> arms)
    : value(std::move(value))
    , arms(std::move(arms))
{
}

void MatchStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Match ";
    writeExpr(out, value);
    out << '\n';
    for (const MatchArm& arm : arms) {
        writeIndent(out, indent + 1);
        out << "Arm ";
        writePattern(out, *arm.pattern);
        if (arm.guard) {
            out << " if ";
            writeExpr(out, arm.guard);
        }
        out << "\n";
        if (arm.body) {
            arm.body->print(out, indent + 2);
        }
    }
}

StructDeclStmt::StructDeclStmt(
    Token name,
    std::vector<TypeParameter> typeParameters,
    std::vector<StructFieldDecl> fields)
    : name(std::move(name))
    , typeParameters(std::move(typeParameters))
    , fields(std::move(fields))
{
}

void StructDeclStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Struct " << name.lexeme;
    writeTypeParameterList(out, typeParameters);
    out << " {";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        if (fields[i].isPrivate) {
            out << "private ";
        }
        out << fields[i].name.lexeme << ": ";
        writeTypeAnnotation(out, fields[i].typeName);
    }
    out << "}\n";
}

ImplStmt::ImplStmt(
    Token typeName,
    std::vector<TypeParameter> typeParameters,
    std::vector<MethodDecl> methods)
    : typeName(std::move(typeName))
    , typeParameters(std::move(typeParameters))
    , methods(std::move(methods))
{
}

void ImplStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Impl " << typeName.lexeme;
    writeTypeParameterList(out, typeParameters);
    out << '\n';
    for (const MethodDecl& method : methods) {
        writeIndent(out, indent + 1);
        out << (method.isOperator ? "Operator " : "Method ") << method.name.lexeme;
        writeTypeParameterList(out, method.typeParameters);
        writeParameterList(out, method.parameters);
        writeReturnAnnotation(out, method.returnTypeName);
        out << '\n';
        for (const auto& statement : method.body) {
            statement->print(out, indent + 2);
        }
    }
}

ImportStmt::ImportStmt(Token keyword, Token path, std::optional<Token> alias)
    : keyword(std::move(keyword))
    , path(std::move(path))
    , alias(std::move(alias))
{
}

void ImportStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Import " << path.lexeme;
    if (alias) {
        out << " as " << alias->lexeme;
    }
    out << "\n";
}

ExportStmt::ExportStmt(Token keyword, std::vector<Token> names, std::optional<Token> sourcePath)
    : keyword(std::move(keyword))
    , names(std::move(names))
    , sourcePath(std::move(sourcePath))
{
}

void ExportStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Export";
    for (const auto& name : names) {
        out << ' ' << name.lexeme;
    }
    if (sourcePath) {
        out << " from " << sourcePath->lexeme;
    }
    out << "\n";
}

ModuleStmt::ModuleStmt(
    std::size_t moduleId,
    std::string path,
    std::string source,
    std::vector<StmtPtr> statements,
    bool isEntry,
    SourceFileId sourceId)
    : moduleId(moduleId)
    , sourceId(sourceId)
    , path(std::move(path))
    , source(std::move(source))
    , statements(std::move(statements))
    , isEntry(isEntry)
{
}

void ModuleStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Module " << moduleId << (isEntry ? " entry" : "") << "\n";
    for (const auto& statement : statements) {
        statement->print(out, indent + 1);
    }
}

LetStmt::LetStmt(Token name, std::optional<TypeAnnotation> typeName, ExprPtr initializer)
    : name(std::move(name))
    , typeName(std::move(typeName))
    , initializer(std::move(initializer))
{
}

void LetStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Let " << name.lexeme;
    writeOptionalTypeAnnotation(out, typeName);
    out << " = ";
    writeExpr(out, initializer);
    out << '\n';
}

PrintStmt::PrintStmt(ExprPtr expression)
    : expression(std::move(expression))
{
}

void PrintStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Print ";
    writeExpr(out, expression);
    out << '\n';
}

ExpressionStmt::ExpressionStmt(ExprPtr expression)
    : expression(std::move(expression))
{
}

void ExpressionStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Expr ";
    writeExpr(out, expression);
    out << '\n';
}

BlockStmt::BlockStmt(std::vector<StmtPtr> statements)
    : statements(std::move(statements))
{
}

void BlockStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Block\n";
    for (const auto& statement : statements) {
        statement->print(out, indent + 1);
    }
}

IfStmt::IfStmt(ExprPtr condition, StmtPtr thenBranch, StmtPtr elseBranch)
    : condition(std::move(condition))
    , thenBranch(std::move(thenBranch))
    , elseBranch(std::move(elseBranch))
{
}

void IfStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "If ";
    writeExpr(out, condition);
    out << '\n';

    writeIndent(out, indent + 1);
    out << "Then\n";
    if (thenBranch) {
        thenBranch->print(out, indent + 2);
    }

    if (elseBranch) {
        writeIndent(out, indent + 1);
        out << "Else\n";
        elseBranch->print(out, indent + 2);
    }
}

WhileStmt::WhileStmt(ExprPtr condition, StmtPtr body)
    : condition(std::move(condition))
    , body(std::move(body))
{
}

void WhileStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "While ";
    writeExpr(out, condition);
    out << '\n';

    writeIndent(out, indent + 1);
    out << "Body\n";
    if (body) {
        body->print(out, indent + 2);
    }
}

ForStmt::ForStmt(Token keyword, StmtPtr initializer, ExprPtr condition, ExprPtr increment, StmtPtr body)
    : keyword(std::move(keyword))
    , initializer(std::move(initializer))
    , condition(std::move(condition))
    , increment(std::move(increment))
    , body(std::move(body))
{
}

void ForStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "For\n";

    writeIndent(out, indent + 1);
    out << "Initializer\n";
    if (initializer) {
        initializer->print(out, indent + 2);
    } else {
        writeIndent(out, indent + 2);
        out << "nil\n";
    }

    writeIndent(out, indent + 1);
    out << "Condition ";
    writeExpr(out, condition);
    out << "\n";

    writeIndent(out, indent + 1);
    out << "Increment ";
    writeExpr(out, increment);
    out << "\n";

    writeIndent(out, indent + 1);
    out << "Body\n";
    if (body) {
        body->print(out, indent + 2);
    }
}

ForInStmt::ForInStmt(Token keyword, Token variable, ExprPtr iterable, StmtPtr body)
    : keyword(std::move(keyword))
    , variable(std::move(variable))
    , iterable(std::move(iterable))
    , body(std::move(body))
{
}

void ForInStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "ForIn " << variable.lexeme << " in ";
    writeExpr(out, iterable);
    out << "\n";
    if (body) {
        body->print(out, indent + 1);
    }
}

BreakStmt::BreakStmt(Token keyword)
    : keyword(std::move(keyword))
{
}

void BreakStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Break\n";
}

ContinueStmt::ContinueStmt(Token keyword)
    : keyword(std::move(keyword))
{
}

void ContinueStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Continue\n";
}

FunctionStmt::FunctionStmt(Token name, std::vector<TypeParameter> typeParameters, std::vector<Parameter> parameters, std::optional<TypeAnnotation> returnTypeName, std::vector<StmtPtr> body)
    : name(std::move(name))
    , typeParameters(std::move(typeParameters))
    , parameters(std::move(parameters))
    , returnTypeName(std::move(returnTypeName))
    , body(std::move(body))
{
}

void FunctionStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Fun " << name.lexeme;
    writeTypeParameterList(out, typeParameters);
    writeParameterList(out, parameters);
    writeReturnAnnotation(out, returnTypeName);
    out << '\n';
    for (const auto& statement : body) {
        statement->print(out, indent + 1);
    }
}

ReturnStmt::ReturnStmt(Token keyword, ExprPtr value)
    : keyword(std::move(keyword))
    , value(std::move(value))
{
}

void ReturnStmt::print(std::ostream& out, int indent) const
{
    writeIndent(out, indent);
    out << "Return ";
    writeExpr(out, value);
    out << '\n';
}

void Program::print(std::ostream& out) const
{
    out << "Program\n";
    if (statements.size() == 1) {
        if (const auto* module = dynamic_cast<const ModuleStmt*>(statements.front().get())) {
            if (module->isEntry
                && moduleGraph
                && moduleGraph->nodes.size() == 1
                && moduleGraph->edges.empty()
                && moduleGraph->nodes.front().moduleId == module->moduleId) {
                for (const auto& statement : module->statements) {
                    statement->print(out, 1);
                }
                return;
            }
        }
    }
    for (const auto& statement : statements) {
        statement->print(out, 1);
    }
}

namespace {

std::optional<SourceRange> tokenRange(const Token& token)
{
    return token.range;
}

void mergeRange(std::optional<SourceRange>& target, const std::optional<SourceRange>& candidate)
{
    if (!candidate || !candidate->source.valid() || candidate->start > candidate->end) {
        return;
    }
    if (!target || !target->source.valid()) {
        target = candidate;
        return;
    }
    if (target->source != candidate->source) {
        // A syntax node is expected to be source-local.  Keep the first
        // source when malformed/recovered input happens to span a boundary;
        // the range remains valid and the diagnostic path is still owned by
        // the node's anchor token.
        return;
    }
    target->start = std::min(target->start, candidate->start);
    target->end = std::max(target->end, candidate->end);
}

std::optional<SourceRange> typeAnnotationRange(const TypeAnnotation& annotation)
{
    std::optional<SourceRange> result = tokenRange(annotation.token);
    mergeRange(result, tokenRange(annotation.qualifier));
    if (annotation.returnType) {
        mergeRange(result, typeAnnotationRange(*annotation.returnType));
    }
    if (annotation.elementType) {
        mergeRange(result, typeAnnotationRange(*annotation.elementType));
    }
    if (annotation.keyType) {
        mergeRange(result, typeAnnotationRange(*annotation.keyType));
    }
    if (annotation.valueType) {
        mergeRange(result, typeAnnotationRange(*annotation.valueType));
    }
    if (annotation.innerType) {
        mergeRange(result, typeAnnotationRange(*annotation.innerType));
    }
    for (const TypeAnnotation& argument : annotation.typeArguments) {
        mergeRange(result, typeAnnotationRange(argument));
    }
    return result;
}

void populatePattern(Pattern& pattern);
void populateStmt(Stmt& statement);

void populateExpr(Expr& expression)
{
    std::optional<SourceRange> result = expression.range;

    if (auto* variable = dynamic_cast<VariableExpr*>(&expression)) {
        mergeRange(result, tokenRange(variable->name));
    } else if (auto* assign = dynamic_cast<AssignExpr*>(&expression)) {
        mergeRange(result, tokenRange(assign->name));
        if (assign->value) {
            populateExpr(*assign->value);
            mergeRange(result, assign->value->range);
        }
    } else if (auto* compound = dynamic_cast<CompoundAssignExpr*>(&expression)) {
        mergeRange(result, tokenRange(compound->name));
        mergeRange(result, tokenRange(compound->op));
        if (compound->value) {
            populateExpr(*compound->value);
            mergeRange(result, compound->value->range);
        }
    } else if (auto* indexAssign = dynamic_cast<IndexAssignExpr*>(&expression)) {
        mergeRange(result, tokenRange(indexAssign->bracket));
        if (indexAssign->collection) {
            populateExpr(*indexAssign->collection);
            mergeRange(result, indexAssign->collection->range);
        }
        if (indexAssign->index) {
            populateExpr(*indexAssign->index);
            mergeRange(result, indexAssign->index->range);
        }
        if (indexAssign->value) {
            populateExpr(*indexAssign->value);
            mergeRange(result, indexAssign->value->range);
        }
    } else if (auto* indexCompound = dynamic_cast<IndexCompoundAssignExpr*>(&expression)) {
        mergeRange(result, tokenRange(indexCompound->bracket));
        mergeRange(result, tokenRange(indexCompound->op));
        if (indexCompound->collection) {
            populateExpr(*indexCompound->collection);
            mergeRange(result, indexCompound->collection->range);
        }
        if (indexCompound->index) {
            populateExpr(*indexCompound->index);
            mergeRange(result, indexCompound->index->range);
        }
        if (indexCompound->value) {
            populateExpr(*indexCompound->value);
            mergeRange(result, indexCompound->value->range);
        }
    } else if (auto* unary = dynamic_cast<UnaryExpr*>(&expression)) {
        mergeRange(result, tokenRange(unary->op));
        if (unary->right) {
            populateExpr(*unary->right);
            mergeRange(result, unary->right->range);
        }
    } else if (auto* binary = dynamic_cast<BinaryExpr*>(&expression)) {
        mergeRange(result, tokenRange(binary->op));
        if (binary->left) {
            populateExpr(*binary->left);
            mergeRange(result, binary->left->range);
        }
        if (binary->right) {
            populateExpr(*binary->right);
            mergeRange(result, binary->right->range);
        }
    } else if (auto* logical = dynamic_cast<LogicalExpr*>(&expression)) {
        mergeRange(result, tokenRange(logical->op));
        if (logical->left) {
            populateExpr(*logical->left);
            mergeRange(result, logical->left->range);
        }
        if (logical->right) {
            populateExpr(*logical->right);
            mergeRange(result, logical->right->range);
        }
    } else if (auto* grouping = dynamic_cast<GroupingExpr*>(&expression)) {
        if (grouping->expression) {
            populateExpr(*grouping->expression);
            mergeRange(result, grouping->expression->range);
        }
    } else if (auto* call = dynamic_cast<CallExpr*>(&expression)) {
        mergeRange(result, tokenRange(call->paren));
        if (call->callee) {
            populateExpr(*call->callee);
            mergeRange(result, call->callee->range);
        }
        for (const ExprPtr& argument : call->arguments) {
            if (argument) {
                populateExpr(*argument);
                mergeRange(result, argument->range);
            }
        }
    } else if (auto* memberCall = dynamic_cast<MemberCallExpr*>(&expression)) {
        mergeRange(result, tokenRange(memberCall->name));
        mergeRange(result, tokenRange(memberCall->paren));
        if (memberCall->receiver) {
            populateExpr(*memberCall->receiver);
            mergeRange(result, memberCall->receiver->range);
        }
        for (const ExprPtr& argument : memberCall->arguments) {
            if (argument) {
                populateExpr(*argument);
                mergeRange(result, argument->range);
            }
        }
    } else if (auto* array = dynamic_cast<ArrayExpr*>(&expression)) {
        mergeRange(result, tokenRange(array->bracket));
        for (const ExprPtr& element : array->elements) {
            if (element) {
                populateExpr(*element);
                mergeRange(result, element->range);
            }
        }
    } else if (auto* map = dynamic_cast<MapExpr*>(&expression)) {
        mergeRange(result, tokenRange(map->brace));
        for (const MapEntry& entry : map->entries) {
            mergeRange(result, tokenRange(entry.colon));
            if (entry.key) {
                populateExpr(*entry.key);
                mergeRange(result, entry.key->range);
            }
            if (entry.value) {
                populateExpr(*entry.value);
                mergeRange(result, entry.value->range);
            }
        }
    } else if (auto* construct = dynamic_cast<StructConstructExpr*>(&expression)) {
        if (construct->qualifier) {
            mergeRange(result, tokenRange(*construct->qualifier));
        }
        mergeRange(result, tokenRange(construct->name));
        for (const StructField& field : construct->fields) {
            mergeRange(result, tokenRange(field.name));
            if (field.value) {
                populateExpr(*field.value);
                mergeRange(result, field.value->range);
            }
        }
    } else if (auto* index = dynamic_cast<IndexExpr*>(&expression)) {
        mergeRange(result, tokenRange(index->bracket));
        if (index->collection) {
            populateExpr(*index->collection);
            mergeRange(result, index->collection->range);
        }
        if (index->index) {
            populateExpr(*index->index);
            mergeRange(result, index->index->range);
        }
    } else if (auto* field = dynamic_cast<FieldAccessExpr*>(&expression)) {
        mergeRange(result, tokenRange(field->name));
        if (field->object) {
            populateExpr(*field->object);
            mergeRange(result, field->object->range);
        }
    } else if (auto* fieldAssign = dynamic_cast<FieldAssignExpr*>(&expression)) {
        mergeRange(result, tokenRange(fieldAssign->name));
        if (fieldAssign->object) {
            populateExpr(*fieldAssign->object);
            mergeRange(result, fieldAssign->object->range);
        }
        if (fieldAssign->value) {
            populateExpr(*fieldAssign->value);
            mergeRange(result, fieldAssign->value->range);
        }
    } else if (auto* fieldCompound = dynamic_cast<FieldCompoundAssignExpr*>(&expression)) {
        mergeRange(result, tokenRange(fieldCompound->name));
        mergeRange(result, tokenRange(fieldCompound->op));
        if (fieldCompound->object) {
            populateExpr(*fieldCompound->object);
            mergeRange(result, fieldCompound->object->range);
        }
        if (fieldCompound->value) {
            populateExpr(*fieldCompound->value);
            mergeRange(result, fieldCompound->value->range);
        }
    } else if (auto* function = dynamic_cast<FunctionExpr*>(&expression)) {
        mergeRange(result, tokenRange(function->keyword));
        for (const TypeParameter& parameter : function->typeParameters) {
            mergeRange(result, tokenRange(parameter.name));
            for (const TypeAnnotation& constraint : parameter.constraints) {
                mergeRange(result, typeAnnotationRange(constraint));
            }
        }
        for (const Parameter& parameter : function->parameters) {
            mergeRange(result, tokenRange(parameter.name));
            if (parameter.typeName) {
                mergeRange(result, typeAnnotationRange(*parameter.typeName));
            }
        }
        if (function->returnTypeName) {
            mergeRange(result, typeAnnotationRange(*function->returnTypeName));
        }
        for (const StmtPtr& statement : function->body) {
            if (statement) {
                populateStmt(*statement);
                mergeRange(result, statement->range);
            }
        }
    }

    expression.range = result;
}

void populatePattern(Pattern& pattern)
{
    std::optional<SourceRange> result = pattern.range;
    if (auto* wildcard = dynamic_cast<WildcardPattern*>(&pattern)) {
        mergeRange(result, tokenRange(wildcard->name));
    } else if (auto* variable = dynamic_cast<VariablePattern*>(&pattern)) {
        mergeRange(result, tokenRange(variable->name));
    } else if (auto* literal = dynamic_cast<LiteralPattern*>(&pattern)) {
        mergeRange(result, tokenRange(literal->value));
    } else if (auto* orPattern = dynamic_cast<OrPattern*>(&pattern)) {
        mergeRange(result, tokenRange(orPattern->pipe));
        for (const PatternPtr& alternative : orPattern->alternatives) {
            if (alternative) {
                populatePattern(*alternative);
                mergeRange(result, alternative->range);
            }
        }
    } else if (auto* record = dynamic_cast<RecordPattern*>(&pattern)) {
        if (record->qualifier) {
            mergeRange(result, tokenRange(*record->qualifier));
        }
        mergeRange(result, tokenRange(record->name));
        for (const RecordPatternField& field : record->fields) {
            mergeRange(result, tokenRange(field.name));
            if (field.pattern) {
                populatePattern(*field.pattern);
                mergeRange(result, field.pattern->range);
            }
        }
    } else if (auto* variant = dynamic_cast<VariantPattern*>(&pattern)) {
        if (variant->qualifier) {
            mergeRange(result, tokenRange(*variant->qualifier));
        }
        mergeRange(result, tokenRange(variant->name));
        for (const std::optional<Token>& argumentName : variant->argumentNames) {
            if (argumentName) {
                mergeRange(result, tokenRange(*argumentName));
            }
        }
        for (const PatternPtr& argument : variant->arguments) {
            if (argument) {
                populatePattern(*argument);
                mergeRange(result, argument->range);
            }
        }
    }
    pattern.range = result;
}

void populateMethod(MethodDecl& method)
{
    std::optional<SourceRange> result = method.range;
    mergeRange(result, tokenRange(method.name));
    for (const TypeParameter& parameter : method.typeParameters) {
        mergeRange(result, tokenRange(parameter.name));
        for (const TypeAnnotation& constraint : parameter.constraints) {
            mergeRange(result, typeAnnotationRange(constraint));
        }
    }
    for (const Parameter& parameter : method.parameters) {
        mergeRange(result, tokenRange(parameter.name));
        if (parameter.typeName) {
            mergeRange(result, typeAnnotationRange(*parameter.typeName));
        }
    }
    if (method.returnTypeName) {
        mergeRange(result, typeAnnotationRange(*method.returnTypeName));
    }
    for (const StmtPtr& statement : method.body) {
        if (statement) {
            populateStmt(*statement);
            mergeRange(result, statement->range);
        }
    }
    method.range = result;
}

void populateStmt(Stmt& statement)
{
    std::optional<SourceRange> result = statement.range;
    if (auto* enumDecl = dynamic_cast<EnumDeclStmt*>(&statement)) {
        mergeRange(result, tokenRange(enumDecl->name));
        for (const TypeParameter& parameter : enumDecl->typeParameters) {
            mergeRange(result, tokenRange(parameter.name));
            for (const TypeAnnotation& constraint : parameter.constraints) {
                mergeRange(result, typeAnnotationRange(constraint));
            }
        }
        for (const EnumVariantDecl& variant : enumDecl->variants) {
            mergeRange(result, tokenRange(variant.name));
            for (const std::optional<Token>& name : variant.payloadNames) {
                if (name) {
                    mergeRange(result, tokenRange(*name));
                }
            }
            for (const TypeAnnotation& type : variant.payloadTypes) {
                mergeRange(result, typeAnnotationRange(type));
            }
        }
    } else if (auto* match = dynamic_cast<MatchStmt*>(&statement)) {
        if (match->value) {
            populateExpr(*match->value);
            mergeRange(result, match->value->range);
        }
        for (const MatchArm& arm : match->arms) {
            if (arm.pattern) {
                populatePattern(*arm.pattern);
                mergeRange(result, arm.pattern->range);
            }
            if (arm.guard) {
                populateExpr(*arm.guard);
                mergeRange(result, arm.guard->range);
            }
            if (arm.body) {
                populateStmt(*arm.body);
                mergeRange(result, arm.body->range);
            }
        }
    } else if (auto* structDecl = dynamic_cast<StructDeclStmt*>(&statement)) {
        mergeRange(result, tokenRange(structDecl->name));
        for (const TypeParameter& parameter : structDecl->typeParameters) {
            mergeRange(result, tokenRange(parameter.name));
            for (const TypeAnnotation& constraint : parameter.constraints) {
                mergeRange(result, typeAnnotationRange(constraint));
            }
        }
        for (const StructFieldDecl& field : structDecl->fields) {
            mergeRange(result, tokenRange(field.name));
            mergeRange(result, typeAnnotationRange(field.typeName));
        }
    } else if (auto* impl = dynamic_cast<ImplStmt*>(&statement)) {
        mergeRange(result, tokenRange(impl->typeName));
        for (const TypeParameter& parameter : impl->typeParameters) {
            mergeRange(result, tokenRange(parameter.name));
            for (const TypeAnnotation& constraint : parameter.constraints) {
                mergeRange(result, typeAnnotationRange(constraint));
            }
        }
        for (MethodDecl& method : impl->methods) {
            populateMethod(method);
            mergeRange(result, method.range);
        }
    } else if (auto* import = dynamic_cast<ImportStmt*>(&statement)) {
        mergeRange(result, tokenRange(import->keyword));
        mergeRange(result, tokenRange(import->path));
        if (import->alias) {
            mergeRange(result, tokenRange(*import->alias));
        }
    } else if (auto* exportStmt = dynamic_cast<ExportStmt*>(&statement)) {
        mergeRange(result, tokenRange(exportStmt->keyword));
        for (const Token& name : exportStmt->names) {
            mergeRange(result, tokenRange(name));
        }
        if (exportStmt->sourcePath) {
            mergeRange(result, tokenRange(*exportStmt->sourcePath));
        }
    } else if (auto* module = dynamic_cast<ModuleStmt*>(&statement)) {
        if (module->sourceId.valid()) {
            mergeRange(result, SourceRange{module->sourceId, 0, module->source.size()});
        }
        for (const StmtPtr& child : module->statements) {
            if (child) {
                populateStmt(*child);
                mergeRange(result, child->range);
            }
        }
    } else if (auto* let = dynamic_cast<LetStmt*>(&statement)) {
        mergeRange(result, tokenRange(let->name));
        if (let->typeName) {
            mergeRange(result, typeAnnotationRange(*let->typeName));
        }
        if (let->initializer) {
            populateExpr(*let->initializer);
            mergeRange(result, let->initializer->range);
        }
    } else if (auto* print = dynamic_cast<PrintStmt*>(&statement)) {
        if (print->expression) {
            populateExpr(*print->expression);
            mergeRange(result, print->expression->range);
        }
    } else if (auto* expression = dynamic_cast<ExpressionStmt*>(&statement)) {
        if (expression->expression) {
            populateExpr(*expression->expression);
            mergeRange(result, expression->expression->range);
        }
    } else if (auto* block = dynamic_cast<BlockStmt*>(&statement)) {
        for (const StmtPtr& child : block->statements) {
            if (child) {
                populateStmt(*child);
                mergeRange(result, child->range);
            }
        }
    } else if (auto* ifStmt = dynamic_cast<IfStmt*>(&statement)) {
        if (ifStmt->condition) {
            populateExpr(*ifStmt->condition);
            mergeRange(result, ifStmt->condition->range);
        }
        if (ifStmt->thenBranch) {
            populateStmt(*ifStmt->thenBranch);
            mergeRange(result, ifStmt->thenBranch->range);
        }
        if (ifStmt->elseBranch) {
            populateStmt(*ifStmt->elseBranch);
            mergeRange(result, ifStmt->elseBranch->range);
        }
    } else if (auto* whileStmt = dynamic_cast<WhileStmt*>(&statement)) {
        if (whileStmt->condition) {
            populateExpr(*whileStmt->condition);
            mergeRange(result, whileStmt->condition->range);
        }
        if (whileStmt->body) {
            populateStmt(*whileStmt->body);
            mergeRange(result, whileStmt->body->range);
        }
    } else if (auto* forStmt = dynamic_cast<ForStmt*>(&statement)) {
        if (forStmt->initializer) {
            populateStmt(*forStmt->initializer);
            mergeRange(result, forStmt->initializer->range);
        }
        if (forStmt->condition) {
            populateExpr(*forStmt->condition);
            mergeRange(result, forStmt->condition->range);
        }
        if (forStmt->increment) {
            populateExpr(*forStmt->increment);
            mergeRange(result, forStmt->increment->range);
        }
        if (forStmt->body) {
            populateStmt(*forStmt->body);
            mergeRange(result, forStmt->body->range);
        }
        mergeRange(result, tokenRange(forStmt->keyword));
    } else if (auto* forIn = dynamic_cast<ForInStmt*>(&statement)) {
        mergeRange(result, tokenRange(forIn->keyword));
        mergeRange(result, tokenRange(forIn->variable));
        if (forIn->iterable) {
            populateExpr(*forIn->iterable);
            mergeRange(result, forIn->iterable->range);
        }
        if (forIn->body) {
            populateStmt(*forIn->body);
            mergeRange(result, forIn->body->range);
        }
    } else if (auto* breakStmt = dynamic_cast<BreakStmt*>(&statement)) {
        mergeRange(result, tokenRange(breakStmt->keyword));
    } else if (auto* continueStmt = dynamic_cast<ContinueStmt*>(&statement)) {
        mergeRange(result, tokenRange(continueStmt->keyword));
    } else if (auto* function = dynamic_cast<FunctionStmt*>(&statement)) {
        mergeRange(result, tokenRange(function->name));
        for (const TypeParameter& parameter : function->typeParameters) {
            mergeRange(result, tokenRange(parameter.name));
            for (const TypeAnnotation& constraint : parameter.constraints) {
                mergeRange(result, typeAnnotationRange(constraint));
            }
        }
        for (const Parameter& parameter : function->parameters) {
            mergeRange(result, tokenRange(parameter.name));
            if (parameter.typeName) {
                mergeRange(result, typeAnnotationRange(*parameter.typeName));
            }
        }
        if (function->returnTypeName) {
            mergeRange(result, typeAnnotationRange(*function->returnTypeName));
        }
        for (const StmtPtr& child : function->body) {
            if (child) {
                populateStmt(*child);
                mergeRange(result, child->range);
            }
        }
    } else if (auto* returnStmt = dynamic_cast<ReturnStmt*>(&statement)) {
        mergeRange(result, tokenRange(returnStmt->keyword));
        if (returnStmt->value) {
            populateExpr(*returnStmt->value);
            mergeRange(result, returnStmt->value->range);
        }
    }
    statement.range = result;
}

void assignPatternIds(Pattern& pattern, std::size_t& next)
{
    pattern.syntaxNodeId = SyntaxNodeId{next++};
    if (auto* orPattern = dynamic_cast<OrPattern*>(&pattern)) {
        for (const PatternPtr& alternative : orPattern->alternatives) {
            if (alternative) {
                assignPatternIds(*alternative, next);
            }
        }
    } else if (auto* record = dynamic_cast<RecordPattern*>(&pattern)) {
        for (const RecordPatternField& field : record->fields) {
            if (field.pattern) {
                assignPatternIds(*field.pattern, next);
            }
        }
    } else if (auto* variant = dynamic_cast<VariantPattern*>(&pattern)) {
        for (const PatternPtr& argument : variant->arguments) {
            if (argument) {
                assignPatternIds(*argument, next);
            }
        }
    }
}

void assignStmtIds(Stmt& statement, std::size_t& next);

void assignExprIds(Expr& expression, std::size_t& next)
{
    expression.syntaxNodeId = SyntaxNodeId{next++};
    auto assign = [&](ExprPtr& child) {
        if (child) {
            assignExprIds(*child, next);
        }
    };
    if (auto* assignExpr = dynamic_cast<AssignExpr*>(&expression)) {
        assign(assignExpr->value);
    } else if (auto* compound = dynamic_cast<CompoundAssignExpr*>(&expression)) {
        assign(compound->value);
    } else if (auto* indexAssign = dynamic_cast<IndexAssignExpr*>(&expression)) {
        assign(indexAssign->collection);
        assign(indexAssign->index);
        assign(indexAssign->value);
    } else if (auto* indexCompound = dynamic_cast<IndexCompoundAssignExpr*>(&expression)) {
        assign(indexCompound->collection);
        assign(indexCompound->index);
        assign(indexCompound->value);
    } else if (auto* unary = dynamic_cast<UnaryExpr*>(&expression)) {
        assign(unary->right);
    } else if (auto* binary = dynamic_cast<BinaryExpr*>(&expression)) {
        assign(binary->left);
        assign(binary->right);
    } else if (auto* logical = dynamic_cast<LogicalExpr*>(&expression)) {
        assign(logical->left);
        assign(logical->right);
    } else if (auto* grouping = dynamic_cast<GroupingExpr*>(&expression)) {
        assign(grouping->expression);
    } else if (auto* call = dynamic_cast<CallExpr*>(&expression)) {
        assign(call->callee);
        for (ExprPtr& argument : call->arguments) {
            assign(argument);
        }
    } else if (auto* memberCall = dynamic_cast<MemberCallExpr*>(&expression)) {
        assign(memberCall->receiver);
        for (ExprPtr& argument : memberCall->arguments) {
            assign(argument);
        }
    } else if (auto* array = dynamic_cast<ArrayExpr*>(&expression)) {
        for (ExprPtr& element : array->elements) {
            assign(element);
        }
    } else if (auto* map = dynamic_cast<MapExpr*>(&expression)) {
        for (MapEntry& entry : map->entries) {
            assign(entry.key);
            assign(entry.value);
        }
    } else if (auto* construct = dynamic_cast<StructConstructExpr*>(&expression)) {
        for (StructField& field : construct->fields) {
            assign(field.value);
        }
    } else if (auto* index = dynamic_cast<IndexExpr*>(&expression)) {
        assign(index->collection);
        assign(index->index);
    } else if (auto* field = dynamic_cast<FieldAccessExpr*>(&expression)) {
        assign(field->object);
    } else if (auto* fieldAssign = dynamic_cast<FieldAssignExpr*>(&expression)) {
        assign(fieldAssign->object);
        assign(fieldAssign->value);
    } else if (auto* fieldCompound = dynamic_cast<FieldCompoundAssignExpr*>(&expression)) {
        assign(fieldCompound->object);
        assign(fieldCompound->value);
    } else if (auto* function = dynamic_cast<FunctionExpr*>(&expression)) {
        for (StmtPtr& statement : function->body) {
            if (statement) {
                assignStmtIds(*statement, next);
            }
        }
    }
}

void assignMethodIds(MethodDecl& method, std::size_t& next)
{
    method.syntaxNodeId = SyntaxNodeId{next++};
    for (StmtPtr& statement : method.body) {
        if (statement) {
            assignStmtIds(*statement, next);
        }
    }
}

void assignStmtIds(Stmt& statement, std::size_t& next)
{
    statement.syntaxNodeId = SyntaxNodeId{next++};
    if (auto* module = dynamic_cast<ModuleStmt*>(&statement)) {
        for (StmtPtr& child : module->statements) {
            if (child) {
                assignStmtIds(*child, next);
            }
        }
    } else if (auto* match = dynamic_cast<MatchStmt*>(&statement)) {
        if (match->value) {
            assignExprIds(*match->value, next);
        }
        for (MatchArm& arm : match->arms) {
            if (arm.pattern) {
                assignPatternIds(*arm.pattern, next);
            }
            if (arm.guard) {
                assignExprIds(*arm.guard, next);
            }
            if (arm.body) {
                assignStmtIds(*arm.body, next);
            }
        }
    } else if (auto* impl = dynamic_cast<ImplStmt*>(&statement)) {
        for (MethodDecl& method : impl->methods) {
            assignMethodIds(method, next);
        }
    } else if (auto* let = dynamic_cast<LetStmt*>(&statement)) {
        if (let->initializer) {
            assignExprIds(*let->initializer, next);
        }
    } else if (auto* print = dynamic_cast<PrintStmt*>(&statement)) {
        if (print->expression) {
            assignExprIds(*print->expression, next);
        }
    } else if (auto* expression = dynamic_cast<ExpressionStmt*>(&statement)) {
        if (expression->expression) {
            assignExprIds(*expression->expression, next);
        }
    } else if (auto* block = dynamic_cast<BlockStmt*>(&statement)) {
        for (StmtPtr& child : block->statements) {
            if (child) {
                assignStmtIds(*child, next);
            }
        }
    } else if (auto* ifStmt = dynamic_cast<IfStmt*>(&statement)) {
        if (ifStmt->condition) {
            assignExprIds(*ifStmt->condition, next);
        }
        if (ifStmt->thenBranch) {
            assignStmtIds(*ifStmt->thenBranch, next);
        }
        if (ifStmt->elseBranch) {
            assignStmtIds(*ifStmt->elseBranch, next);
        }
    } else if (auto* whileStmt = dynamic_cast<WhileStmt*>(&statement)) {
        if (whileStmt->condition) {
            assignExprIds(*whileStmt->condition, next);
        }
        if (whileStmt->body) {
            assignStmtIds(*whileStmt->body, next);
        }
    } else if (auto* forStmt = dynamic_cast<ForStmt*>(&statement)) {
        if (forStmt->initializer) {
            assignStmtIds(*forStmt->initializer, next);
        }
        if (forStmt->condition) {
            assignExprIds(*forStmt->condition, next);
        }
        if (forStmt->increment) {
            assignExprIds(*forStmt->increment, next);
        }
        if (forStmt->body) {
            assignStmtIds(*forStmt->body, next);
        }
    } else if (auto* forIn = dynamic_cast<ForInStmt*>(&statement)) {
        if (forIn->iterable) {
            assignExprIds(*forIn->iterable, next);
        }
        if (forIn->body) {
            assignStmtIds(*forIn->body, next);
        }
    } else if (auto* function = dynamic_cast<FunctionStmt*>(&statement)) {
        for (StmtPtr& child : function->body) {
            if (child) {
                assignStmtIds(*child, next);
            }
        }
    } else if (auto* returnStmt = dynamic_cast<ReturnStmt*>(&statement)) {
        if (returnStmt->value) {
            assignExprIds(*returnStmt->value, next);
        }
    }
}

} // namespace

void populateSyntaxRanges(Program& program)
{
    for (const StmtPtr& statement : program.statements) {
        if (statement) {
            populateStmt(*statement);
        }
    }
}

void assignSyntaxNodeIds(Program& program)
{
    std::size_t next = 0;
    for (StmtPtr& statement : program.statements) {
        if (statement) {
            assignStmtIds(*statement, next);
        }
    }
}

void finalizeSyntaxMetadata(Program& program)
{
    populateSyntaxRanges(program);
    assignSyntaxNodeIds(program);
}
