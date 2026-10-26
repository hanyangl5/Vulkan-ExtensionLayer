<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Software RT Fallback: Implementation Status

Checkpoint: 2026-10-26 (Asia/Hong_Kong). The [implementation goal](ray_tracing_fallback_goal.md)
is in progress. **The complete AS/RQ/RTP fallback is not implemented.**
The [review](ray_tracing_fallback_review.md) records the pre-change `f18c40d3`
baseline; goal alignment was committed as `82251ec3`.

This file is the repository progress checkpoint. GitHub Issues are disabled in
this repository. It records project work; it does not activate a hosted execution
service or imply that an external scheduler is running.

## Goal progress

- [x] Review the committed implementation and align the complete goal/route.
- [x] Add the initial portable BVH4 ABI, checked planner, CPU reference encoder,
  and structural regression tests.
- [x] Fix base RQ opcode 6021 lowering and candidate/committed SBT-offset lookup.
- [x] Fix procedural AABB opacity precedence and add an executed helper regression.
- [x] Add a validated Mesa scalar-IR CPU adapter retaining IDs/flags/instance relocation.
- [x] Execute the shared BVH4 hierarchy encoder component on Vulkan compute.
- [x] Add checked typed continuation frame/provenance planning and stress fixtures.
- [x] Remove native AS/RT bits from RQ buffer/barrier/queue-submit translation.
- [x] Encode actual Mesa GPU builder output into shared BVH4 in one command buffer.
- [x] Add tested device stripping/discovery masks and event/timestamp/render-pass translation.
- [x] Run an external Vulkan RT CTS smoke matrix with separate native controls and retained failures.
- [x] Preserve actual AS descriptor/BDA values, arrays and helper passage in RQ lowering.
- [x] Emit and execute typed frame/suspension codecs from continuation plans.
- [x] Fix shared object-registry first-use initialization and hidden AS-cell lifetime/sharing.
- [x] Adopt shared BVH4 in the production RQ builder and resumable traverser.
- [x] Verify constant-space four-child/depth140 traversal and expand actual RQ CTS to 40 cases.
- [x] Generate and execute real source CFG ReportIntersection continuations.
- [x] Resolve aligned opaque AS values for legal 16-byte buffer suballocations, including GENERIC aliases/offsets.
- [x] Expand actual RQ CTS to 46 cases with validation and matching native controls.
- [x] Emit actual source TraceRay/Callable requests and execute typed cross-stage payload/callable/attribute backing.
- [x] Implement shared-BVH4 GPU CLONE/COMPACT and expand real CTS to 54 passes with two retained mandatory-operation failures.
- [x] Provide synchronous owned deferred operations without GPU backend prerequisites for unrelated devices.
- [x] Execute descriptor-free BDA system frames and linked typed stages with GPU-only call scheduling in one dispatch.
- [ ] M0: complete all capability, no-native-RT, resource ABI, and continuation proofs.
- [ ] M1: integrate Mesa builders, GPU BVH4 encoding/refit, and shared AS objects.
- [ ] M2: complete Ray Query lowering and state semantics.
- [ ] M3: complete typed RTP continuation compilation and runtime integration.
- [ ] M4: complete mandatory API, resource, synchronization, and lifecycle behavior.
- [ ] M5: qualify the supported configurations and physical no-RT GPUs.

Component checkmarks do not complete M0/M1/M2 or the full goal.

## Code in this checkpoint

