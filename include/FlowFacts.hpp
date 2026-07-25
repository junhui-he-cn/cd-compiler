#pragma once

#include "Ast.hpp"
#include "TypeUtils.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct FlowNarrowing {
    std::string resolvedName;
    TypeInfo type;
};

struct BranchFlowFacts {
    std::vector<FlowNarrowing> thenNarrowings;
    std::vector<FlowNarrowing> elseNarrowings;
};

class FlowFacts {
public:
    using VariableNarrowingResolver = std::function<std::optional<FlowNarrowing>(const VariableExpr&)>;
    using TargetNarrowingResolver = std::function<std::optional<FlowNarrowing>(const Expr&)>;

    void clear();
    BranchFlowFacts factsForIfCondition(
        const Expr& condition,
        const VariableNarrowingResolver& resolveVariableNarrowing) const;
    BranchFlowFacts factsForIfConditionTargets(
        const Expr& condition,
        const TargetNarrowingResolver& resolveTargetNarrowing) const;
    std::vector<FlowNarrowing> activeNarrowings() const;
    void appendNarrowings(const std::vector<FlowNarrowing>& narrowings);
    std::optional<TypeInfo> narrowedTypeFor(const std::string& resolvedName) const;
    void invalidate(const std::string& resolvedName);
    void invalidateAll();
    void withoutNarrowings(const std::function<void()>& body);
    void withNarrowings(
        const std::vector<FlowNarrowing>& narrowings,
        const std::function<void()>& body);
    void withLoopBody(const std::function<void()>& body);

private:
    struct ActiveFlowFact {
        std::string resolvedName;
        std::optional<TypeInfo> narrowedType;
    };

    std::vector<ActiveFlowFact> activeNarrowings_;
};
