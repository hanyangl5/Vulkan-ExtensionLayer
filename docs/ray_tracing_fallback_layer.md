<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Compute ray tracing pipeline fallback

Historical record: the superseded HANYANG executable and its build entry were
removed during source consolidation. The commands and source descriptions below
refer to their original revisions, available in Git history. Use the current
[unified implementation](../layers/raytracing/README.md) for builds and execution.

The active target is [complete mandatory Vulkan AS/RQ/RTP fallback](ray_tracing_fallback_goal.md), using Mesa construction, a shared portable BVH4 runtime, and typed SPIR-V continuation lowering. See the [baseline feasibility review](ray_tracing_fallback_review.md). The complete fallback is not finished.

This page records the executable RTP prototype and its limitations at baseline `f18c40d3`. Its D3D-derived builder and direct-call shader path are migration sources, not the final selected architecture. The [D3D12 port record](ray_tracing_fallback_port.md) preserves historical source and component-test evidence.

Current typed RTP development is integrated into
`VK_LAYER_VKRT_SOFTWARE_RAY_QUERY`, sharing its Mesa/BVH4 AS backend. Its
invocation-private continuation scheduler, public RTP API, GPU SBT and selected
external CTS evidence are recorded in the [current checkpoint](rt_cts_typed_runtime_2026-10-26.md).
The baseline limitations below describe the older
`VK_LAYER_HANYANG_ray_tracing_fallback` executable; they are retained as history.

`VK_LAYER_HANYANG_ray_tracing_fallback` is an executable prototype of a Vulkan ray tracing pipeline implemented using compute shaders. It accepts ordinary raygen, miss and closest-hit SPIR-V modules, software-builds BLAS/TLAS, reads shader binding tables and translates `vkCmdTraceRaysKHR` into a compute dispatch. No hardware ray tracing instructions are used by the generated compute program.

**This is a restricted experimental subset, not a conformant implementation of the complete KHR extensions and not a drop-in layer for arbitrary games.** The layer exposes the KHR names to exercise their calling convention, but several mandatory extension commands and shader cases remain outside this implementation. Enable it explicitly only for applications within the supported subset. Do not install it as an implicit system layer.

## Relationship to D3D12RaytracingFallback

The starting point is Microsoft's [D3D12RaytracingFallback](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Libraries/D3D12RaytracingFallback). Its [uber-shader program](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Libraries/D3D12RaytracingFallback/src/UberShaderRayTracingProgram.cpp) links application shaders, traversal and a scheduler into a compute PSO. Its [traversal wrapper](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Libraries/D3D12RaytracingFallback/src/TraverseShader.hlsli) selects hit or miss shader IDs from shader tables.

The executable prototype reimplements only that architecture. Pinned Microsoft runtime/compiler references now live in `layers/raytracing/docs/upstream`, with original licenses. The complete port has begun with typed SPIR-V continuation analysis and a tested GLSL adaptation of the original runtime helpers. The prototype execution path still uses synchronous nonrecursive calls; state-function generation and runtime integration remain unfinished.

| D3D12 fallback concept | Vulkan prototype |
| --- | --- |
| Compute BVH construction and traversal | Compute-built binary BLAS and TLAS accessed through buffer device addresses |
| Shader identifiers in shader tables | 32-byte group handles, with a group selector in the first word |
| Linked compute uber shader | SPIRV-Cross emits namespaced GLSL functions; a new linker lowers supported RT operations; glslang emits compute SPIR-V |
| DXIL continuation compiler | Restricted direct-call lowering; only raygen may call TraceRay |
| Root signature patching | AS descriptors become storage-buffer address descriptors; an internal dispatch descriptor set is appended |
| DispatchRays | CmdTraceRaysKHR becomes compute workgroups of 8 by 8 by 1 |

## Build and run

The independently buildable directory avoids changing the existing layers' default dependency requirements. On Ubuntu 24.04:

```sh
sudo apt-get install cmake ninja-build libvulkan-dev mesa-vulkan-drivers \
    glslang-dev glslang-tools spirv-tools libspirv-cross-c-shared-dev
cmake -S layers/raytracing -B build-rt -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-rt
ctest --test-dir build-rt --output-on-failure
```

The normal repository build can also opt in using `-DBUILD_RAY_TRACING_FALLBACK=ON`, with those additional compiler dependencies installed. Default builds leave the experimental target disabled.

