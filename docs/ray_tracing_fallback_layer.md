<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Compute ray tracing pipeline fallback

`VK_LAYER_HANYANG_ray_tracing_fallback` is an executable prototype of a Vulkan ray tracing pipeline implemented using compute shaders. It accepts ordinary raygen, miss and closest-hit SPIR-V modules, software-builds BLAS/TLAS, reads shader binding tables and translates `vkCmdTraceRaysKHR` into a compute dispatch. No hardware ray tracing instructions are used by the generated compute program.

**This is a restricted experimental subset, not a conformant implementation of the complete KHR extensions and not a drop-in layer for arbitrary games.** The layer exposes the KHR names to exercise their calling convention, but several mandatory extension commands and shader cases remain outside this implementation. Enable it explicitly only for applications within the supported subset. Do not install it as an implicit system layer.

## Relationship to D3D12RaytracingFallback

The starting point is Microsoft's [D3D12RaytracingFallback](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Libraries/D3D12RaytracingFallback). Its [uber-shader program](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Libraries/D3D12RaytracingFallback/src/UberShaderRayTracingProgram.cpp) links application shaders, traversal and a scheduler into a compute PSO. Its [traversal wrapper](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Libraries/D3D12RaytracingFallback/src/TraverseShader.hlsli) selects hit or miss shader IDs from shader tables.

This project reimplements that architecture for Vulkan; it does not port the Windows runtime or DXIL compiler and does not vendor Microsoft source. In particular, the first compiler subset uses synchronous nonrecursive function calls rather than the original continuation/state-machine compiler. General continuations are the next major compiler milestone.

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

For an application, set `VK_LAYER_PATH` to the build directory containing the JSON manifest and shared library, and explicitly enable `VK_LAYER_HANYANG_ray_tracing_fallback` in instance creation. Request Vulkan 1.2 or newer. ShaderInt64, shaderInt16, storageBuffer16BitAccess, buffer device address and coherent host-visible memory must be supported; the layer enables the required device features internally. The 16-bit storage feature provides correctly aligned UINT16 index loads without reading past a buffer's final index. A spare descriptor-set slot and sufficient compute limits are also needed for the linked program. Only 64-bit builds are supported. The integration test additionally requests Vulkan 1.3 to exercise core synchronization2.

To choose Mesa's CPU Vulkan implementation explicitly:

```sh
export VK_ICD_FILENAMES="$(find /usr/share/vulkan/icd.d -name 'lvp_icd*.json' -print -quit)"
ctest --test-dir build-rt --output-on-failure
```

This uses a software Vulkan device to verify the same compute path that would run on a GPU without hardware RT support. Physical GPU execution and Windows builds have not been verified.

## Supported executable subset

- Device-side BUILD of one geometry per BLAS or TLAS, with up to 1,048,576 primitives or instances.
- Opaque `R32G32B32_SFLOAT` triangles, float-aligned vertex streams, UINT16/UINT32 indices or nonindexed vertices, and optional triangle build transforms.
- Contiguous instance arrays, instance masks, instance custom IDs, per-instance SBT offsets, and invertible transforms. Instances may share a BLAS.
- Raygen, miss and triangle closest-hit stages, including shader selection from SBT group handles.
- A single `vec4` payload at location zero and `vec2` triangle hit attributes. Only raygen may issue TraceRay. Multiple sequential TraceRay calls in raygen do not require recursive lowering.
- Common launch, ray, primitive and instance builtins listed in `compiler.cpp`. Other builtins are rejected during compilation.
- Original application descriptor sets and push constants where the emitted linked GLSL remains compatible. Shader record user data is not supported.
- Direct trace dispatch, deferred pipeline creation completed synchronously, legacy and synchronization2 pipeline-barrier and queue-submit semaphore-stage remapping.
- Compute pipeline, descriptor binding and push constant restoration around internal dispatches. Private dispatch resources remain alive until command-buffer reset, rerecording, pool reset/free or device destruction, after the application has ensured GPU completion.

The builder constructs a balanced tree in input order, with power-of-two padding. It is a correctness/reference builder: it performs no Morton sorting, SAH optimization, compaction or parallel LBVH construction. It is deliberately not presented as a performance port of Microsoft's GPU builder. Scratch is reported conservatively; the current single-invocation builder does not consume it.

## Unsupported cases and failure behavior

