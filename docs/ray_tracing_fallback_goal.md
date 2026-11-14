<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Software Ray Tracing Pipeline Fallback: Implementation Goal

**Project status:** active development; the complete fallback is not implemented.
This contract was rewritten on 2026-10-30 for branch
`codex/ray-tracing-pipeline-only`. It supersedes the earlier AS/RQ/RTP goal:
**Ray Query (`VK_KHR_ray_query`) is out of scope and is no longer advertised,
lowered, or tested.**

## Outcome

Implement a Vulkan layer that provides the complete mandatory functionality of
`VK_KHR_acceleration_structure` and `VK_KHR_ray_tracing_pipeline` on
qualifying Vulkan devices without native RT. Unmodified applications use their
ordinary AS objects, descriptors, SPIR-V shaders, SBTs, and `vkCmdTraceRays*`
commands. GPU compute builds AS. Ray traversal and intersection run in a
layer-generated compute program that schedules the six RT stages; no native RT
extension, shader instruction, or driver-private BVH interface is required
downstream.

`VK_KHR_ray_query` is deliberately excluded:

- The extension is not advertised by the layer.
- `VkPhysicalDeviceRayQueryFeaturesKHR` is consumed and stripped; a shader
  module containing `OpTypeRayQueryKHR`/`OpRayQuery*KHR` is rejected with
  `VK_ERROR_FEATURE_NOT_PRESENT` instead of being lowered.
- Ray Query only test fixtures, CTS selections and evidence records were
  removed. Reintroducing Ray Query requires a new goal and a new lowering path.

The AS representation, AS object model and software traversal remain shared
infrastructure for the RT pipeline runtime; "pipeline only" does not mean the
AS backend is optional.

## Architecture decisions

| Module | Selected route |
| --- | --- |
| API and objects | One layer-owned AS/device/descriptor model for RT pipelines |
| GPU AS build | Mesa leaf, Morton sort, LBVH, PLOC and HPLOC algorithms through a public-Vulkan host adapter |
| Build intermediate | Original Mesa build IR; internal construction interface only |
| Traversal representation | Portable BVH4 with FP32 child bounds |
| Traversal | Layer-owned software implementation inspired by the RADV state machine |
| RT Pipeline | Typed SPIR-V continuation/state-machine lowering into a compute program; the traversal GLSL is compiled into each stage |
| OMM | Separate optional workstream; not part of this goal |

## Device and compatibility contract

Target 64-bit Vulkan 1.2+ devices with compute, buffer device address, scalar
block layout, int8/16/64, storage8/16, Vulkan device-scope memory model,
storage-buffer dynamic/NonUniform array indexing, variable storage-buffer
pointers, compute subgroup basic/ballot and Vulkan 1.2 timestamps or core
events. Feature/profile truth for AS/RTP and its mandatory limits is supplied
by one software profile; unsupported optional RT features stay false and are
rejected when requested.

Newer optional RT mechanisms may be excluded individually through documented
capability masks; the layer must not advertise what it does not implement, and
must not forward virtual AS/pipeline objects into native RT entry points.

## Required behavior

### Layer and API translation

- Virtualize extension enumeration, feature/property queries, device creation,
  dispatch lookup, and required dependency behavior consistently.
- Strip emulated AS/RTP extension names, feature structures, buffer usages,
  stages, access masks and capabilities before calling a driver that lacks
  them. Strip `VK_KHR_ray_query` as well; it is not implemented.
- Preserve unrelated features, `pNext` chains, allocators, command state,
  threading and object lifetimes.
- Provide mandatory deferred-host-operation behavior where permitted; optional
  host AS commands stay gated by their real support.

### Acceleration structures

- Implement all mandatory device-side AS operations: size/address queries,
  BUILD/UPDATE, clone/compact, property queries, serialization/deserialization,
  compatibility, and their required lifecycle/synchronization behavior.
- Support indexed/nonindexed triangles, AABBs, multi-geometry BLASes, instance
  arrays/pointer arrays, transforms, inactive primitives/instances, masks,
  flags, original IDs, and BLAS reuse.
