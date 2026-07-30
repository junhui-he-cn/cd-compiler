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

ExprPtr truthy(std::string name)
{
    return variable(std::move(name));
}

ExprPtr field(ExprPtr object, std::string name)
{
    return std::make_unique<FieldAccessExpr>(
        std::move(object),
        token(TokenType::Identifier, std::move(name)));
}

ExprPtr fieldNilCheck(std::string object, std::string name, TokenType op)
{
    return std::make_unique<BinaryExpr>(
        field(variable(std::move(object)), std::move(name)),
        token(op, op == TokenType::BangEqual ? "!=" : "=="),
        nilLiteral());
}

ExprPtr index(ExprPtr collection, std::string indexValue)
{
    return std::make_unique<IndexExpr>(
        std::move(collection),
        token(TokenType::LeftBracket, "["),
        std::make_unique<LiteralExpr>(std::move(indexValue)));
}

ExprPtr indexNilCheck(std::string collection, std::string indexValue, TokenType op)
{
    return std::make_unique<BinaryExpr>(
        index(variable(std::move(collection)), std::move(indexValue)),
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

FlowFacts::TargetNarrowingResolver targetResolver()
{
    return [](const Expr& expression) -> std::optional<FlowNarrowing> {
        if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
            if (variable->name.lexeme == "numberValue") {
                return FlowNarrowing{"numberValue#0", simpleType(StaticType::Number)};
            }
        }
        if (const auto* fieldAccess = dynamic_cast<const FieldAccessExpr*>(&expression)) {
            if (fieldAccess->name.lexeme == "value") {
                return FlowNarrowing{"box#2.value", simpleType(StaticType::Number)};
            }
        }
        if (const auto* indexAccess = dynamic_cast<const IndexExpr*>(&expression)) {
            const auto* literal = dynamic_cast<const LiteralExpr*>(indexAccess->index.get());
            if (literal && literal->value == "0") {
                return FlowNarrowing{"values#3[0]", simpleType(StaticType::Number)};
            }
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

void test_truthiness_narrows_then_branch_only()
{
    FlowFacts facts;
    const ExprPtr condition = truthy("numberValue");

    const BranchFlowFacts branchFacts = facts.factsForIfConditionTargets(*condition, targetResolver());

    assert(branchFacts.thenNarrowings.size() == 1);
    assert(branchFacts.thenNarrowings.front().resolvedName == "numberValue#0");
    assert(branchFacts.thenNarrowings.front().type.kind == StaticType::Number);
    assert(branchFacts.elseNarrowings.empty());
}

void test_target_resolver_narrows_direct_field_targets()
{
    FlowFacts facts;
    const ExprPtr condition = fieldNilCheck("box", "value", TokenType::BangEqual);

    const BranchFlowFacts branchFacts = facts.factsForIfConditionTargets(*condition, targetResolver());

    assert(branchFacts.thenNarrowings.size() == 1);
    assert(branchFacts.thenNarrowings.front().resolvedName == "box#2.value");
    assert(branchFacts.thenNarrowings.front().type.kind == StaticType::Number);
    assert(branchFacts.elseNarrowings.empty());
}

void test_target_resolver_narrows_direct_index_targets()
{
    FlowFacts facts;
    const ExprPtr condition = indexNilCheck("values", "0", TokenType::BangEqual);

    const BranchFlowFacts branchFacts = facts.factsForIfConditionTargets(*condition, targetResolver());

    assert(branchFacts.thenNarrowings.size() == 1);
    assert(branchFacts.thenNarrowings.front().resolvedName == "values#3[0]");
    assert(branchFacts.thenNarrowings.front().type.kind == StaticType::Number);
    assert(branchFacts.elseNarrowings.empty());
}

void test_active_narrowings_can_be_appended_after_branch_analysis()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> outer{{"value#0", simpleType(StaticType::Number)}};
    const std::vector<FlowNarrowing> appended{{"other#1", simpleType(StaticType::String)}};

    facts.withNarrowings(outer, [&]() {
        const std::vector<FlowNarrowing> active = facts.activeNarrowings();
        assert(active.size() == 1);
        assert(active.front().resolvedName == "value#0");

        facts.appendNarrowings(appended);
        const std::optional<TypeInfo> outerNarrowing = facts.narrowedTypeFor("value#0");
        assert(outerNarrowing.has_value());
        assert(outerNarrowing->kind == StaticType::Number);
        const std::optional<TypeInfo> narrowed = facts.narrowedTypeFor("other#1");
        assert(narrowed.has_value());
        assert(narrowed->kind == StaticType::String);
    });
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

void test_invalidate_all_clears_nested_facts()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> outer{
        {"numberValue#0", simpleType(StaticType::Number)},
        {"stringValue#1", simpleType(StaticType::String)}};
    const std::vector<FlowNarrowing> inner{
        {"otherValue#2", simpleType(StaticType::Bool)}};

    facts.withNarrowings(outer, [&]() {
        facts.withNarrowings(inner, [&]() {
            facts.invalidateAll();
            assert(!facts.narrowedTypeFor("numberValue#0").has_value());
            assert(!facts.narrowedTypeFor("stringValue#1").has_value());
            assert(!facts.narrowedTypeFor("otherValue#2").has_value());
        });

        assert(!facts.narrowedTypeFor("numberValue#0").has_value());
        assert(!facts.narrowedTypeFor("stringValue#1").has_value());
    });
}

void test_root_invalidation_clears_field_facts()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> fieldFacts{{"box#0.value", simpleType(StaticType::Number)}};

    facts.withNarrowings(fieldFacts, [&]() {
        facts.invalidate("box#0");
        assert(!facts.narrowedTypeFor("box#0.value").has_value());
    });
}

void test_root_invalidation_clears_index_facts()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> indexFacts{{"values#0[0]", simpleType(StaticType::Number)}};

    facts.withNarrowings(indexFacts, [&]() {
        facts.invalidate("values#0");
        assert(!facts.narrowedTypeFor("values#0[0]").has_value());
    });
}

void test_index_binding_invalidation_clears_dynamic_index_facts()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> indexFacts{
        {"values#0[index#1]", simpleType(StaticType::Number)},
        {"matrix#2[row#3][column#4]", simpleType(StaticType::String)},
        {"values#0[other#5]", simpleType(StaticType::Bool)}};

    facts.withNarrowings(indexFacts, [&]() {
        facts.invalidate("index#1");
        assert(!facts.narrowedTypeFor("values#0[index#1]").has_value());
        assert(facts.narrowedTypeFor("matrix#2[row#3][column#4]").has_value());
        assert(facts.narrowedTypeFor("values#0[other#5]").has_value());
    });
}