| Component | Implemented and tested | Remaining boundary |
| --- | --- | --- |
| [Portable BVH4 components](../layers/common/rt_fallback/README.md) | Checked reference/CPU adapter; generic BDA hierarchy encoder; direct default Mesa GPU IR adaptation/record emission with compact active leaves, full flags and explicit TLAS relocation; actual LBVH build-to-encode in one command buffer | Four-child GPU collapse, broader builder variants/large construction IDs, numerical qualification, RTP/object integration, refit |
| Typed continuation frames | Typed CFG/continuations; persistent invocation backing; BDA library export with system builtin/push/record state; default/provided specialization and one-dispatch GPU stage linkage | Production scheduling/traversal/SBT/API integration, pointer Phi/select/physical-source provenance, conflicting per-stage FP modes and complete descriptor/layout budgets |
| RQ host translation | Consume AS/RQ device requirements; translate resource/synchronization masks; lifetime-owned AS address cells with allocator/flush/failure handling and enabled-queue-family sharing; private SSBO indexing/variable-pointer prerequisites | Complete emulation capability/limit reporting on no-RT ICDs, buffer-requirement query paths, full-layout descriptor transport and shared-layer object model |
| AS copy and host-operation dependency | Execution-time GPU CLONE/COMPACT from shared total_size; destination publication, strided 64-bit copy, state replay; fully owned synchronous deferred operations and no-RT device passthrough | AS property query storage/publication, actual serialization/compatibility, refit and final unified RTP object model |
| RQ compiler compatibility | Actual per-query AS roots from scalar/array descriptors and integer conversions; helper/value/Phi flow; direct/grouped NonUniform propagation; preserve application workgroup/specialization metadata and remove native RT declarations | Full resource-limit transparency, complete opcode/stage coverage and unified RTP traversal |
| RTP API compatibility | Preserve registry-known device pNext structures including Vulkan 1.4; inject storage16 prerequisites without duplicate aggregates; accept empty dynamic-state lists; generate absolute multi-config-aware manifests | Typed compiler/runtime, arbitrary payloads, complete pipeline options/mandatory API and shared backend integration |
| Base RQ SBT offset | Lower opcode 6021; select candidate/committed instance; return only the low 24 offset bits; shared-BVH4 helper and CTS evidence | Complete required getter/stage coverage and unified RTP consumption remain open |
| AABB opacity | Apply the existing ray-over-instance-over-geometry precedence before opacity culling and expose the correct candidate opacity | Does not complete all RQ traversal or no-native-RT layer behavior |

The BVH4 planner covers allocations larger than 4 GiB and the mandatory
`2^29-1` primitive-count size case without allocating those giant buffers.
Structural tests cover 64 generated random trees, an 8192-leaf unbalanced tree,
empty/singleton inputs, parent/leaf coverage, conservative bounds, and rejection
paths. This is a CPU reference, not evidence of GPU builder adoption.

The RQ getter test lowers real base Ray Query instructions and validates generated
SPIR-V, including candidate/committed selectors and signed/unsigned result ABI
coverage. The helper compute test executes the production GLSL functions over
30 valid opacity/instance/cull combinations and distinct SBT offsets with high
flag bits set. It enables no native RT extension, feature, or fallback layer.
It verifies those production helpers; it is not the complete no-native-RT proof.

## Reproduce the targeted checks

With the documented toolchain and Vulkan dependencies installed:

```sh
cmake -S layers/common/rt_fallback -B build-bvh4 \
  -DCMAKE_BUILD_TYPE=Debug -DVEL_RT_BVH4_SANITIZERS=ON
cmake --build build-bvh4 --parallel
ctest --test-dir build-bvh4 --output-on-failure

cmake -S vkrt_layer/layer -B build-rq -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-rq --parallel 2
ctest --test-dir build-rq -L offline --output-on-failure

# Select the intended compute ICD; Lavapipe was used for this checkpoint.
ctest --test-dir build-rq -L no_native_rt --output-on-failure
```

Local results: BVH4 ASan/UBSan checks pass; five RQ offline tests and the compute
helper semantic test pass. LeakSanitizer was disabled locally because this
container's tracing environment prevents it from starting; leak checking is not
claimed. The helper tests also reject the old missing-getter/geometry-only-opacity
implementations in mutation checks.

The existing commit-policy check rejected the normal GitHub merge subject in
historical `f18c40d3`. It now excludes multi-parent merge subjects from authored
subject-style validation while preserving source-format checks and ordinary
commit validation. Focused checks cover merge, valid ordinary, and invalid
ordinary subjects.

CI builds the shared BVH4 reference separately and runs the RQ offline and
no-native-RT helper labels. Physical GPU qualification, the full no-RT device
path, and full fallback/CTS completion remain open.