For an application, set `VK_LAYER_PATH` to the build directory containing the JSON manifest and shared library, and explicitly enable `VK_LAYER_HANYANG_ray_tracing_fallback` in instance creation. Request Vulkan 1.2 or newer. ShaderInt64, shaderInt16, storageBuffer16BitAccess, buffer device address and coherent host-visible memory must be supported; the layer enables the required device features internally. The 16-bit storage feature provides correctly aligned UINT16 index loads without reading past a buffer's final index. The original bitonic kernels require 1024 invocations per workgroup, X workgroup size of at least 1024, and 16 KiB shared memory. These limits are checked before advertising fallback support. A spare descriptor-set slot and sufficient remaining compute limits are also needed for the linked program. Only 64-bit builds are supported. The integration test additionally requests Vulkan 1.3 to exercise core synchronization2.

To choose Mesa's CPU Vulkan implementation explicitly:

```sh
export VK_ICD_FILENAMES="$(find /usr/share/vulkan/icd.d -name 'lvp_icd*.json' -print -quit)"
ctest --test-dir build-rt --output-on-failure
```

This uses a software Vulkan device to verify the same compute path that would run on a GPU without hardware RT support. Physical GPU execution and Windows builds have not been verified.

## Supported executable subset

- Device-side BUILD of multiple geometries per BLAS (including zero-count geometries), and the required single instance geometry per TLAS; up to 1,048,576 primitives/instances and geometries.
- `R32G32B32_SFLOAT` triangles, float-aligned vertex streams, UINT16/UINT32 indices or nonindexed vertices, byte primitive offsets, firstVertex and optional triangle build transforms. AABB geometry can be built/refitted; procedural shader execution remains unimplemented.
- Contiguous instance arrays or arrays of device-address pointers, inactive instances (null BLAS references), masks, custom IDs, SBT offsets, and invertible transforms. Instances may share a BLAS. Sorting preserves original primitive, geometry and instance IDs.
- Raygen, miss and triangle closest-hit stages, including per-geometry SBT stride and shader selection from group handles.
- In-place/out-of-place UPDATE using saved inverse-sort and parent caches; CLONE and the upstream fallback's same-size COMPACT copy. True compression and compact-size queries remain unimplemented.
- A single `vec4` payload at location zero and `vec2` triangle hit attributes. Only raygen may issue TraceRay. Multiple sequential TraceRay calls in raygen do not require recursive lowering.
- Common launch, ray, primitive and instance builtins listed in `compiler.cpp`. Other builtins are rejected during compilation.
- Original application descriptor sets and push constants where the emitted linked GLSL remains compatible. Shader record user data is not supported.
- Direct trace dispatch, deferred pipeline creation completed synchronously, legacy and synchronization2 pipeline-barrier and queue-submit semaphore-stage remapping.
- Compute pipeline, descriptor binding and push constant restoration around internal dispatches. Private dispatch resources remain alive until command-buffer reset, rerecording, pool reset/free or device destruction, after the application has ensured GPU completion.

The serial padded/input-order tree is replaced by a multi-pass GPU builder: parallel geometry loading, per-group/scene AABB reduction, the source's Y/X/Z 30-bit Morton codes, stable key/index bitonic sorting (2048 shared items per 1024-thread group with outer/inner merge passes), primitive/metadata reorder, Karras LBVH hierarchy and bottom-up AABB fitting. Device-scope acquire/release counter operations publish child bounds across workgroups. Scratch now stores the working arrays and must match the queried size and 256-byte address alignment. UPDATE reloads into the original sorted slots and refits without rebuilding the hierarchy.

BLAS builds now run the source's seven-frontier-node treelet optimization after LBVH construction. Each 32-thread group evaluates the 127 nonempty subsets with cardinality-ordered dynamic programming, reuses six internal nodes, and resumes bottom-up through sibling completion counters. Default builds run one pass (threshold 7); fast-trace runs up to three (7/14/28); fast-build skips optimization. TLAS and UPDATE skip treelets. A final fit places the smaller subtree first. Empty/zero-area boxes use deterministic frontier selection and finite costs. The source's collapse-cost model is retained; its triangle-list-leaf emission is itself guarded out, so the Vulkan tree retains binary leaves.

This is **not yet a fully qualified original-builder port**: scratch aliasing/packing and D3D12-versus-Vulkan behavioral qualification remain outstanding. The Vulkan BDA layout is versioned separately (ABI v2); it is not binary-compatible with D3D12 or the previous prototype. See the port contract for its layout.

## Unsupported cases and failure behavior

Any-hit, procedural traversal/intersection shaders, callable shaders, recursive tracing from hit/miss, arbitrary payload layouts, shader-record data, pipeline libraries, specialization constants, capture/replay, indirect builds/traces, true compaction, size-query commands, serialization and host AS construction are not implemented. Secondary-command-buffer state inheritance, device groups, update templates, descriptor buffers, push descriptors, event-based RT synchronization, timestamp-stage translation and general application integration require further work. Use the implemented pipeline barriers and queue-submit paths for RT dependencies. Querying an unimplemented RT command returns null, including on a device that also supports native RT, to prevent mixing incompatible AS formats.

