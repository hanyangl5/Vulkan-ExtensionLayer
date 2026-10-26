<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# External RTP control flow and module-Private state

The additional exact external CTS selection in
[`rt_cts_controlflow_cases.txt`](rt_cts_controlflow_cases.txt) contains 46 cases,
all present in CTS mustpass and absent from the previous 100-case RTP selection.
The same list was used for the canonical layer and native control. Every result
is retained, including the initial failures.

| Selection | Cases | What the source exercises |
| --- | ---: | --- |
| Complex control flow | 30 | Ten shapes, each with Callable/raygen, TraceRay/closest-hit and ReportIntersection/intersection: if, loop, switch, double-call, sparse double-call, three nested-loop forms, helper call, nested helper calls |
| Nonuniform arguments | 4 | SSBO-supplied TraceRay arguments, multiple ray types and miss causes |
| AS helper arguments | 1 | Explicit SPIR-V helper parameters containing both an AS descriptor pointer and a bare AS value |
| Resource pointers | 3 | Additional combined/sampler descriptor pointers live across real RT suspension |
| Pipeline barriers | 8 | SSBO/image resource dependencies between RT and ordinary compute, fragment and transfer commands |

The complex-control-flow source uses a bounded 4 by 4 launch. Nested helpers
retain arrays of 14 and 256 elements. The intersection variants declare `v0`
as hit attribute storage and `v1`, `v2`, `v3` as ordinary module-Private `uvec2`
variables. All ten ReportIntersection variants initially failed with
`Private storage requires invocation backing`; all other selected cases passed.
Native passed all 46.

## Implemented storage contract

The typed state compiler now gives reachable numeric/composite module-Private
variables separate typed storage in each logical source invocation's existing
frame. It uses the same checked scalar-packed layouts as Function storage.
Offsets, total frame size, stack charging and BDA/PrivateArray bounds include
these variables. Native source module-Private variables are not emitted as
shared scheduler state.

Constant initializers execute in source entry state zero, which cannot be a
branch target, exactly once for each logical activation. Resume states do not
repeat initialization. Different invocations, including recursive activations
of the same stage, name different frames; reusing a completed frame starts a
new activation and runs initialization again. Variables without an initializer
receive no compiler-generated initialization. Legal undefined-value stores
remain undefined; no deterministic value is substituted for them.

The generic continuation planner still rejects mutable module-Private storage
by default. Only the executable state compiler opts into the explicit backing
contract. Unknown pointer provenance, pointer Phi/Select, opaque Private storage,
volatile persistent memory and frames exceeding checked indexing limits remain
unsupported boundaries. This change does not add those semantics implicitly.

## Results and retained evidence

CTS source revision: `d18e4c68543103371715845cd914c71e5859963e`.
Device: NVIDIA GeForce RTX 3080. Khronos validation was enabled in all runs.
No validation errors, VUIDs or synchronization hazards were printed in the final
canonical or native stdout logs.

| Run | Pass | Fail | NotSupported/crash/timeout/noresult | Evidence directory |
| --- | ---: | ---: | ---: | --- |
| Canonical `b9cf53b` | 36 | 10 | 0 | `build-rt-cts-evidence/rtp-controlflow-b9cf53b` |
| Matching native control | 46 | 0 | 0 | `build-rt-cts-evidence/rtp-controlflow-native` |
| Canonical with Private backing | 46 | 0 | 0 | `build-rt-cts-evidence/rtp-controlflow-private-backing` |

Each directory contains `summary.json`, exact commands, DLL/manifest hashes
where relevant, and individual QPA/stdout logs. The initial canonical run
snapshots the already built parent `build-rq-goal` DLL. The fixed run snapshots
the isolated `build-rq-once` DLL; its declared revision explicitly includes the
concurrent query-publication API worktree changes. The runner copies the DLL
before executing the selection, so subsequent builds cannot change a run.

The source CFG, continuation-analysis and typed RT-request component tests also
pass 3/3 with native Khronos validation and synchronization validation enabled.
The source CFG test now runs both Function and module-Private composite state
over 32 independent GPU invocations, reusing each frame for two complete logical
activations. Its independent oracle checks dynamic loops/branches, numeric Phi
swaps, helper inlining, aliases and repeated suspension/resume. It verifies that
initializers reset state at a new entry and preserve it on every resume, and
that uninitialized bytes are untouched. Additional generated-SPIR-V checks cover
null composite initialization, valid undefined composite stores, matching
BDA/PrivateArray layouts and rejection of an 8 GiB Private array before codec
emission. Component log:
`build-rt-emitter/private-storage-component-validation.log`.

## Coverage limits

Inspection of the actual original QPA SPIR-V found **no `OpPhi` instructions in
these 46 cases**: the GLSL front end represents their control-flow values in
local variables. The AS helper case contains 23 `OpFunctionParameter`
instructions. All ten failing cases contain module-Private variables. The eight
barrier cases use Vulkan pipeline barriers, not shader `OpMemoryBarrier`.
The per-case inventory is retained as
`rtp-controlflow-b9cf53b/source-instruction-inventory.json` under the evidence
directory.

Therefore this slice provides external control-flow, helper-parameter,
module-Private and API-barrier evidence; it does not establish external pointer
Phi coverage. Numeric Phi execution is covered by the component source test.
Pointer Phi, additional pointer parameter shapes, mutable resource encodings,
other shader memory models and complete M3 source coverage remain open. The
46 passing cases are a bounded slice, not a Vulkan conformance claim.

Primary CTS source files inspected at the declared revision:

- `vktRayTracingComplexControlFlowTests.cpp`
- `vktRayTracingNonUniformArgsTests.cpp`
- `vktRayTracingAccelerationStructuresTests.cpp` (function argument assembly)
- `vktRayTracingDataSpillTests.cpp`
- `vktRayTracingBarrierTests.cpp`

They are under
`D:/Codes/VK-GL-CTS/external/vulkancts/modules/vulkan/ray_tracing`; the exact
case names were checked against
`external/vulkancts/mustpass/main/vk-default/ray-tracing-pipeline.txt`.