## First 2026-10-26 checkpoint (68b75c2)

Development continues on `codex/ray-tracing-fallback`. A platform Goal is active
for the full M0-M5 contract; this checkpoint does not complete any full milestone.

- Windows x64, Clang 20.1.3, Ninja, Vulkan SDK 1.4.357.0; physical compute tests
  ran on NVIDIA GeForce RTX 3080, driver 596.49. The shared GPU component enables
  no RT extensions/features or fallback layer. This is not physical no-RT-device
  qualification.
- Shared BVH4 reference, Mesa adapter and GPU hierarchy checks pass **3/3**.
  The same three pass with ASan/UBSan in RelWithDebInfo. GPU checks also pass
  with Khronos validation enabled and zero reported errors. Generated SPIR-V
  validates and contains only Shader, Int64 and PhysicalStorageBufferAddresses
  capabilities. New CI coverage runs the optional GPU component on Lavapipe.
- RQ offline checks pass **5/5**, the production traversal-helper compute test
  **1/1**, the CPU AS fixture **1/1**, and software GPU BUILD checks **1/1**.
  Downstream spies reject leaked native usages/stages/access masks and verify
  unchanged caller arrays/pNext data and downstream failures. Restoring the old
  synchronization implementation causes 34 spy assertions to fail.
- Corrected the pre-existing rejected-build fixture to allocate the queried
  required size minus one; the old fixed 256-byte size could be sufficient for
  a singleton and therefore did not reliably test insufficient storage.
- Typed continuation frame fixtures pass, including independent aggregate
  offsets, arrays/matrices/bools, >4 GiB arithmetic and overflow, mutable aliases,
  opaque AS/image/sampler provenance, hidden dynamic indices, ReportIntersection
  result timing and stripped names. Unsupported pointer provenance, unresolved
  specialized array lengths and reachable module Private storage fail explicitly.
  These are planning tests, not executed transformed RT continuations.
- Fixed a pre-existing GPU treelet partition discrepancy on the 4099-primitive
  RTX 3080 fixture by preventing multiply/add contraction for the internal cost.
  The independent CPU DP oracle is unchanged; generated SPIR-V marks both cost
  operations NoContraction, and builder/continuation checks pass. Failure
  diagnostics now report the scene size, node and both child pairs.
- Fixed Windows source-provenance checks with path-specific `-text` attributes
  for imported D3D12/DXC references. All 188 stored blobs already matched the
  manifest; checkout CRLF conversion was the cause. Exact reference bytes,
  manifest and strict verifier are unchanged. Verification also passes for a
  fresh export with `core.autocrlf=true`, while a deliberate CRLF mutation fails.
- Final RTP standalone regression passes **10/10**, covering u16/u32/nonindexed
  and empty scenes, the existing continuation runtime, builder/refit/copies,
  metadata/TLAS, typed frame planning and fixed-source provenance. This retains
  existing prototype behavior; it does not demonstrate the unimplemented full
  typed RTP compiler or mandatory API coverage.

Reproduce shared component checks on this Windows toolchain:

```powershell
cmake -S layers/common/rt_fallback -B build-rt-bvh4 -G Ninja `
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
  -DCMAKE_BUILD_TYPE=Debug -DVEL_RT_BVH4_GPU_TESTS=ON
cmake --build build-rt-bvh4 --parallel 4
$env:VEL_RT_BVH4_VALIDATION='1'
ctest --test-dir build-rt-bvh4 --output-on-failure

# Release CRT avoids Debug-CRT/ASan startup incompatibility in this installation.
# The LLVM ASan runtime DLL must be discoverable when running instrumented EXEs.
$env:PATH='C:\Program Files\LLVM\lib\clang\20\lib\windows;' + $env:PATH
cmake -S layers/common/rt_fallback -B build-rt-bvh4-sanitize-release -G Ninja `
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVEL_RT_BVH4_SANITIZERS=ON `
  -DVEL_RT_BVH4_GPU_TESTS=ON