The device-create feature-chain copier currently recognizes loader nodes, Features2, Vulkan11/12/13Features, BufferDeviceAddressFeatures and Synchronization2Features, in addition to the emulated AS/RT feature structs. Other nodes and unsupported optional RT features return `VK_ERROR_FEATURE_NOT_PRESENT`. This intentionally narrow list must be extended before using the layer in a general engine.

Unsupported pipeline constructs return `VK_ERROR_FEATURE_NOT_PRESENT`. Detectable unsupported recorded builds mark the command buffer as failed; `vkEndCommandBuffer` reports `VK_ERROR_VALIDATION_FAILED_EXT` and prints a diagnostic. This is a prototype diagnostic policy, not a claim of Vulkan validation-layer equivalence. Applications must still provide valid Vulkan inputs and synchronization. Device-lost recovery, comprehensive allocation-failure handling and custom host allocator integration have not been qualified.

Use one common application push-constant block across linked stages. Descriptor aliasing, reflection differences and arbitrary shader combinations have not been exhaustively covered. SPIRV-Cross and glslang compilation failures are surfaced rather than silently replacing shaders with a hardcoded program.

## Implementation map

- `layers/raytracing/layer.cpp`: loader negotiation, intercepts, object tracking, descriptor conversion, feature and barrier translation, command recording and resource lifetime.
- `layers/raytracing/compiler.cpp`: real application SPIR-V ingestion, stage namespacing, supported builtin/TraceRay lowering, group dispatch generation and compute compilation.
- `layers/raytracing/runtime.h`: BDA memory access, BVH traversal, triangle intersections and hit/miss routing.
- `layers/raytracing/bvh.cpp`, `bvh.h`, `bvh.glsl`: AS/scratch layout, GPU load/reduce/Morton/bitonic/reorder/LBVH/treelet/fit/copy kernels.
- `layers/raytracing/tests.cpp`: offscreen KHR API application and independent CPU hit reference.

## Validation

The original four integration variants cover UINT16, UINT32, nonindexed and empty BLAS inputs. The UINT16 case starts at a two-byte primitive offset. Nonempty variants use three triangles, a padded tree, shared BLAS instances, nonuniform instance scaling, four cull-mask settings, 13 by 9 dispatch dimensions, SBT handles, shader module destruction after linking, command pool resets and application compute-state restoration. Legacy and synchronization2 semaphore waits and barriers are exercised. Shader tests also link debug-stripped SPIR-V and reject unsupported stage/recursion cases. Deferred-operation entry points are exercised synchronously.

Additional builder tests cover 0/1/3/4099/10243 primitives, all-equal Morton keys, empty leading/intermediate/trailing geometries, indexed/nonindexed firstVertex, primitive and transform offsets, CPU stable-sort ordering, every node's bounds, parent and inverse caches, unchanged UPDATE connectivity, source preservation for out-of-place UPDATE and exact clone bytes. A TLAS test checks 2051 pointer-array instances against CPU-transformed AABB corners, including negative/nonuniform scale, inactive instances, AABB BLAS/TLAS updates and an all-inactive rebuild. A rendered metadata variant checks original IDs and different per-geometry closest-hit shader selection after sorting.

These tests compare every pixel's hit/miss result with an independent CPU triangle test, check valid barycentric payloads, and verify that a following native compute dispatch runs with the application's restored pipeline and push constants. They are not Vulkan CTS coverage or a performance benchmark.

For a validation layer below the fallback, install `vulkan-validationlayers`, include its manifest directory in `VK_LAYER_PATH`, and run `vel_rt_tests` with `VEL_RT_TEST_VALIDATION=1`. That checks the lowered native calls; it does not establish complete conformance of the emulated extension surface.

## Remaining complete-port work

The [active goal and milestones](ray_tracing_fallback_goal.md#milestones-and-exit-criteria) supersede this earlier D3D-port work list, particularly the exact builder/scratch reproduction requirement below.

1. Generate typed continuation state functions and stack frames for arbitrary payloads and nested TraceRay; integrate the tested runtime instead of direct-call GLSL rewriting.
2. Implement resumable any-hit/procedural traversal, IgnoreIntersection/TerminateRay and intersection shaders.
3. Port original scratch reuse/packing and qualify treelet fast-trace/fast-build behavior against upstream.
4. Complete size queries, serialization, remaining API/state/descriptor translation and Vulkan-specific obligations.
5. Port original behavioral tests, qualify physical GPUs and Windows, and establish conformance coverage.

Point of contact: [@hanyangl5](https://github.com/hanyangl5).
