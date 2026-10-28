<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Software RT Fallback: Baseline and Feasibility Review

Reviewed on 2026-10-25. Source baseline:
`f18c40d316688066834d1334b2c666d1d60b9351` on
`codex/ray-tracing-fallback`. Paused local Mesa-IR migration changes are excluded.
Later fixes do not retroactively change the findings at this baseline.

## Verdict

Proceed with the [full implementation goal](ray_tracing_fallback_goal.md).
The proposed GPU algorithms and explicit continuation state machines can be
expressed using public Vulkan shader/compute mechanisms. This review found no
requirement for an AMD-private AS format, hardware RT instruction, or driver change
in the selected software backend.

This is an engineering feasibility judgment, not proof of a completed transparent
compiler or extension implementation. The largest unproven mechanisms are typed
continuation generation and full application-resource/API virtualization. They are
explicit M0 gates, not details to hide behind a successful BVH demo.

## What actually exists

| Area | Baseline evidence | Required change |
| --- | --- | --- |
| RTP builder | GPU D3D-derived LBVH/treelet kernels, update/copy component tests | Migrate to shared Mesa build core and BVH4 encode/refit |
| RQ builder | GPU leaf/sort/LBVH to its own BVHIR container | Shared object/AS contract; PLOC/HPLOC host wiring and capability variants |
| RTP compiler | SPIRV-Cross/GLSL rewriting; RGEN/MISS/CHS only; location-zero vec4 payload | Typed SPIR-V state splitting, arbitrary layouts, all six stages |
| Continuations | Grammar-aware CFG/liveness analysis and isolated runtime tests | Frame layout, mutable-local lifetime, state emission, runtime integration |
| RQ lowering | Same-module software library calls; actual AS operand discarded | Preserve each AS value, multiple bindings/arrays/provenance |
| Native-RT absence | RTP has partial virtualization; RQ still requires native extension exposure | One complete no-RT discovery/device/format/resource contract |
| API completion | RQ update/copy/query/serialization incomplete; some stubs only log | Implement mandatory behavior and reject unsupported experimental paths |
| Traversal | Separate binary layouts/algorithms with fixed-stack assumptions | Shared BVH4, resumable candidate protocol and proven overflow behavior |

Specific confirmed RQ defects include missing base opcode 6021
(`OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR`) and AABB
opacity classification that ignores combined ray/instance overrides. These can
be fixed independently while the shared-backend migration proceeds.

The RQ instance dispatch lacks extension/features/properties virtualization;
device creation forwards native RT requests; buffer and synchronization mapping
retains unsupported RT bits. Current GPU fixtures request native AS/RQ support.
Their success therefore does not prove operation on an ICD without RT.

The RQ traversal stack silently omits pushes at its capacity. Its original depth
argument concerns its LBVH construction and cannot automatically apply to
PLOC/HPLOC or a BVH4 runtime.

## Mandatory-limit audit

The RTP baseline reports values below Vulkan's required minimums. Reporting a
small value does not make the corresponding mandatory feature conformant.

| Limit | Baseline prototype | Required minimum |
| --- | --- | --- |
| `maxGeometryCount` | 2^20 | 2^24 - 1 |
| `maxInstanceCount` | 2^20 | 2^24 - 1 |
| `maxPrimitiveCount` | 2^20 | 2^29 - 1 |
| AS descriptors per stage/set | 1 | 16 |
| `maxRayDispatchInvocationCount` | 2^24 | 2^30 |
| `maxRayHitAttributeSize` | 8 bytes | 32 bytes |

Source: [Vulkan required limits](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html).
The implementation must audit the complete pinned table, not only these examples.
Do not fix this merely by increasing returned numbers: storage, dispatch,
descriptors, compiler, and stack behavior must support the claimed values.

## Route review

### AS build and traversal: viable with a host adaptation layer

The imported Mesa PLOC and HPLOC algorithms are substantial existing kernels, not
missing clustering algorithms. The executable RQ path currently creates five
LBVH-related pipelines; PLOC is compiled but not dispatched, and HPLOC is not part
of that build. Complete their scratch planning, initialization, specialization,
and ordinary-Vulkan dispatch integration.

Do not link the entire Mesa runtime or copy its private dispatch helpers. Its
build IR remains a common intermediate; encode the result to a layer-owned BVH4.
RADV's software box/triangle tests and traversal control flow are useful references,
but AMD address packing, hardware descriptors/stacks, and NIR intrinsics must be
re-expressed. Validate exact floating-point and memory-order requirements.

### Shader compiler: viable architecture, major implementation outstanding

Adopt a typed SPIR-V state-machine backend using the existing grammar/liveness
work and SPIRV-Tools. D3D's continuation design provides a reference for stack,
pending/committed state, and scheduler behavior; its LLVM/DXIL passes cannot be
linked as a ready-made Vulkan compiler.

SSA live IDs alone do not describe mutable local storage, aliases, or opaque
descriptor values. Prove these across suspension, including helper calls,
Phi loops, ReportIntersection returns, callable shaders, and nested traces.

Mesa NIR shader-call lowering is also a useful reference. A SPIR-V to NIR to
SPIR-V route is not a drop-in solution: shader-call resume operations, resource
conventions, and the scheduler ABI still require owned lowering and validation.

### Transparency: viable candidate mechanisms, proof required

Both existing layers assume resource arrangements that are insufficient for
general applications. Adding an internal descriptor set can fail when legal
application layouts already use every slot. Replacing AS descriptors with SSBOs
also consumes a different physical descriptor budget.

Potential mechanisms include BDA context snapshots, virtualized RT push constants,
or constant-address pipeline variants as a slow semantic fallback. These are
options to prototype, not interchangeable proven solutions. Validate full-layout
cases, state restoration, command-buffer replay, concurrent submissions, and
GPU-generated inputs before selecting the final ABI.

## Proceed conditions

The project can proceed without waiting for OMM or a native driver change. Start
with the shared runtime contract and the compiler/virtualization proof gates;
implement verified baseline bug fixes in parallel. A failed proof should trigger
an architecture correction or precise blocker report, never a silent reduction
of the full functional goal.

See the [goal](ray_tracing_fallback_goal.md) for milestones, acceptance, and the
iteration/stop policy. This review does not claim cross-vendor GPU qualification
or formal Vulkan conformance.