void test_without_narrowings_restores_state_after_success_and_throw()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> outer{{"value#0", simpleType(StaticType::Number)}};
    const std::vector<FlowNarrowing> inner{{"value#0", simpleType(StaticType::String)}};

    facts.withNarrowings(outer, [&]() {
        facts.withoutNarrowings([&]() {
            assert(!facts.narrowedTypeFor("value#0").has_value());
            facts.withNarrowings(inner, [&]() {
                const std::optional<TypeInfo> narrowed = facts.narrowedTypeFor("value#0");
                assert(narrowed.has_value());
                assert(narrowed->kind == StaticType::String);
            });
            assert(!facts.narrowedTypeFor("value#0").has_value());
        });

        const std::optional<TypeInfo> restored = facts.narrowedTypeFor("value#0");
        assert(restored.has_value());
        assert(restored->kind == StaticType::Number);

        bool threw = false;
        try {
            facts.withoutNarrowings([&]() {
                assert(!facts.narrowedTypeFor("value#0").has_value());
                throw 11;
            });
        } catch (int value) {
            threw = value == 11;
        }
        assert(threw);

        const std::optional<TypeInfo> restoredAfterThrow = facts.narrowedTypeFor("value#0");
        assert(restoredAfterThrow.has_value());
        assert(restoredAfterThrow->kind == StaticType::Number);
    });
}

void test_with_loop_body_preserves_outer_facts_during_body_only()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> outer{
        {"value#0", simpleType(StaticType::Number)},
        {"other#1", simpleType(StaticType::String)}};
    const std::vector<FlowNarrowing> body{{"body#2", simpleType(StaticType::Bool)}};

    facts.withNarrowings(outer, [&]() {
        facts.withLoopBody([&]() {
            const std::optional<TypeInfo> value = facts.narrowedTypeFor("value#0");
            const std::optional<TypeInfo> other = facts.narrowedTypeFor("other#1");
            assert(value.has_value());
            assert(value->kind == StaticType::Number);
            assert(other.has_value());
            assert(other->kind == StaticType::String);

            facts.appendNarrowings(body);
            const std::optional<TypeInfo> bodyFact = facts.narrowedTypeFor("body#2");
            assert(bodyFact.has_value());
            assert(bodyFact->kind == StaticType::Bool);
        });

        assert(!facts.narrowedTypeFor("value#0").has_value());
        assert(!facts.narrowedTypeFor("other#1").has_value());
        assert(!facts.narrowedTypeFor("body#2").has_value());
    });
}

void test_with_loop_body_restores_state_after_throw()
{
    FlowFacts facts;
    const std::vector<FlowNarrowing> outer{{"value#0", simpleType(StaticType::Number)}};
    const std::vector<FlowNarrowing> body{{"body#1", simpleType(StaticType::Bool)}};

    bool threw = false;
    facts.withNarrowings(outer, [&]() {
        try {
            facts.withLoopBody([&]() {
                facts.invalidate("value#0");
                facts.appendNarrowings(body);
                throw 13;
            });
        } catch (int value) {
            threw = value == 13;
        }

        const std::optional<TypeInfo> restored = facts.narrowedTypeFor("value#0");
        assert(restored.has_value());
        assert(restored->kind == StaticType::Number);
        assert(!facts.narrowedTypeFor("body#1").has_value());
    });

    assert(threw);
}

} // namespace

int main()
{
    test_not_nil_narrows_then_branch();
    test_equal_nil_narrows_else_branch_through_grouping();
    test_logical_and_combines_then_facts();
    test_logical_or_combines_else_facts();
    test_non_narrowable_variable_produces_no_facts();
    test_truthiness_narrows_then_branch_only();
    test_target_resolver_narrows_direct_field_targets();
    test_target_resolver_narrows_direct_index_targets();
    test_active_narrowings_can_be_appended_after_branch_analysis();
    test_with_narrowings_restores_stack_after_success_and_throw();
    test_invalidation_propagates_and_nested_facts_restore();
    test_invalidate_all_clears_nested_facts();
    test_root_invalidation_clears_field_facts();
    test_root_invalidation_clears_index_facts();
    test_index_binding_invalidation_clears_dynamic_index_facts();
    test_without_narrowings_restores_state_after_success_and_throw();
    test_with_loop_body_preserves_outer_facts_during_body_only();
    test_with_loop_body_restores_state_after_throw();
}