Any-hit, intersection/procedural geometry, callable shaders, recursive tracing from hit/miss, arbitrary payload layouts, shader-record data, pipeline libraries, specialization constants, capture/replay, indirect builds/traces, UPDATE, compaction, copy/serialization and host AS construction are not implemented. Secondary-command-buffer state inheritance, device groups, update templates, descriptor buffers, push descriptors, event-based RT synchronization, timestamp-stage translation and general application integration require further work. Use the implemented pipeline barriers and queue-submit paths for RT dependencies. Querying an unimplemented RT command returns null, including on a device that also supports native RT, to prevent mixing incompatible AS formats.

The device-create feature-chain copier currently recognizes loader nodes, Features2, Vulkan11/12/13Features, BufferDeviceAddressFeatures and Synchronization2Features, in addition to the emulated AS/RT feature structs. Other nodes and unsupported optional RT features return `VK_ERROR_FEATURE_NOT_PRESENT`. This intentionally narrow list must be extended before using the layer in a general engine.

Unsupported pipeline constructs return `VK_ERROR_FEATURE_NOT_PRESENT`. Detectable unsupported recorded builds mark the command buffer as failed; `vkEndCommandBuffer` reports `VK_ERROR_VALIDATION_FAILED_EXT` and prints a diagnostic. This is a prototype diagnostic policy, not a claim of Vulkan validation-layer equivalence. Applications must still provide valid Vulkan inputs and synchronization. Device-lost recovery, comprehensive allocation-failure handling and custom host allocator integration have not been qualified.

Use one common application push-constant block across linked stages. Descriptor aliasing, reflection differences and arbitrary shader combinations have not been exhaustively covered. SPIRV-Cross and glslang compilation failures are surfaced rather than silently replacing shaders with a hardcoded program.

## Implementation map

- `layers/raytracing/layer.cpp`: loader negotiation, intercepts, object tracking, descriptor conversion, feature and barrier translation, command recording and resource lifetime.
- `layers/raytracing/compiler.cpp`: real application SPIR-V ingestion, stage namespacing, supported builtin/TraceRay lowering, group dispatch generation and compute compilation.
- `layers/raytracing/runtime.h`: BDA memory access, triangle/instance build shader, BVH traversal, triangle intersections and hit/miss routing.
- `layers/raytracing/tests.cpp`: offscreen KHR API application and independent CPU hit reference.

## Validation

Four integration variants cover UINT16, UINT32, nonindexed and empty BLAS inputs. The UINT16 case starts at a two-byte primitive offset. Nonempty variants use three triangles, a padded tree, shared BLAS instances, nonuniform instance scaling, four cull-mask settings, 13 by 9 dispatch dimensions, SBT handles, shader module destruction after linking, command pool resets and application compute-state restoration. Legacy and synchronization2 semaphore waits and barriers are exercised. Shader tests also link debug-stripped SPIR-V and reject unsupported stage/recursion cases. Deferred-operation entry points are exercised synchronously.

These tests compare every pixel's hit/miss result with an independent CPU triangle test, check valid barycentric payloads, and verify that a following native compute dispatch runs with the application's restored pipeline and push constants. They are not Vulkan CTS coverage or a performance benchmark.

For a validation layer below the fallback, install `vulkan-validationlayers`, include its manifest directory in `VK_LAYER_PATH`, and run `vel_rt_tests` with `VEL_RT_TEST_VALIDATION=1`. That checks the lowered native calls; it does not establish complete conformance of the emulated extension surface.

## Next implementation milestones

1. Replace GLSL-text interface rewriting with typed SPIR-V or NIR lowering and linking; introduce explicit continuation frames for arbitrary payloads, nested TraceRay and callable execution.
2. Implement any-hit traversal resumption, IgnoreIntersection/TerminateRay and procedural intersection shaders, with per-stage and nested-trace tests.
3. Complete the KHR AS API surface: update, copy, compact-size queries, serialization, indirect and host paths, feature negotiation and API-specific synchronization coverage.
4. Replace the serial reference builder with Morton sorting and parallel LBVH construction or a reusable software builder, retaining the same software-AS ABI initially.
5. Expand engine compatibility, state preservation and descriptor models; qualify physical GPU backends, then add systematic CTS coverage before claiming extension conformance.

Point of contact: [@hanyangl5](https://github.com/hanyangl5).
