// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "continuations.h"
#include "typed_system_abi.h"

namespace vel::rt {
inline constexpr uint32_t kTypedContinuationControlWords = 32;
enum class TypedContinuationRequestKind : uint32_t {
    None = 0,
    TraceRay = 1,
    ReportIntersection = 2,
    ExecuteCallable = 3,
    IgnoreIntersection = 4,
    TerminateRay = 5
};
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
struct TypedPrivateVariable {
    uint32_t variable, type;
    uint64_t offset, size;
    uint32_t alignment;
};
struct TypedAccelerationStructureDescriptor {
    uint32_t variable, descriptor_set, binding;
    // The original AS descriptor/array now contains ordinary SSBO value cells.
    // The application binding/array selection is retained; pipeline layout
    // integration must translate descriptor type to STORAGE_BUFFER.
};
enum class TypedFrameTransport { StorageBuffer, PhysicalAddress, PrivateArray };
struct TypedContinuationCompileOptions {
    TypedFrameTransport transport = TypedFrameTransport::StorageBuffer;
    uint32_t frame_descriptor_set = 0, frame_binding = 0;
    // false emits a SPIR-V1.5 Linkage library, with one exported
    // void step(uint64 frameBDA), rather than a compute entry point.
    bool emit_compute_entry = true;
    std::string step_export_name;
    // PrivateArray libraries share one imported invocation-private uint array.
    // The uint64 step parameter then denotes a byte offset, not a native BDA.
    uint32_t private_frame_words = 0;
    // Standalone BDA wrapper only: specialization values supply the low/high
    // uint32 words of an immutable per-dispatch context address.
    uint32_t context_root_low_spec_id = 0, context_root_high_spec_id = 1;
};
struct TypedBuiltinVariable {
    uint32_t variable, type, builtin, byte_offset, byte_size;
};
struct TypedRequiredExecutionMode {
    uint32_t mode;
    std::vector<uint32_t> operands;  // literal operands, not module IDs
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
    // Module-Private storage is independent in each logical source invocation,
    // including recursive activations of the same stage. Constant initializers
    // execute at source entry once; suspension/resume never reinitializes it.
    std::vector<TypedPrivateVariable> private_variables;
    std::vector<TypedAccelerationStructureDescriptor> acceleration_structure_descriptors;
    std::vector<TypedBuiltinVariable> builtin_variables;
    std::vector<TypedRequiredExecutionMode> required_execution_modes;
    TypedFrameTransport transport = TypedFrameTransport::StorageBuffer;
    std::string step_export_name;
    uint32_t private_frame_words = 0;
    // BDA pool addressing/counts are uint64. The legacy max_invocations field
    // below constrains only the default SSBO wrapper. Each individual source
    // frame remains checked below4GiB; global pools have no such32-bit ceiling.
    uint64_t frame_stride_bytes = 0, max_invocations64 = 0;
    uint32_t system_header_bytes = 0, control_byte_offset = 0;
    bool uses_app_push_constants = false, uses_shader_record = false;
    uint64_t resource_snapshot_bytes = 0;
    uint32_t resource_snapshot_fields = 0;
    uint32_t frame_stride_words, control_word, state_count, max_invocations;
};
// In physical mode all FunctionFrame slot offsets, invocation-variable offsets
// and control offsets include VEL_RT_SYSTEM_BYTES. Type member offsets remain
// relative to their own value. Builtins read real fields from that system
// prefix; push constants and shader records use the addresses stored there.
// Library exports have no context/root globals and take frameBDA directly;
// PrivateArray instead takes a uint64 byte offset into the shared Private array.
// IgnoreIntersection/TerminateRay finish the stage with pending kinds4/5;
// payload writes remain in backing and there is no resumed result to inject.
// Actual source CFG lowering, independent of the legacy GLSL compiler. Each
// compute invocation resumes one frame, executes source blocks/parallel Phi
// edges in a dynamic structured dispatcher, and runs until suspension/return.
// Source Function and module-Private storage are persistent in each logical
// stage invocation; Private initializers execute once at the source entry.
// Only suspension-live numeric SSA is spilled.
// Helpers are inlined before analysis. Logical local access-chain/copy aliases,
// numeric/composite ALU, push constants, ordinary buffer resources, and launch
// ID/size inputs are supported. Native image/sampler values and image texel
// pointers use traditional descriptor provenance across state boundaries.
// Dynamic selectors, texel coordinates and sample numbers are captured with
// typed codecs at their original definitions, independently for each result.
// Only stable descriptor identities are reconstructed; image reads and atomic
// effects retain their original source positions, scopes and memory semantics.
// Opaque Phi/Select and nontraditional descriptor handles remain unsupported.
// The current backend accepts Logical GLSL450 only; VulkanMemoryModel and
// physical-storage-buffer addressing require explicit scheduler/frame support.
//
// TraceRay/ExecuteCallable/ReportIntersection emit typed late-completion
// requests. All selected-entry payload/callable/attribute variables have
// persistent invocation backing, including incoming storage in non-suspending
// callees. AS values use an explicit uint64 opaque-value ABI; native AS descriptors
// become SSBO value cells, with remap metadata returned for layout integration.
// Production pipeline creation links exported stage libraries into the shared
// traversal scheduler. The standalone compute entry remains a component harness.
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
TypedContinuationProgram CompileTypedContinuationSteps(const std::vector<uint32_t> &source, const std::string &entry,
                                                       uint32_t execution_model, const TypedContinuationCompileOptions &options);
}  // namespace vel::rt
