// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "continuations.h"

namespace vel::rt {
inline constexpr uint32_t kTypedContinuationControlWords = 32;
enum class TypedContinuationRequestKind : uint32_t { None = 0, TraceRay = 1, ReportIntersection = 2, ExecuteCallable = 3 };
// Shared fixed-word request transport. Float arguments are IEEE float32 bits;
// ASLow/ASHigh form the actual uint64 opaque software AS value; registry
// resolution is required before physical BVH access. Data references are relative
// to this invocation's frame base and use the compiler-owned typed layout.
// Integer arguments retain raw source bits. The Trace consumer applies the
// instruction's low-8 cull mask, low-4 SBT offset/stride, and low-16 miss index.
enum class TypedContinuationRequestWord : uint32_t {
    PC = 0,
    Pending = 1,
    Ready = 2,
    Result = 3,
    ASLow = 4,
    ASHigh = 5,
    Site = 6,
    Flags = 7,
    CullMask = 8,
    SBTOffset = 9,
    SBTStride = 10,
    MissIndex = 11,
    OriginX = 12,
    OriginY = 13,
    OriginZ = 14,
    TMin = 15,
    DirectionX = 16,
    DirectionY = 17,
    DirectionZ = 18,
    TMax = 19,
    DataOffset = 20,
    DataSize = 21,
    DataLocation = 22,
    DataStorageClass = 23,
    DataType = 24,
    DataVariable = 25,
    // Kind-specific aliases for the first argument words.
    ReportDistance = 4,
    ReportHitKind = 5,
    CallableIndex = 4
};
struct TypedInvocationVariable {
    uint32_t variable, type, storage_class, location;
    uint64_t offset, size;
    uint32_t alignment;
};
struct TypedAccelerationStructureDescriptor {
    uint32_t variable, descriptor_set, binding;
    // The original AS descriptor/array now contains ordinary SSBO value cells.
    // The application binding/array selection is retained; pipeline layout
    // integration must translate descriptor type to STORAGE_BUFFER.
};
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
    std::vector<TypedInvocationVariable> invocation_variables;
    std::vector<TypedAccelerationStructureDescriptor> acceleration_structure_descriptors;
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
// TraceRay/ExecuteCallable/ReportIntersection emit typed late-completion
// requests. All selected-entry payload/callable/attribute variables have
// persistent invocation backing, including incoming storage in non-suspending
// callees. AS values use an explicit uint64 opaque-value ABI; native AS descriptors
// become SSBO value cells, with remap metadata returned for layout integration.
// This component is not yet wired into production pipeline creation/traversal.
// Other source resource bindings and push constants retain their original ABI.
//
// Control words, relative to control_word: 0=PC (UINT32_MAX means finished),
// 1=pending kind, 2=result ready, 3=Report bool; remaining request fields are in
// TypedContinuationRequestWord. Initialize all kTypedContinuationControlWords
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