- GPU CONVERT/CLONE/COMPACT, semantic UPDATE (topology-preserving refit),
  serialization with public-BLAS relocation, and bounded property-query
  publication are implemented on the shared BVH4 runtime.
- Large-AS arithmetic and chunked dispatch must avoid an accidental 4 GiB
  byte-offset ceiling.

### RT pipeline compiler and runtime

- Preserve entry points, specialization constants, types, resources and legal
  shader instructions through a typed transformation of validated SPIR-V.
- Generate resumable states for `TraceRay`, `ReportIntersection`,
  `ExecuteCallable`, and transitive suspending helper calls; implement Phi-edge
  liveness, typed frames, spills/rematerialization, return values and CFG
  restoration.
- Implement all six base RT stages, legal payload/attribute/callable layouts,
  hit kinds/builtins, nested traces, callable dispatch and 3D launch IDs.
- Preserve SBT indexing, group handles, record data, pipeline libraries,
  stack queries and pipeline caches.
- Do not unroll recursion to a fixed depth.

### Synchronization and execution

Translate RT/AS dependencies to the actual compute/transfer/shader accesses,
including barriers, events, semaphores, ownership, query availability and
cross-queue use. Preserve application state around internal work, command
buffer reuse, secondary buffers and concurrent recording.

## Milestones and exit criteria

| Milestone | Deliverable and exit criterion |
| --- | --- |
| M0: prove the architecture | Capability/limit audit; full-layout resource transport; no-native-RT device creation/dispatch; typed continuation stress proof; versioned BVH4/addressing contract |
| M1: shared AS backend | Mesa build variants to BVH4, GPU encode/refit, structural and numerical checks, original-ID/lifetime correctness |
| M2: complete RT pipeline | Typed continuation compiler and integrated scheduler; all six stages, callable/procedural/recursive shaders, payloads/SBT/stack coverage |
| M3: API and integration | Mandatory AS copy/query/serialization, device/descriptor/pipeline translation, synchronization and lifecycle matrix |
| M4: qualification | Applicable AS/RTP CTS and regression matrix, physical no-RT GPU runs, supported platforms, truthful capability reporting |

Milestones are complete only after their exit criteria are demonstrated. A CPU
encoder, one working shader pattern, or a passing CTS slice does not complete a
milestone.

## Verification surface

- External CTS from the user's `D:\Codes\VK-GL-CTS` checkout: run applicable
  `dEQP-VK.ray_tracing_pipeline` and `dEQP-VK.acceleration_structure` subsets.
  `dEQP-VK.ray_query.*` cases are out of scope.
- No-native-RT execution: a boundary that rejects native AS/RTP features and
  operations, plus a physical device without RT. Lavapipe alone is not
  cross-vendor qualification.
- Builder/runtime: graph structure, leaf coverage, bounds, original IDs,
  transforms for empty/singleton/duplicate/inactive inputs, unbalanced trees,
  multiple geometries, >4 GiB arithmetic, update and copy.
- Compiler/RTP: stripped debug names, aggregates, Phi loops, pointer aliases,
  opaque values across suspension, helper TraceRay/ReportIntersection,
  callable and nested traces, hit side effects, null groups, GPU-generated SBT.
- API/resources: every mandatory entry point, query modes, serialization
  relocation, allocation failure, allocator callbacks, command-state
  restoration, synchronization and resource limits.
- Keep reproducible commands and per-revision results; retain failures, skips
  and unrun cases with reasons. A skipped case is not a pass.

## Iteration and stop policy

Choose the next work item from demonstrated gaps and failed proof gates.
Record the result, update the status page, and continue toward the full goal.
Do not merge the development branch or change support claims to close a
milestone. If a proof fails, investigate alternatives within the public-Vulkan
contract; if none remains, report the blocker with its evidence.

## References

- [Current status](ray_tracing_fallback_status.md)
- [Baseline review](ray_tracing_fallback_review.md)
- [D3D12 source provenance](ray_tracing_fallback_port.md)
- [Vulkan ray traversal specification](https://docs.vulkan.org/spec/latest/chapters/raytraversal.html)
- [SPV_KHR_ray_tracing](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_ray_tracing.html)
- [Pinned Mesa builder](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/vulkan/runtime/vk_acceleration_structure.c)
