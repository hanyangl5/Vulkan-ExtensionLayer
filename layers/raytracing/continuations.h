// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vel::rt {
// Front-end analysis for the SPIR-V equivalent of DxrFallback's LiveValues and
// StateFunctionTransform. This does not emit a state machine or a compute shader.
enum class SuspensionKind { TraceRay, ReportIntersection, ExecuteCallable, FunctionCall };
struct Suspension {
    SuspensionKind kind;
    uint32_t word_offset;
    uint32_t block;
    uint32_t result;
    // Function-local SSA values needed by the continuation, before the result of
    // the suspended operation exists. Module constants and globals are excluded.
    // This is NOT a stack layout: pointers/resources require rematerialization;
    // address-taken function storage requires separate lifetime/alias analysis.
    std::vector<uint32_t> live_values;
};
struct BlockLiveness {
    uint32_t label;
    std::vector<uint32_t> successors;
    std::vector<uint32_t> live_in;
    std::vector<uint32_t> live_out;
};
struct FunctionContinuations {
    uint32_t function;
    bool can_suspend;
    std::vector<BlockLiveness> blocks;
    std::vector<Suspension> suspensions;
};
struct ContinuationAnalysis {
    uint32_t entry;
    std::vector<FunctionContinuations> functions;
};
// Analyze only the entry point's reachable function call graph. Validates the
// module with SPIRV-Tools first; does not depend on OpName or debug information.
ContinuationAnalysis AnalyzeContinuations(const std::vector<uint32_t> &spirv, const std::string &entry, uint32_t execution_model);
}  // namespace vel::rt
