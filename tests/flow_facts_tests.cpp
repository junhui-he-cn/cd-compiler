#include "Ast.hpp"
#include "FlowFacts.hpp"
#include "Token.hpp"
#include "TypeUtils.hpp"

#include <cassert>
#include <optional>
#include <string>
#include <utility>

namespace {

Token token(TokenType type, std::string lexeme)
{
    return Token{type, std::move(lexeme), 1, 1};
}

ExprPtr variable(std::string name)
{
    return std::make_unique<VariableExpr>(token(TokenType::Identifier, std::move(name)));
}

ExprPtr nilLiteral()
{
    return std::make_unique<LiteralExpr>("nil");
}

ExprPtr nilCheck(std::string name, TokenType op)
{
    return std::make_unique<BinaryExpr>(
        variable(std::move(name)),
        token(op, op == TokenType::BangEqual ? "!=" : "=="),
        nilLiteral());
}

ExprPtr grouped(ExprPtr expression)
{
    return std::make_unique<GroupingExpr>(std::move(expression));
}

ExprPtr logical(ExprPtr left, TokenType op, ExprPtr right)
{
    return std::make_unique<LogicalExpr>(
        std::move(left),
        token(op, op == TokenType::AmpersandAmpersand ? "&&" : "||"),
        std::move(right));
}

FlowFacts::VariableNarrowingResolver resolver()
{
    return [](const VariableExpr& expression) -> std::optional<FlowNarrowing> {
        if (expression.name.lexeme == "numberValue") {
            return FlowNarrowing{"numberValue#0", simpleType(StaticType::Number)};
        }
        if (expression.name.lexeme == "stringValue") {
            return FlowNarrowing{"stringValue#1", simpleType(StaticType::String)};
        }
        return std::nullopt;
    };
}

void test_not_nil_narrows_then_branch()
{
    FlowFacts facts;
    const ExprPtr condition = nilCheck("numberValue", TokenType::BangEqual);

    const BranchFlowFacts branchFacts = facts.factsForIfCondition(*condition, resolver());

    assert(branchFacts.thenNarrowings.size() == 1);
    assert(branchFacts.thenNarrowings[0].resolvedName == "numberValue#0");
    assert(branchFacts.thenNarrowings[0].type.kind == StaticType::Number);
    assert(branchFacts.elseNarrowings.empty());
}

void test_equal_nil_narrows_else_branch_through_grouping()
{
    FlowFacts facts;
    const ExprPtr condition = grouped(nilCheck("stringValue", TokenType::EqualEqual));

    const BranchFlowFacts branchFacts = facts.factsForIfCondition(*condition, resolver());

    assert(branchFacts.thenNarrowings.empty());
    assert(branchFacts.elseNarrowings.size() == 1);
    assert(branchFacts.elseNarrowings[0].resolvedName == "stringValue#1");
    assert(branchFacts.elseNarrowings[0].type.kind == StaticType::String);
}

void test_logical_and_combines_then_facts()
{
    FlowFacts facts;
    const ExprPtr condition = logical(
        nilCheck("numberValue", TokenType::BangEqual),
        TokenType::AmpersandAmpersand,
        nilCheck("stringValue", TokenType::BangEqual));

    const BranchFlowFacts branchFacts = facts.factsForIfCondition(*condition, resolver());

    assert(branchFacts.thenNarrowings.size() == 2);
    assert(branchFacts.thenNarrowings[0].resolvedName == "numberValue#0");
    assert(branchFacts.thenNarrowings[1].resolvedName == "stringValue#1");
    assert(branchFacts.elseNarrowings.empty());
}

void test_logical_or_combines_else_facts()
{
    FlowFacts facts;
    const ExprPtr condition = logical(
        nilCheck("numberValue", TokenType::EqualEqual),
        TokenType::PipePipe,
        nilCheck("stringValue", TokenType::EqualEqual));

    const BranchFlowFacts branchFacts = facts.factsForIfCondition(*condition, resolver());

    assert(branchFacts.thenNarrowings.empty());
    assert(branchFacts.elseNarrowings.size() == 2);
    assert(branchFacts.elseNarrowings[0].resolvedName == "numberValue#0");
    assert(branchFacts.elseNarrowings[1].resolvedName == "stringValue#1");
}

void test_non_narrowable_variable_produces_no_facts()
{
    FlowFacts facts;
    const ExprPtr condition = nilCheck("dynamicValue", TokenType::BangEqual);

    const BranchFlowFacts branchFacts = facts.factsForIfCondition(*condition, resolver());

    assert(branchFacts.thenNarrowings.empty());
    assert(branchFacts.elseNarrowings.empty());
}

void test_with_narrowings_restores_stack_after_success_and_throw()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> outer{{"value#0", simpleType(StaticType::Number)}};
    const std::vector<FlowNarrowing> inner{{"value#0", simpleType(StaticType::String)}};

    facts.withNarrowings(outer, [&]() {
        const std::optional<TypeInfo> narrowedOuter = facts.narrowedTypeFor("value#0");
        assert(narrowedOuter.has_value());
        assert(narrowedOuter->kind == StaticType::Number);

        facts.withNarrowings(inner, [&]() {
            const std::optional<TypeInfo> narrowedInner = facts.narrowedTypeFor("value#0");
            assert(narrowedInner.has_value());
            assert(narrowedInner->kind == StaticType::String);
        });

        const std::optional<TypeInfo> restoredOuter = facts.narrowedTypeFor("value#0");
        assert(restoredOuter.has_value());
        assert(restoredOuter->kind == StaticType::Number);
    });

    assert(!facts.narrowedTypeFor("value#0").has_value());

    bool threw = false;
    try {
        facts.withNarrowings(outer, []() {
            throw 7;
        });
    } catch (int value) {
        threw = value == 7;
    }

    assert(threw);
    assert(!facts.narrowedTypeFor("value#0").has_value());
}

void test_invalidation_propagates_and_nested_facts_restore()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> outer{{"value#0", simpleType(StaticType::Number)}};
    const std::vector<FlowNarrowing> inner{{"other#1", simpleType(StaticType::String)}};

    facts.withNarrowings(outer, [&]() {
        facts.withNarrowings(inner, [&]() {
            facts.invalidate("value#0");
            assert(!facts.narrowedTypeFor("value#0").has_value());
        });

        assert(!facts.narrowedTypeFor("value#0").has_value());

        const std::optional<TypeInfo> restoredOuter = facts.narrowedTypeFor("other#1");
        assert(!restoredOuter.has_value());
    });

    assert(!facts.narrowedTypeFor("value#0").has_value());
}

} // namespace

int main()
{
    test_not_nil_narrows_then_branch();
    test_equal_nil_narrows_else_branch_through_grouping();
    test_logical_and_combines_then_facts();
    test_logical_or_combines_else_facts();
    test_non_narrowable_variable_produces_no_facts();
    test_with_narrowings_restores_stack_after_success_and_throw();
    test_invalidation_propagates_and_nested_facts_restore();
}
