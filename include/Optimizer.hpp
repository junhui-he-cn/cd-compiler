#pragma once

#include "SSA.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

enum class SSAOptimizationLevel {
    O0,
    O1,
};

struct SSAOptimizationStats {
    std::size_t copiesPropagated = 0;
    std::size_t phisSimplified = 0;
    std::size_t instructionsRemoved = 0;
    std::size_t passesRun = 0;
};

struct SSAOptimizationResult {
    SSAFunction function;
    SSAOptimizationStats stats;

    void verify(const ControlFlowGraph& cfg) const;
};

// Constant evaluation is deliberately classified before a future folding
// pass is allowed to replace an instruction. Runtime traps stay runtime traps;
// non-finite values are rejected at the cdbc constant boundary rather than
// being serialized as optimizer output.
enum class SSAConstantEvaluationKind {
    Folded,
    RuntimeTrap,
    NonFinite,
    Unsupported,
};

struct SSAConstantEvaluation {
    SSAConstantEvaluationKind kind = SSAConstantEvaluationKind::Unsupported;
    std::optional<Value> value = std::nullopt;

    bool isFolded() const
    {
        return kind == SSAConstantEvaluationKind::Folded && value.has_value();
    }
};

SSAConstantEvaluation evaluateSSAConstantUnary(IROp op, const Value& operand);
SSAConstantEvaluation evaluateSSAConstantBinary(
    IROp op,
    const Value& left,
    const Value& right);

// The fingerprint is a stable internal pipeline identity. O0 cache records
// mirror this identity; optimized pipeline selection is not yet connected to
// production lowering.
std::string ssaOptimizationPipelineFingerprint(SSAOptimizationLevel level);

// Optimize one already verified SSA function. O0 is an identity pass. O1 is
// intentionally conservative: it propagates Copy values, simplifies only
// trivial phis whose replacement dominates the join, and removes unused pure
// value-producing instructions. Memory, calls, allocation, traps, and control
// flow remain untouched.
SSAOptimizationResult optimizeSSA(
    const ControlFlowGraph& cfg,
    const SSAFunction& input,
    SSAOptimizationLevel level);

// Run one ordinary IR function through the internal CFG/SSA optimizer
// boundary. This preserves the existing virtual-register representation and
// returns offset metadata for a later program-level integration; it is not
// called by IRCompiler, the CLI, or any artifact emitter yet.
SSADeSSAIRResult optimizeIRFunction(
    const IRFunction& input,
    const std::vector<IRModuleDependency>& moduleDependencies,
    SSAOptimizationLevel level);
