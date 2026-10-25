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

// A compiler-owned, scalar-packed frame ABI, independent of Vulkan buffer
// layout decorations. Emission must copy typed components, not memcpy source
// aggregates. Booleans use normalized 32-bit words; scalar widths are preserved.
struct ContinuationTypeLayout {
    uint32_t type;
    uint64_t size;
    uint32_t alignment;
    uint64_t element_stride;
    std::vector<uint64_t> member_offsets;
};
struct ContinuationFrameSlot {
    uint32_t value, type;
    uint64_t offset, size;
    uint32_t alignment;
    // Function variables are relocated to this backing storage for their entire
    // invocation. They are not loaded/copied speculatively at a suspension.
    bool local_storage;
};
struct ContinuationRematerialization {
    uint32_t value, type, word_offset;
    // Replay the typed instruction at word_offset after restoring these IDs.
    // Recipes are in dependency order. Module constants/global reference IDs
    // need no SSA slot; the runtime must separately preserve the backing of RT
    // payload/callable/attribute storage for the shader invocation.
    std::vector<uint32_t> dependencies;
};
struct SuspensionFrame {
    uint32_t word_offset;
    std::vector<uint32_t> saved_values;
    std::vector<ContinuationRematerialization> rematerializations;
};
struct FunctionFrame {
    uint32_t function;
    uint64_t size;
    uint32_t alignment;
    std::vector<ContinuationFrameSlot> slots;
    std::vector<SuspensionFrame> suspensions;
};
struct ContinuationFrameOptions {
    // Explicit backend ABI: AS values are the uint64 software values held in
    // layer-owned descriptor cells. Registry resolution is required before
    // physical BVH access; these are not arbitrary native opaque-handle bytes.
    bool acceleration_structure_addresses = false;
    // A source-CFG state compiler also needs local backing for a shader that
    // returns without suspending (for example a closest-hit/callable callee).
    bool persistent_function_storage = false;
};
struct ContinuationFrames {
    ContinuationAnalysis analysis;
    std::vector<ContinuationTypeLayout> types;
    std::vector<FunctionFrame> functions;
    ContinuationFrameOptions options;
};
// Plan only; does not emit or execute continuations. Logical pointers and opaque
// values are never byte-spilled. Preserve their provenance through supported
// access/copy/load/sample operations and save dynamic index operands. Reject
// unsupported provenance (including pointer parameters/phi, specialized array
// lengths, and reachable mutable module-Private storage) explicitly. Shader
// invocation/control/argument/return backing is a separate unimplemented ABI;
// these metadata alone are not an executable continuation frame implementation.
// The opt-in software-AS-value policy serializes uint64 values, never native
// opaque object bytes. Other opaque/reference values retain the default rules.
// Additional value types request the same checked scalar-packed layouts for
// transient source values needed by emission, without allocating frame slots.
ContinuationFrames PlanContinuationFrames(const std::vector<uint32_t> &spirv, const std::string &entry, uint32_t execution_model,
                                          const std::vector<uint32_t> &additional_value_types = {},
                                          ContinuationFrameOptions options = {});
}  // namespace vel::rt
