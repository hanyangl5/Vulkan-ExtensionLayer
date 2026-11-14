<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Software RT Fallback: Implementation Status

Checkpoint: 2026-10-30 (Asia/Hong_Kong). Branch
`codex/ray-tracing-pipeline-only`, based on `219ad90`.
**The complete AS/RTP fallback is not implemented.**

## Scope pivot: Ray Query removed

This branch delivers `VK_KHR_acceleration_structure` and
`VK_KHR_ray_tracing_pipeline` only. `VK_KHR_ray_query` is out of scope, per the
[goal](ray_tracing_fallback_goal.md). The removal is not a build flag:

- Discovery no longer advertises `VK_KHR_ray_query`; the software extension
  list contains AS, RTP, the pipeline-library dependency and deferred host
  operations.
- `VkPhysicalDeviceRayQueryFeaturesKHR` is consumed before the driver sees it
  and never reported as supported. A minimal ray-query request still selects
  the software backend device, but the feature is not granted.
- The consuming-stage Ray Query lowering (`vkrt_spirv_lower`, the SPIR-V ID
  rewriter, the `sw_ray_query.spv` embed and its staged copy) is deleted.
  `vkCreateShaderModule` now only classifies: a module containing
  `OpTypeRayQueryKHR`/`OpRayQuery*KHR` or an RT pipeline construct that escaped
  the pipeline path is rejected with `VK_ERROR_FEATURE_NOT_PRESENT`.
- The typed pipeline compiler no longer has a "Ray Query inside RT stages"
  branch, `TypedContinuationCompileOptions::ray_query_as_context` is gone, and
  an RT stage that still declares a query type fails explicitly.
- Shared traversal was renamed from `sw_ray_query.*` to `sw_traversal.*` and is
  compiled into each RT stage from its embedded GLSL source. It remains the RTP
  traversal engine, not Ray Query code.
- Ray Query only fixtures, CTS selections and case lists were removed. The
  RT-stage query/suspension checkpoint and its evidence records are deleted.

### Coverage consequences

These are intentional and must not be reported as passes:

- `test_gpu_build` and `test_single_tri` (compute ray-differential oracles) are
  removed together with `rq_test.comp`. End-to-end AS build verification now
  runs through the RT pipeline fixtures (`test_rtp_api` and friends).
- `test_as_copy`, `test_as_update` and `test_as_serialization` keep their GPU
  AS object, copy, refit, serialization and structural assertions, but their
  former "trace one ray through the copied/updated AS" expectations are gone.
  Traversal over those objects is covered by the RT pipeline fixtures instead.
- `test_shader_inline` no longer has a Ray Query mode; it now also asserts that
  the layer does not advertise `rayQuery`.
- Discovery/feature tests now assert `rayQuery == VK_FALSE` and a software
  extension list without `VK_KHR_ray_query`.

## Verified on this branch

- Standalone build (`-DBUILD_TESTING=ON`) completes with the SDK toolchain.
- Offline suite: **49/49 pass** in `build-rtp-only`
  (compiler, concurrency, no-native-RT host/guard, AS/BVH4, descriptor,
  shader-object and translation checks).
- Removed from the suite: 4 Ray Query tests (typed query pipeline, spirv lower,
  spirv ids, shader-object RQ) and the ray-differential GPU tests.

Not yet re-qualified on this branch:

- GPU and external CTS runs. The last recorded RTP/AS CTS results belong to the
  pre-pivot tree and must be re-run with the new DLL before they are quoted.
- `dEQP-VK.ray_query.*` cases are no longer applicable.

## Code in this checkpoint

| Component | State | Remaining boundary |
| --- | --- | --- |
| AS backend / BVH4 | Mesa LBVH/PLOC/HPLOC, GPU four-child collapse, topology-preserving refit, GPU CLONE/COMPACT, serialization/relocation, bounded property queries | Large-ID/large-allocation qualification, builder throughput, numerical/transform completeness |
| Typed continuation compiler | Six stages, SBT, callables, nested traces, pipeline libraries, private/function pointer memory, GLSL450/Vulkan memory model, state frames | Compile cost, remaining memory/execution-mode variants, full resource limits |
| RTP runtime | Compute scheduler, execution-owned BDA arenas, cached native variants, state restoration, 3D launch IDs | Callable-stage compile time under CTS watchdogs, large-launch memory cost |
| API translation | AS/RTP discovery, device stripping, descriptor/pipeline layout ownership, shader-module/pipeline-layout/descriptor-set-layout tokens, deferred operations | Complete command schemas, native query interaction, protected/multi-device |
| Synchronization | RT stage/access translation, events, query publication, execution replay | Logical-stage executor, complete scope qualification |
| Software profile | Feature/property/format truth, resource budget, AS value publication | Physical no-RT qualification, group/protected and POSIX matrix |

## Milestones

- [ ] M0: capability/no-native-RT/resource/continuation proofs.
- [ ] M1: complete Mesa builder integration, GPU BVH4 encode/refit, shared AS.
- [ ] M2: complete typed RTP continuation compilation and runtime.
- [ ] M3: complete mandatory API, resource, synchronization and lifecycle.
- [ ] M4: qualify supported configurations and physical no-RT GPUs.

## Reproduce

```powershell
cmake -S layers/raytracing -B build-rtp-only -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON `
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang.exe" `
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe" `
  -DPython3_EXECUTABLE="<python>" `
  -DVulkan_INCLUDE_DIR="<Vulkan-Headers>/include" `
  -DVKRT_VULKAN_REGISTRY="<Vulkan-Headers>/registry/vk.xml"
cmake --build build-rtp-only --parallel 8
ctest --test-dir build-rtp-only -L offline --output-on-failure
```

GPU fixtures need the layer on `VK_LAYER_PATH` and
`ENABLE_VKRT_SOFTWARE_RAY_QUERY=1`; see the CTest properties for the exact
environment.

## Next work

1. Re-run the AS/RTP external CTS selection with this DLL and record it as a
   new checkpoint. Do not reuse Ray Query era numbers.
2. Reduce per-pipeline compile/link cost; the pre-pivot RTP stage compile was
   tens of seconds and a whole-pipeline `-O` took minutes, which trips CTS
   watchdogs before any result is produced.
3. Re-baseline the GPU test label matrix; several tests lost their traversal
   oracle in this pivot and need RTP-based replacements where the object under
   test is the AS itself.
4. Complete the mandatory resource budget, query-schema and physical no-RT
   qualification items carried over from the pre-pivot status history.
