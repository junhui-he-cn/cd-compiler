#pragma once

#include "SSA.hpp"

#include <cstddef>
#include <map>
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
    std::size_t constantsFolded = 0;
    std::size_t branchesSimplified = 0;
    std::size_t blocksRemoved = 0;
    std::size_t blocksMerged = 0;
    std::size_t jumpsRemoved = 0;
    std::size_t jumpsThreaded = 0;
    std::size_t instructionsRemoved = 0;
    std::size_t passesRun = 0;
};

struct SSAOptimizationResult {
    SSAFunction function;
    SSAOptimizationStats stats;
    // Only folded instructions need a new constant-pool value. Original
    // Constant operands already refer to the source program pool.
    std::map<SSAValueId, Value> foldedConstants;

    void verify(const ControlFlowGraph& cfg) const;
};

// A program-level result owns one verified de-SSA result for the anonymous
// main stream and one for every function-table entry.  The vector order is
// intentionally the original function-table order: MakeFunction operands are
// indices into that table and are never renumbered by this adapter.
struct SSADeSSAProgramResult {
    SSADeSSAIRResult mainStream;
    std::vector<SSADeSSAIRResult> functions;
    SSAOptimizationStats mainStats;
    std::vector<SSAOptimizationStats> functionStats;

    // Check stream maps, function identity/order, binding metadata, and
    // function references against the source program before rebuilding it.
    void verify(const IRProgram& input) const;

    // Copy the source program's constants, names, sources, and canonical
    // bindings, replacing only the verified IR streams and main dependency
    // anchors.
    IRProgram rebuild(const IRProgram& input) const;
};

// Constant evaluation is deliberately classified before a folding pass is
// allowed to replace an instruction. Runtime traps stay runtime traps;
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

// The fingerprint is a stable pipeline identity used by module-product cache
// records. O0 is the default; explicit O1 lowering selects its own versioned
// identity so products cannot be reused across optimization configurations.
std::string ssaOptimizationPipelineFingerprint(SSAOptimizationLevel level);

// Optimize one already verified SSA function. O0 is an identity pass. O1 is
// intentionally conservative: it propagates Copy values, simplifies only
// trivial phis whose replacement dominates the join, folds primitive constant
// expressions only when evaluation proves success, and removes unused pure
// value-producing instructions. Memory, calls, allocation, traps, and control
// flow remain untouched. The optional pool is needed only for source-level
// Constant instructions; callers without it retain the earlier internal
// simplification behavior.
SSAOptimizationResult optimizeSSA(
    const ControlFlowGraph& cfg,
    const SSAFunction& input,
    SSAOptimizationLevel level,
    const std::vector<Value>* constantPool = nullptr);

// Run one ordinary IR function through the internal CFG/SSA optimizer
// boundary. This preserves the existing virtual-register representation and
// returns offset metadata for the program-level integration. O1 also performs
// known-condition normalization, post-de-SSA unreachable-block pruning, and
// conservative linear block merging; direct callers without a pool retain
// source Constant operands without folding them.
SSADeSSAIRResult optimizeIRFunction(
    const IRFunction& input,
    const std::vector<IRModuleDependency>& moduleDependencies,
    SSAOptimizationLevel level);

// Run the same internal adapter over the main stream and every nested
// function. Explicit O1 CLI/module-product lowering uses this opt-in service;
// O0 continues to bypass it for byte-for-byte compatibility.
SSADeSSAProgramResult optimizeIRProgram(
    const IRProgram& input,
    SSAOptimizationLevel level);
