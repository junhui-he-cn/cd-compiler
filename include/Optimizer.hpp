#pragma once

#include "SSA.hpp"

#include <cstddef>
#include <string>

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