cmake --build build-rt-bvh4-sanitize-release --parallel 4
ctest --test-dir build-rt-bvh4-sanitize-release --output-on-failure
```

The current RTP test build uses ignored, build-local copies of this SDK's CMake
package configs with their incorrect installed prefixes repaired; SDK files are
unchanged. Its standalone continuation check is
`ctest --test-dir build-rt-frames -R continuation_analysis --output-on-failure`.
The portable CPU and RQ builds do not require that workaround.

## Current development and external CTS evidence

The user explicitly requires feasible external Vulkan RT CTS execution from
`D:\Codes\VK-GL-CTS`, with truthful failures/skips and retained evidence. This is
now part of the [goal verification contract](ray_tracing_fallback_goal.md#external-cts-delivery-evidence).
The [latest CTS report](rt_cts_bvh4_2026-10-26.md) and
[per-case evidence](rt_cts_bvh4_2026-10-26.json) expand production shared-BVH4 RQ to 40 exact cases.
The [earlier 20-case report](rt_cts_results_2026-10-26.md) is retained as history.

Production RQ now passes **40/40 selected CTS cases with Khronos validation**
across three frozen batches (21 + 16 + 3); matching native controls also pass
40/40. New cases cover object/world transforms and rays, candidate/committed
geometry/barycentric/front-face getters, original IDs, procedural traversal,
transformed instance culling, and all six ordinary graphics/compute stages.
There are no selected skips, crashes, timeouts or API validation errors.
The AS builder writes Mesa IR only into application scratch, encodes the final
shared BVH4 on GPU, and production traversal reads that representation directly.
RTP still uses its legacy runtime/object model; its latest measured six-case
matrix remains **3 pass, 3 fail** with the vec4 payload restriction unresolved.
This remains development smoke coverage on a native-RT GPU, not full CTS or
physical no-RT qualification.

Retained intermediate failures drove three concrete fixes: preserve the AS
operand instead of retargeting one global descriptor; remove native RT SPIR-V
extension declarations after lowering; remove the traversal dummy entry's
WorkgroupSize/SpecId metadata. The latter incorrectly supplied size 1 beside
application workgroups of size 4, 48 or 64. New compile regressions preserve
application metadata and AS operands, including direct/grouped NonUniform and
helper-pointer cases. The earlier empty-TLAS STATUS_INVALID_HANDLE crash did
not recur in the latest matrix; its cause is still unknown and its record is
not replaced by later passes.

Shared BVH4 checks now pass **4/4**, including a direct GPU Mesa-IR encoder with
triangle/AABB/instance record emission, full flags, explicit BLAS relocation and
GPU error status. Eight actual imported LBVH triangle/AABB scenes feed this
encoder in the same command buffer without CPU count/topology readback. The
same four component checks pass with ASan/UBSan and Khronos validation. Binary
arity, 32-bit construction IDs, numerical qualification, refit, RTP integration
and unified AS objects remain open. Encoder errors now mark the runtime header
invalid, without requiring consumers to retain construction scratch. Explicit
identity relocation supports instances that already reference final runtime
BLASes. CPU fixture construction uses the same adapter/reference encoder.

RQ offline checks now pass **11/11**. The production traversal helper, CPU AS
fixture and software GPU BUILD checks also pass. The new production planner is
checked against the shared planner through the mandatory primitive-count size
case and >4 GiB AS sizes, with distinct construction/map/flag/status scratch
regions. Build execution still retains the Mesa construction-ID ceiling.
A GPU regression traverses depth140 through fourth children and ascends the full
chain; another rejects one candidate, confirms a nearer one, prunes a saved
farther cursor and resumes to the closest hit. Shared registry initialization
uses one platform once object across translation units; 32-thread/two-TU stress
passes 20 fresh Release and 20 ASan processes. The unsafe baseline produces 32
initializations in the delayed negative control. POSIX/TSAN execution was not
available, and this defect is not linked to the historical CTS startup crash.
AS-cell spies cover creation before backing binding, descriptor update before
publication, eight-thread publication, noncoherent flush/retry, allocator and
failure cleanup, and immutable deduplicated queue-family sharing. Unused AS
recording metadata was removed to avoid races between command-buffer recordings.

The typed frame emitter and new actual source CFG compiler pass SPIRV-Tools
and RTX3080 execution with Khronos validation. The original 64-frame typed
codec tests remain; the generated source test independently checks 32 shader
invocations and 372 completed reports, dynamic loops, parallel Phi swaps,
branches/switches, inlined suspending helpers, persistent aliases, aggregates,
source SSBO I/O, push constants and launch builtins. Pending/ready controls
prevent premature continuation; frame/dispatch overflow and actual descriptor
capacity guards prevent aliasing or OOB. Source literal strings, FP controls
and copied-resource-pointer NonUniform decorations are preserved.

That source-CFG checkpoint passed **13/13**, with analysis/frame/source-CFG
**3/3 under validation**. The next typed request component now also lowers
TraceRay/ExecuteCallable and retains selected-entry payload, incoming payload,
callable, incoming callable and hit-attribute variables in typed persistent
module backing. Requests capture every AS/flags/mask/SBT/ray argument and a
typed frame reference. AS values are explicit opaque 64-bit values, preserved
across suspension independently of later descriptor updates. Conversion from
uint64/uvec2, descriptor arrays/nonuniform indexing, signed32 Trace operands and
16/64-bit local access-chain indices have dedicated regressions.

Actual compiled RayGeneration, ClosestHit and Callable sources execute 23
TraceRay plus 23 Callable requests; the test transfers typed backing between
synchronized compiled stages and checks late callee writes through caller
aliases while retaining pre-trace SSA snapshots. Separate tests execute
Intersection Report/attribute backing and a non-suspending incoming callable
without a source uint32 type. This component does not generate production
traversal, shader selection, SBT decoding or builtin-state scheduling, and its
descriptor remap still needs production layout/resource integration. Unsupported
provenance/memory operations, specialization and source memory models fail
explicitly. It does not resolve legacy RTP CTS failures or complete M0/M3.
The final standalone RTP suite passes **14/14**, and analysis/frame/source/request
checks pass **4/4 with validation**. Logs/hashes and the exact scope are retained
in the [current report](rt_cts_aligned_as_2026-10-26.md#typed-rtp-request-component).

The observed AS alignment gap is now fixed in the RQ production path. A sparse
seven-level resolver separates 256-aligned public AS values from 4-aligned
physical BVH4 roots. GENERIC objects retain same-buffer offset differences and
same-offset aliases. GetASAddress publishes into a preallocated coherent leaf;
stable calls neither allocate nor flush/write mapped memory. RQ embeds the
per-device context as an ordinary constant; TLAS construction resolves public
BLAS references into physical runtime records. The [address ABI](../layers/common/rt_fallback/AS_ADDRESS_ABI.md)
records the pinned normative contract, publication/lifetime and remaining limits.

An actual public-API GPU build/query regression passes with backing BDA
`0x7332010` and physical BLAS root `0x7332110`, both 16 modulo 256, while its
public GENERIC value is `0x200`. Aliases/offset differences and destruction are
checked; Khronos validation reports zero errors. This is executed AS evidence,
unlike the earlier ordinary-buffer probe. Real CTS now passes **46/46** with
validation: the previous 40 rerun after this change plus six new GENERIC
triangle/AABB combinations. Matching native controls pass all 46 (the original
40 retain their previous controls). See [the new evidence report](rt_cts_aligned_as_2026-10-26.md).
Full application descriptor/allocation budgets, optional capture/replay,
mandatory query/update/serialization and physical no-RT qualification stay open.

The next [AS-copy/BDA-stage checkpoint](rt_cts_copy_pipeline_2026-10-26.md) reruns
all 46 passing cases and adds eight passing CLONE cases. Its complete measured
matrix is **54 pass, 2 fail**; all ten new native operation controls pass.
The retained compaction/serialization failures expose AS property-query pools
being forwarded without native AS enablement. A follow-up interception rejects
these unimplemented pools before native creation: the two cases still fail,
explicitly with FEATURE_NOT_PRESENT and zero validation errors. Unsupported
command stubs now fail recording instead of logging and appearing successful.
GPU COMPACT itself preserves the valid representation/size and passes direct
public API execution; the compaction CTS path still requires property queries.

The unmapped device-local copy fixture covers 4,097 triangles, copying beyond
the first dispatch stride, BLAS/TLAS clone/compact chains, transformed getters
and source-object destruction. Application-facing and downstream validation
pass. Synchronous deferred operations have allocator/concurrency/ASan evidence.
A public API1.0 device with only the Deferred extension and zero enabled features
now creates successfully, exercises all five operations and ordinary buffer/
memory creation with validation in both layer orders. Ordinary devices skip AS
backend initialization, feature injection, BDA resource augmentation and RQ rewrite.

Typed BDA libraries export `void stage(uint64 frameBDA)` and share a pinned
256-byte system prefix. Tests execute all 17 requested RT builtins/transforms,
shader-record reads, source push snapshots without native push ranges, late
completion and any-hit terminal dispositions. Global pool addressing uses checked
64-bit math; per-source-frame layout still has a documented <4-GiB boundary.
The new linker applies specialization and resolves real stage libraries into
one native compute module. One dispatch performs 21 GPU-scheduled callable
invocations with late typed writes, alias/SSA preservation and source side effects;
no host coordinates requests between stages. Default/provided specialization,
stripped source names, malformed interfaces and conflicting FP modes are tested.
The standalone RTP suite passes **16/16**, with the six typed checks **6/6 under
validation**. These components do not complete canonical RTP API/SBT/traversal
integration or prove complete AS-descriptor/multi-stage resource budgets.

## Next work

Run M0 proof work in parallel with shared-backend construction:

1. Complete the public AS resolver's application resource budgets and share its
   object/address model with RTP. Aligned suballocation values and GENERIC
   same-buffer differences are now implemented and tested. Complete one
   capability/device/object translation path for genuinely no-RT
   drivers. Device stripping and buffer/barrier/submit/event/timestamp/render-pass
   masks and synchronous deferred operations are now tested; truthful no-RT base
   discovery/limits and buffer-requirement/external-buffer queries remain open.
2. Extend value-preserving RQ lowering beyond the now-passing AS array/helper
   CTS slice and the now-integrated shared BVH4 representation to complete
   opcode/stage semantics and full resource budgets. A bounded fast traversal
   path can be added ahead of the proven constant-space parent-walk path.
   Address-cell transport still consumes an SSBO per AS descriptor
   and an allocation per AS; it does not prove full application resource budgets.
3. Integrate the typed BDA stage compiler/linker and persistent system/invocation
   backing with production pipeline creation, shader/SBT selection,
   builtins and traversal scheduling. Address invocation-private storage,
   pointer parameters/Phi provenance, specialization and full argument/return
   ABI before claiming the full
   typed-continuation M0 stress proof or resolving the RTP payload CTS failures.
4. Adopt the shared builder, BVH4 traversal and one AS object model in RTP; RQ
   production integration is now tested. Implement four-child GPU collapse/refit
   and qualify numerical cases and large construction IDs. Wire PLOC/HPLOC only
   with complete host contracts. M1 remains incomplete.
5. Implement full AS property queries and serialization to address the two
   measured [external CTS failures](rt_cts_copy_pipeline_2026-10-26.md). Use
   execution-time GPU sizes and per-submission host publication; do not invent
   serialization sizes or rely on unproven coherent-memory polling. Expand
   configurations and validation. Retain historical failures and
   investigate the old unexplained startup crash if it recurs. Do not erase
   failures or count skips as passes.
6. Qualify the complete capability profile and threading/queue-family behavior
   on physical no-RT GPUs and POSIX. SSBO dynamic/nonuniform indexing and
   variablePointersStorageBuffer and compatible coherent resolver memory are
   now explicit private prerequisites;
   legacy Vulkan 1.0 extension-dependency coverage remains unqualified.

Follow the goal's [iteration and stop policy](ray_tracing_fallback_goal.md#iteration-and-stop-policy).
