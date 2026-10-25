// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "continuations.h"

namespace vel::rt {
struct TypedContinuationFrameBounds {
    uint32_t stride_words, max_invocations;
};
// Checked frame/control ABI sizing; useful before allocating any GPU memory.
TypedContinuationFrameBounds PlanTypedContinuationFrameBounds(uint64_t payload_bytes);
struct TypedContinuationProgram {
    std::vector<uint32_t> spirv;
    // Validated source after exhaustive helper inlining. IDs in frame metadata
    // refer to this module, rather than to pre-inlining application IDs.
    std::vector<uint32_t> source;
    ContinuationFrames frames;
    uint32_t frame_stride_words, control_word, state_count, max_invocations;
};
// Actual source CFG lowering, independent of the legacy GLSL compiler. Each
// compute invocation resumes one frame, executes source blocks/parallel Phi
// edges in a dynamic structured dispatcher, and runs until suspension/return.
// Source Function storage is persistent; only suspension-live SSA is spilled.
// Helpers are inlined before analysis. Logical local access-chain/copy aliases,
// numeric/composite ALU, push constants, ordinary buffer resources, and launch
// ID/size inputs are supported. Other pointer/opaque provenance is rejected.
// The current backend accepts Logical GLSL450 only; VulkanMemoryModel and
// physical-storage-buffer addressing require explicit scheduler/frame support.
//
// Current scheduler hook: ReportIntersectionKHR. Other RT operation/invocation
// backing ABIs remain explicitly unsupported; this is not wired into pipeline
// creation. The frame buffer is at the caller-specified descriptor set/binding.
// Source resource bindings and push constants retain their original ABI.
//
// Control words, relative to control_word: 0=PC (UINT32_MAX means finished),
// 1=pending kind (0=none, 2=ReportIntersection), 2=result ready, 3=result bool,
// 4=distance float bits, 5=hit kind, 6=source suspension word offset. Initialize
// control words to zero. Between synchronized dispatches, complete a request by
// writing result then ready=1; this ABI does not permit concurrent frame writes.
// Dispatching while pending && !ready performs no source operations. The source
// result is materialized only after completion; the next suspension clears ready.
// Invocation flattening rejects arithmetic overflow, indices >= max_invocations,
// and frames beyond the actual descriptor's runtime-array capacity before any
// frame load/store. This does not validate the source resource descriptors.
TypedContinuationProgram CompileTypedContinuationSteps(const std::vector<uint32_t> &source, const std::string &entry,
                                                       uint32_t execution_model, uint32_t frame_descriptor_set = 0,
                                                       uint32_t frame_binding = 0);
}  // namespace vel::rt
