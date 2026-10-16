<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Complete D3D12 fallback port: contract and current gaps

The target is the behavior and implementation of Microsoft's D3D12RaytracingFallback,
including its separate DXC continuation compiler. A restricted executable sample
is not an acceptable substitute. **The complete port is not implemented yet.**
The existing layer is a development harness, not the finished fallback.

## Source baseline

The exact source paths, revisions and Git blob IDs are recorded in
`layers/raytracing/upstream/manifest.json`. The reference copies include the
original GPU algorithms, traversal, runtime, compiler passes and D3D12 unit tests.
Their presence does not mean they execute on Vulkan. Run `upstream/verify.py` to
check source identity. The original Windows/D3D12 tests have not been executed in
this environment.

The source files that define the target are:

| Area | Original implementation | Required Vulkan work |
| --- | --- | --- |
| Program construction | `UberShaderRayTracingProgram.cpp`, `DxilShaderPatcher.cpp` | Typed SPIR-V ingestion, stage/group linking, descriptor and shader-record accesses |
| Continuations | DXC `LiveValues.cpp`, `StateFunctionTransform.cpp`, `Reducibility.cpp` | Phi-edge-aware liveness, control-flow normalization, typed spill/rematerialization, continuation states and return frames |
| Runtime | DXC `runtime/runtime.c`, `DxrFallbackCompiler.cpp` | Downward-growing word stack, separate pending/committed attributes, payload offsets, nested ray saves, scheduler and stack sizing |
| Traversal | `TraverseFunction.hlsli`, `TraverseShader.hlsli`, `RayTracingHelper.hlsli` | Triangle and procedural traversal, candidate/committed hits, any-hit suspension/resumption, ignore/terminate, culling and SBT selection |
| Inputs | `LoadPrimitivesPass.cpp`, `LoadInstancesPass.cpp` and shaders | Multiple geometries, R16/R32/nonindexed inputs, transforms, AABBs, instance arrays/pointers, metadata |
| Build | `GpuBVH2Builder.cpp`, scene/Morton/bitonic/rearrange/split passes | Scene reduction, Morton codes, bitonic sort, primitive/metadata reorder, Karras LBVH; scratch partitioning and pass barriers |
| Optimization | `TreeletReorder.hlsl`, AABB passes | BLAS treelet optimization selected by build flags, node AABB fitting and leaf combining |
| Update | Sort cache, parent indices, `ComputeAABBs.hlsli` | Preserve topology and sorted input mapping; reload geometry/instances and refit AABBs |
| Copy and queries | `GpuBvh2Copy.cpp`, `GetBVHCompactedSize.hlsl` | Equivalent copy/postbuild-size behavior, correct Vulkan query and object lifetime |
| Host API | Device/state-object/command-list classes | Vulkan loader dispatch, object/descriptor/SBT mapping, features/properties, command-buffer state and synchronization |

Changing bitonic to radix sort, replacing LBVH/treelets with the prototype's
input-order tree, or unrolling TraceRay to a chosen fixed depth would change the
implementation target. Necessary API/IR/addressing differences must be documented
and verified, not silently used to discard upstream features.

## Implemented port components

- `continuations.cpp` parses validated SPIR-V using the binary grammar, finds the
  selected entry's reachable call graph, propagates suspension through helpers,
  and computes SSA liveness at TraceRay, ReportIntersection and suspending calls.
  Phi inputs are uses on predecessor edges, including loop back edges. The
  analysis also recognizes ExecuteCallable, without claiming callable execution.
  It does not depend on debug names. The prototype linker uses this analysis for
  nested-call diagnostics.
- `continuation_runtime.glsl` adapts the upstream runtime helpers: word stack and
  frames, trace frames, payload offsets, pending and committed hit state, attribute
  buffer swapping, closest-hit/miss save/restore, builtin helpers and scheduler.
  GLSL uses `inout` parameters and a per-invocation stack array instead of C
  pointers. Matrix storage remains twelve scalars. Signedness conversions are
  explicit. Generated code must provide the runtime's three `rewrite_*` hooks.
- Compiler tests check straight-line and branch/loop Phi liveness, transitive
  suspending calls, parameter preservation, exclusion of unborn call results,
  stripped debug names, validation failures, ReportIntersection, callable
  recognition and terminal any-hit operations. The GLSL runtime is compiled and
  SPIRV-Tools-validated.
- A Lavapipe compute test runs the runtime helpers and two scheduler states. It
  checks stack operations, candidate versus committed hits, attribute swaps,
  nested ray state saves/restores, matrices, any-hit state, return sentinel and
  dispatch bounds. This is a component test with hand-written states, **not**
  evidence that arbitrary recursive RT shaders are lowered or executable.

## Work still required for completion

SSA live IDs are not a frame layout. Function-local storage and alias lifetimes,
opaque handles, pointer provenance/rematerialization, payload/attribute types and
alignment must be lowered correctly. The compiler still needs continuation
splitting, state-function code generation, typed linking, stack sizing and
scheduler integration. The generated runtime is not connected to the prototype's
RT execution path.

Any-hit/procedural traversal, all original GPU build passes, refit, copy/query
paths, shader-record lowering and full API/state compatibility remain unported.
The existing serial builder and direct-call linker must be replaced. Retaining
prototype integration tests provides regression coverage but does not reduce the
required scope. No full-port or arbitrary-game compatibility claim is justified.

## Upstream limitations versus Vulkan obligations

The pinned developer guide explicitly states: no callable shaders; DispatchRays
Depth ignored; one bound AS per dispatch; no HLSL subobjects; limited local root
descriptors; app-provided stack size ignored; compaction flags ignored; COMPACT
copies behave as normal copies. It also lists payload and procedural-opacity
bugs. These are baseline limitations, not missing upstream algorithms to invent.

Vulkan extension conformance is a separate, stricter contract. In particular,
mandatory Vulkan callable and shader semantics cannot be declared implemented
because a D3D12 feature-parity port is finished. Optional features must report
truthful support, and every advertised mandatory command must behave correctly.
Document intentional bug fixes and Vulkan-only additions separately; do not
silently reproduce an upstream bug that violates an advertised Vulkan guarantee.

## Acceptance

Completion requires all target rows above to execute through the layer, original
behavioral tests ported to Vulkan and passing, shader/stack/traversal differential
coverage, no native RT instructions in generated compute code, clean validation
of lowered API calls, and qualified GPU runs. Imported files, isolated runtime
tests, or a successful rendered triangle cannot meet this acceptance criterion.
