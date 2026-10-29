<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Software RT Fallback: Implementation Status

Checkpoint: 2026-10-29 (Asia/Hong_Kong). The [implementation goal](ray_tracing_fallback_goal.md)
is in progress. **The complete AS/RQ/RTP fallback is not implemented.**
The [review](ray_tracing_fallback_review.md) records the pre-change `f18c40d3`
baseline; goal alignment was committed as `82251ec3`.

This file is the repository progress checkpoint. GitHub Issues are disabled in
this repository. It records project work; it does not activate a hosted execution
service or imply that an external scheduler is running.

The user clarified on 2026-10-28 that some newer optional-extension
incompatibilities are acceptable and that all tests need not pass. Subsequent
work prioritizes the mandatory AS/RQ/RTP baseline; known newer-feature exclusions
must be explicit. CTS/regression outcomes remain categorized evidence, with no
requirement to make unrelated optional suites all pass. See the goal's
[compatibility and test scope](ray_tracing_fallback_goal.md#compatibility-and-test-scope).
The latest slice is the [ShaderModule ownership and header qualification checkpoint](rt_shader_ownership_2026-10-29.md).
It resolves the ShaderModule P1 from the
[ub-mode branch review and HPLOC/memory-model checkpoint](rt_ub_review_2026-10-29.md).
The [physical RT interfaces and production Mesa PLOC checkpoint](rt_pointer_interfaces_ploc_2026-10-29.md)
is its preceding baseline.
The [Mesa pipeline comparison and mandatory boundary checkpoint](rt_mesa_pipeline_2026-10-29.md)
is its committed baseline.
The preceding [Event-based AS query checkpoint](rt_query_events_2026-10-28.md)
remains the query-publication baseline.

Source is consolidated under `layers/raytracing`: production C/C++ and ABI
headers in `src`, all production GPU code in `shaders`, fixtures/oracles in
`tests`, documentation and pinned references in `docs`, and build support in
`cmake`. The root has one canonical CMake entry and README. The separate `vkrt_layer`
tree and superseded HANYANG executable path have been removed. The canonical
layer uses one AS/RQ/RTP backend; pinned upstream references remain intact.
The [directory consolidation record](rt_directory_layout_2026-10-27.md) records
the final layout, passing build/local checks and the 16-case external CTS recheck.

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
- [x] Connect canonical typed RTP to the shared AS backend, with invocation-private frames and public GPU SBT/Trace execution.
- [x] Implement GPU AS serialization/compatibility/relocation and semantic UPDATE through full reconstruction.
- [x] Expand external canonical RTP CTS to 20 passing cases, preserving intermediate compiler failures and matching native controls.
- [x] Cache native RTP stack variants with a private context-push layout and preserve maintenance6/application compute state.
- [x] Translate explicit RT buffer requirement/external queries consistently and execute the public descriptor budget boundary.
- [x] Preserve traditional descriptor-backed opaque identities across suspension and compare descriptor layout definitions.
- [x] Execute bounded public AS property-query publication and real CTS compaction/serialization paths.
- [x] Back numeric module-Private state per logical RT invocation and execute the additional control-flow CTS slice.
- [x] Preserve shader-object binary interfaces, linked peers, successful batch prefixes and legal SPIR-V 1.6 RQ inputs, with expanded external CTS.
- [x] Pool sparse AS pages and shared descriptor values, apply the descriptor budget to discovery/device creation, and emit real four-child GPU BVH4 nodes.
- [x] Refit shared BVH4 topology on GPU UPDATE, retaining recoverable degenerate instances.
- [x] Preserve logical StorageBuffer pointer Phi/Select/helper values across suspension with typed route/selector snapshots.
- [x] Link nested RT pipeline libraries through the shared typed compute runtime.
- [x] Execute typed Function/Private pointer memory and supported PtrAccessChain loops across suspension.
- [x] Add SDK-derived owned command capture and independently tested logical-stage planning components.
- [x] Execute fresh primary/secondary AS-query recordings with independent payloads, overlapping submissions and completion retirement.
- [x] Replace production RTP PrivateArray frames with tiled, execution-owned BDA arenas and generic primary/secondary replay.
- [x] Retire obsolete destination PrivateArray code, retain source Private semantics, and compile the private backend as genuine SPIR-V 1.4.
- [x] Unify software discovery/device/resource planning and verify masked public selection/execution with bounded optional compatibility gates.
- [x] Publish execution-owned AS queries without native timestamps on the Event-backed singleton compute path, with explicit fallback qualification and injected-capability/public/CTS evidence.
- [x] Preserve GLSL450 source physical pointer identities and pointer-bearing Function/Private aggregates across typed continuations, with nested public Callable mutation oracles.
- [x] Account for public/private native memory allocations in one transaction ledger, with real 4096-allocation boundary controls and free/retry evidence.
- [x] Allow independent AS query operations inside known compute-insensitive native query scopes, preserving conservative compute-statistic admission.
- [x] Preserve physical-pointer-bearing payload/callable/hit-attribute interfaces through the actual six-stage typed scheduler, including recursive type metadata and nested pointer mutation.
- [x] Select and execute production Mesa PLOC on eligible devices, encode its GPU IR into shared BVH4, and retain LBVH/refit policy fallbacks.
- [x] Integrate range-arrival HPLOC and 32/64-bit Morton variants, with shared-BVH4 structure/ray checks and artificial XYZ dispatch qualification.
- [x] Support the tested GLSL450/Vulkan source memory-model paths, mixed native/RT entries and conservative slab intervals; repair reproduced registry and discovery contracts.
- [x] Review branch-wide code/build/tooling contracts under ub-mode, retaining independent controls, tool limits and remaining quality debt.
- [x] Unify source/native ShaderModule identities, verify public allocator/rollback/consumer behavior, and preserve newer compatible feature prefixes using matching header/registry metadata.
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
| [Portable BVH4 components](../layers/raytracing/docs/BVH4.md) | Checked reference/CPU adapter; generic BDA hierarchy encoder; direct Mesa GPU IR adaptation/record emission with compact active leaves, full flags and explicit TLAS relocation; actual LBVH/eligible PLOC/HPLOC with 32/64-bit keys, two-to-four-child GPU collapse and topology-preserving UPDATE/refit; conservative slab regression | Builder performance, large construction IDs, complete numerical/transform qualification and full object/resource qualification |
| Typed continuation frames | Typed CFG/continuations; execution-owned BDA backing; builtin/push/record state; specialization, linked six-stage scheduler/shared traversal; traditional descriptor-backed opaque provenance; logical StorageBuffer pointer snapshots, supported PtrAccessChain loops, typed Function/Private pointer memory, source physical address identities/aggregate bridges and cross-stage interfaces; tested GLSL450/Vulkan source memory-model conversion and mixed-entry selection | Remaining pointer/memory/execution-mode variants, multi-root/NonUniform execution qualification, large aggregates, conflicting per-stage FP modes and complete resource qualification |
| RQ host translation | Consume AS/RQ device requirements; translate resource/synchronization masks and explicit RT buffer queries; lifetime-owned AS address cells with allocator/flush/failure handling and enabled-queue-family sharing; private SSBO indexing/variable-pointer prerequisites | Complete emulation capability/limit reporting on no-RT ICDs, future unknown buffer chains, full-layout descriptor transport and private allocation budgets |
| AS operations and host dependency | GPU CLONE/COMPACT, serialization/UUID/BLAS relocation, topology-preserving UPDATE/refit; public property pools with per-execution primary/secondary replay, payload/native completion acquisition and reuse/reset/free publication; synchronous deferred operations; owned command-capture and logical-stage planning components | Native logical-stage executor, remaining replay schemas and native-query interaction, true shrinking compaction, refit performance and full resource budgets |
| RQ compiler compatibility | Actual per-query AS roots from scalar/array descriptors and integer conversions; helper/value/Phi flow; direct/grouped NonUniform propagation; preserve application workgroup/specialization metadata and remove native RT declarations | Full resource-limit transparency, complete opcode/stage coverage and full shared traversal qualification |
| RTP API compatibility | Canonical shader/pipeline/SBT/stack/Trace path; cached native variants/private context push; public layouts with retained private copies; copied descriptor definitions; nested pipeline libraries; maintenance6 and compute pipeline/shader-object state restoration | Remaining pipeline options and optional library-handle contracts, remaining descriptor disturbance/interoperability paths, complete mandatory API/resource qualification |
| Base RQ SBT offset | Lower opcode 6021; select candidate/committed instance; return only the low 24 offset bits; shared-BVH4 helper and CTS evidence | Complete required getter/stage and shared-consumer qualification remain open |
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
cmake -S layers/raytracing -DVKRT_BVH4_COMPONENTS_ONLY=ON -B build-bvh4 \
  -DCMAKE_BUILD_TYPE=Debug -DVEL_RT_BVH4_SANITIZERS=ON
cmake --build build-bvh4 --parallel
ctest --test-dir build-bvh4 --output-on-failure

cmake -S layers/raytracing -B build-rq -G Ninja \
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
cmake -S layers/raytracing -DVKRT_BVH4_COMPONENTS_ONLY=ON -B build-rt-bvh4 -G Ninja `
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
  -DCMAKE_BUILD_TYPE=Debug -DVEL_RT_BVH4_GPU_TESTS=ON
cmake --build build-rt-bvh4 --parallel 4
$env:VEL_RT_BVH4_VALIDATION='1'
ctest --test-dir build-rt-bvh4 --output-on-failure

# Release CRT avoids Debug-CRT/ASan startup incompatibility in this installation.
# The LLVM ASan runtime DLL must be discoverable when running instrumented EXEs.
$env:PATH='C:\Program Files\LLVM\lib\clang\20\lib\windows;' + $env:PATH
cmake -S layers/raytracing -DVKRT_BVH4_COMPONENTS_ONLY=ON -B build-rt-bvh4-sanitize-release -G Ninja `
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
BLAS references into physical runtime records. The [address ABI](../layers/raytracing/docs/AS_ADDRESS_ABI.md)
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

The subsequent [canonical typed RTP checkpoint](rt_cts_typed_runtime_2026-10-26.md)
does connect production pipeline creation, SBT selection and all six stages to
the shared Mesa/BVH4 AS backend. Continuation frames use native invocation-private
storage sized from actual metadata, with no shared writable frame pool. Public
API GPU traces, lifecycle/state restoration, Report/Any-hit/Callable/nested Trace
and serialization/UPDATE proofs pass validation. Standalone RTP passes **17/17**;
canonical integration passes **15/15 offline** and **7/7 selected GPU tests**.

The new external matrix measures **20/20 RTP** and **54/56 RQ/AS**, with no skips,
crashes or timeouts. Matching RTP native controls pass **20/20**. The two public AS query-pool failures remain explicit; intermediate 18/20 and
19/20 aggregate/image-atomic/root failures are retained with their fixes. Original 0/6 and 4/6 canonical RTP runs are
retained, including the now-fixed image type/unused-attribute/UPDATE defects.
No complete extension-support or physical no-RT claim follows from this slice.

The subsequent [cache/resource/expanded CTS checkpoint](rt_cts_cache_resources_2026-10-26.md)
retains full native layout capacity while caching pipelines by stack budget.
It adds consistent buffer requirement and external-buffer translation,
registry-derived allocation-free chain storage, maintenance6 core/KHR commands,
compute shader-object binding restoration, and descriptor-definition identity.
The resource planner remains inactive in global discovery; its public GPU proof
fills AS16, static SSBO1,048,544, dynamic SSBO16, set31/32 and all256 push bytes.

Traditional image/sampler/combined/texel-pointer identities now cross suspension
through independent typed provenance snapshots. The initial real state-spill
matrix measured 39/52; its unchanged native control measured 52/52, and all 13
opaque failures passed the focused external recheck. Linking now repairs
duplicate aggregate type copies, translates ShaderCall memory scopes while
preserving semantics, and validates physical scalar-layout shader records.
The complete merged-build RTP reruns now measure **98 pass, 0 fail, 2
NotSupported** across 100 selected cases, matching native controls; no validation
diagnostics, crashes or timeouts were found. The RQ/AS rerun retains **54 pass,
2 fail**, with all 56 native controls passing. The new report records the
complete expanded reruns and intermediate failures. Complete M0–M5 and public
AS query-pool publication remain open.

The following [query/control-flow checkpoint](rt_query_controlflow_2026-10-26.md)
implements bounded public property-query publication. The former compaction and
serialization CTS failures pass 2/2; an expanded AS operation slice passes 24/24
with 24/24 native controls. The complete 56-case first query rerun instead
measures 53 passes and three early process crashes. All three pass against the
identical snapshot on recheck; the original crashes remain unresolved and are
not converted into a clean 56/56 run. Pre-Vulkan WGL initialization is a source
hypothesis under investigation, rather than a proven cause.

Another 46-case external control-flow slice improves from 36 passes/10 failures
to 46/46 with matching native controls. Typed source invocation frames now own
numeric module-Private variables, including initializer/reset/resume behavior.
Base derivative creation and disable-optimization flags have public software,
native and validation evidence. Shader-object source envelopes now rebuild for
the destination device and preserve allocator/handle-alias lifetimes. Valid
BINARY imports with absent/different layouts still fail two external CTS cases;
the original compilation interface must be preserved before full qualification.

After [source consolidation](rt_source_consolidation_2026-10-26.md), one expanded
226-case external RT run records **224 pass, 2 NotSupported, zero failures,
crashes or timeouts**. It includes all 56 RQ cases, the prior 100 RTP cases,
46 control-flow cases and 24 AS-operation cases. Both skips match native controls.
Earlier startup crashes remain unresolved historical evidence. Fresh builds
pass 24 offline, 29 GPU and five independent BVH4 checks. The final rebuilt DLL
passes its three RT smoke cases and two binary-interface controls, retaining the
two known binary-interface failures. Simultaneous queries, generic stage
scheduling and the complete M0–M5 criteria remain open.

The subsequent [shader-object transport checkpoint](rt_shader_objects_2026-10-26.md)
fixes the null/different-layout binary imports and two additional creation-batch
failures. Proven ordinary groups retain native binaries; RQ groups preserve the
complete original linked group and owned descriptor/sampler/push definitions,
rebuild against the destination resolver, then perform the public native BINARY
creation. Device-owned definitions survive original object destruction, and
independent successful batch prefixes survive later failures. Source robustness,
enabled features and specialized integer constraints are checked before replay.
Legal SPIR-V 1.6 RQ modules now retain their version and literal workgroup size
through the matching linker/validator environment.

The external binary/create matrix improves from **125/129 to 129/129**, with
**129/129** native controls. Eight additional functional CTS cases pass in both
software and native modes; the FORMAT3 RQ/RTP regression passes **59/59**. The
final separate-build fixtures pass eight actual GPU executions across both
validation-layer orders with synchronization checks, including cross-device RQ,
SPIR-V 1.6, linked cross-profile imports and immutable sampler values after source
device destruction. Main integration passes **25 offline** and **14 selected
host/GPU** tests. The [machine evidence](rt_shader_binary_v3_2026-10-26.json)
retains exact snapshots and historical failures. These are distinct tested
snapshots, not one aggregate conformance run.

Final review also fixes descriptor/pipeline-layout native-alias retirement and
guards unordered linked batches whose failure would otherwise produce invalid
native topology. The exact final DLL passes **25/25 offline**, **2/2 binary GPU**
and a fresh **15/15 external CTS** recheck. Its immediately preceding alias/recipe
snapshot also passes **7/7 selected GPU** checks, including RTP derivatives and
maintenance6. Both snapshots and their scopes are preserved in the same evidence
record; the complete earlier 129-case matrix is not relabeled as this final DLL.

Full binary compatibility remains open when an original RQ peer/interface needs
features absent on the destination. Unknown extension chains, complete source
environment constraints and private resource budgets still require qualification.
The [context-free address candidate](rt_context_free_address_design.md) records
one potential way to eliminate resolver-specialized replay; it is not adopted.
The complete M0-M5 goal remains active.

The following resource/construction work after `c88decf` is currently in
validation. The [resource profile](rt_resource_profile_2026-10-26.md) now applies
to legacy/2/KHR properties, base-feature/extension masking and virtual RT device
creation. Ordinary no-RT properties retain native truth, and complete emulated
base RT advertisement is still gated. AS four descriptor caps are 16 even though
the AS update-after-bind feature remains false: SDK357's ALL-layout rules also
apply to ordinary AS bindings. The original 03572/03574 validation failure and
its fix are retained.

Sparse registry pages/header and same-offset shared descriptor values now use
DEVICE/Instance-owned coherent arenas. They retain distinct physical-BDA and
descriptor-offset alignment rules and release empty grown arenas. Actual public
BUILD/RQ execution over 1024 simultaneously live offsets, alias retirement and
hole reuse passes both software and native controls. Its software validation
runs each allocate one 64-KiB AS-address arena. The complete RTP resource-boundary
fixture now consumes the advertised public budget directly and passes both VVL
orders and the native control. Six positive VVL/synchronization processes have
zero diagnostics; an existing intentionally invalid undersized-build probe is
retained in a separate historical log. Full private allocation-count minima
and all other private resource budgets remain open.

The [production GPU collapse](rt_bvh4_collapse_2026-10-26.md) uses pointer-jump
depth parity, dense retained-node IDs and correct four-child/parent records.
Five independent component tests and the first six public GPU BUILD/control
tests pass. The main offline suite passes 25/25. UPDATE remains reconstruction,
and full numerical, refit, large source-ID and no-RT qualification remain open.

The exact prior 226-case RT selection completed against its immutable DLL
snapshot in `build-rt-cts-evidence/resource-collapse-rt-first`: **220 pass,
2 NotSupported, 4 crashes**. Three early exits are `0xc0000008` with empty
stdout/QPA; the separate `data_spill.trace_ray.v3uint64` internal touch-watchdog
termination reports `0xffffffff`. These are separate observations, not one proven
cause. The older startup investigation remains open. All raw failures remain
retained; this is not a clean 226-case run.

## 2026-10-28 refit, pointer and pipeline-library checkpoint

The [integrated checkpoint](rt_refit_pointer_libraries_2026-10-28.md) adds
production topology-preserving GPU refit and preserves zero-mask/empty-BLAS
instances so legal updates can make them visible again. Original geometry and
primitive IDs map refreshed source leaves to the retained runtime hierarchy;
Device-scope acquire/release publication joins child bounds without polling.

Logical StorageBuffer pointer Phi/Select and inlined helper values now cross
suspension through route tags and typed selectors, including parallel loop Phi
assignment. Generated memory operations execute inside the selected route's
branch. The prior valid native-pointer-Phi sample's NVIDIA compute-pipeline
exception and its raw shader/debugger evidence remain retained. OpPtrAccessChain,
mutable pointer memory, multiple descriptor-root execution and real descriptor
array NonUniform qualification remain open.

Nested RT pipeline libraries, including empty intermediate libraries, now
retain typed definitions and link through the shared compute backend. Shader
and group lists preserve implicit append order and repeated references.
Maintenance5 flags2 is consumed at full width and replaces the legacy flags.
The first merged CTS attempt records 30 pass/2 fail for the missing flags2 path;
its focused recheck passes 2/2 and the complete rebuilt selection passes 32/32.

All 32 external cases also pass the test-only downstream native-RT rejection
boundary, with completed zero-rejection audits and no validation diagnostics.
Matching native controls pass 20/20 pipeline-library and 12/12 UPDATE cases.
The boundary retains driver discovery for CTS selection; masked discovery still
exposes the unfinished no-RT advertisement path. It is not physical no-RT or
cross-vendor qualification. The prior 226-case crashes remain unresolved.

Main/parent builds, 30 offline and five fresh component checks pass. These
results close tested construction/compiler/API components, not any entire M0-M5
milestone. Query execution replay, logical-stage scheduling, full resource and
capability contracts, additional builder variants/large IDs, and physical no-RT
qualification remain required.

## 2026-10-28 command capture and pointer-memory checkpoint

The [new checkpoint](rt_query_replay_pointer_memory_2026-10-28.md) adds numeric
identity records for supported pointer-valued Function/Private cells and
aggregates, same-stride PtrAccessChain loop advancement, and dynamic array frame
codecs. Source and generated SPIR-V validation remain mandatory. Source-validated
same-pointee stride-changing loops are explicitly rejected before fixed-point
analysis; volatile pointer memory, OpCopyMemory and additional views remain open.

New GPU checks execute both source storage classes over an 8193-cell pointer
table with dynamic indices, alias writes, aggregate suspension and signed loop
advancement. These executions use the standalone StorageBuffer frame harness;
new BDA/PrivateArray variants and the 65537-element array-codec case have
compilation/validation evidence only.

SDK-derived owned command capture is connected to command entrypoint/lifecycle
hooks, with retained named-layout definitions and explicit unsupported schemas.
The separately tested logical-stage planner preserves future-submission gates
and includes a progress counterexample against broadening TRANSFER waits to
physical compute. Neither component enables per-execution native/query replay
or a native logical-stage executor. Simultaneous-query and unsafe-wait rejection
remain. Inline layout retention, ambiguous public native aliases and unwrapped
platform/beta commands need complete admission/virtualization before replay
can be enabled. M0-M5 and the complete fallback goal remain unfinished.

Final offline checks pass 34/34. All 11 selected GPU checks have passing
observations across an initial 10 pass/1 stale-property-oracle failure and its
separate corrected 1/1 recheck. The frozen external library/UPDATE selection
passes 32/32 with zero-rejection guard audits and no validation diagnostics.
Canonical/parent RT targets pass; the unrelated parent default-build Windows
macro failures and every intermediate failure are recorded in the checkpoint.

## 2026-10-28 execution-owned query checkpoint

The [execution replay checkpoint](rt_execution_replay_2026-10-28.md) connects
fresh primary/secondary query recordings, loader dispatch initialization,
independent payloads/markers, transactional failure handling and private-fence
retirement. Public GPU tests cover repeated occurrences, overlapping pending
submissions, GPU-changing inputs, archived results and prefix availability
before a blocked tail. Both VVL orders also pass with synchronization validation
explicitly enabled. Teardown alone drains hidden pending observations; normal
submission and query collection do not use queue/device idle waits.

Per-queue admission and epoch-bounded idle clearing preserve wait history across
concurrent submission/publication and pipeline-initialization failure. Query
allocations now use the shared pool with separate physical-address and native
copy-offset alignment. Thirteen formatter layouts pass over three GPU updates.
The SDK357 journal has wrappers for all 317 known commands, including checked
ABI-only unsupported forwarding; inline/template and unknown future schemas
still need complete qualification.

Typed pointer memory adds no-memory-operands CopyMemory and correct array-stride
restoration through unadorned views. All six pointer forms execute on GPU with
independent oracles. Their new BDA/PrivateArray variants and large array codec
retain compilation/validation-only evidence.

The final build passes 37/37 offline checks and the parent RT target. Six typed
GPU checks pass on unchanged component executables. A frozen pre-teardown DLL
passes 50/50 guarded external CTS; the final teardown DLL separately passes
13/13 relevant CTS. All guard audits finish with zero rejections and no scanned
validation diagnostics. New native controls pass 18/18; earlier 20-library and
12-UPDATE native controls remain separate. The final DLL SHA-256 is
`88af24d458f9812c06afef8fb26332309270984037e4c96b6a9a9bd22b5879dd`.

The historical watchdog target passes both new software snapshots, without
establishing its original cause. The old 220-pass/2-NotSupported/4-crash matrix
and first signal-order test failure remain retained. Scoped signal timing is
now recorded as an observation, not a portable early-progress requirement.
The native logical-stage executor, complete no-RT profile, production large-AS
limits and M0-M5 qualification remain open.

## 2026-10-28 indexed construction and native acquisition checkpoint

The [new checkpoint](rt_indexed_build_acquire_2026-10-28.md) removes the
production construction IR's 4-GiB byte-offset gate and X-only dispatch gate.
Indexed v3 IDs use 64-bit address decoding, with v2 decoder compatibility and
unchanged public BVH4/serialization formats. Maximum-count host checks,
21 GPU codec reads (15 synthetic wide offsets), a failing truncation mutation,
and public BUILD/RQ under forced `(2,2,64)` dispatch limits provide distinct
component evidence. Three builder/encoder GPU components also pass with their
validation option enabled. Actual giant allocations and complete numerical
qualification remain open.

Query COPY now accepts original narrow native waits through its
ALL_COMMANDS-to-CS acquire bridge. The old rejection/history/epoch machinery,
query-copy metadata walker and query-free observer batches were removed.
Per-execution query resource fences, settled retirement, per-queue admission and
synchronous source-error handling remain. RESET retains the native-transfer
completion edge after COPY. Public native acquisition passes ten cases; final
software acquisition and repeated/overlapping replay pass in both VVL orders
with synchronization validation and zero-rejection guard logs. The GPU
compute-only-bridge mutation still passes two cases on this driver; it is not
a successful negative control. Independent host COPY/RESET scope mutations
fail as expected. Earlier invalid fixtures and rejected-submit diagnostics
remain retained.

The public RT candidate now requires the current query backend's host reset
and buffer-int64-atomic support. Ordinary shader-object eligibility stays
independent. Complete software properties/discovery, query-capable queue and
device-group behavior, private memory budgets and physical no-RT qualification
remain open. Journal metadata is a separately tested capture component; it does
not constitute a native logical-stage executor.

Typed access operands and CopyMemory paths have validation plus two basic BDA
and four enhanced SSBO/BDA GPU observations. The enhanced matrix uses actual
independent buffers and 16-lane NonUniform descriptor arrays. Large PrivateArray
execution still returns DEVICE_LOST; all diagnostic variants and a native GLSL
control fail. Device-fault capture reports invalid writes for both the lowered
and native controls, without proving the underlying cause. At that checkpoint,
production RTP still used PrivateArray and scalable arena integration remained
a delivery gate. The following checkpoint supplies a production replacement;
the historical failures remain failures.

The final integrated build passes 38/38 offline checks and the parent RT target.
The frozen pre-profile/cleanup DLL passes 50/50 guarded external CTS. The final
DLL separately passes 13/13 relevant CTS. These snapshots, raw failures and
qualification boundaries are recorded in the checkpoint and its manifest.
No complete M0-M5 milestone or full-goal completion is claimed.

## 2026-10-28 execution-owned RTP arena checkpoint

The [production arena checkpoint](rtp_execution_arenas_2026-10-28.md) replaces
RTP destination PrivateArray storage with device-local-preferred BDA arena
leases. Native compilation is cached once per pipeline, independently of the
current dynamic stack budget. A checked tile planner retains the complete
per-invocation budget, shrinks allocation widths on OOM and preserves original
three-dimensional launch IDs through a private 16-byte push. The 64-MiB target
is an allocation strategy, not a public stack cap. Context storage is O(stage
count); HOST/context visibility and tile reuse cover the real buffer slices.

Generic per-execution replay now includes RTP independently of optional query
initialization. Source recording does not allocate mutable execution storage.
Each primary/secondary occurrence owns its leases through completion retirement.
Pool metadata belongs to generic execution, and forget-before-native-destroy
prevents a same-handle creation from losing its new metadata. Independent O0/O2
host checks and a failing old-order mutation cover that race.

A new public fixture keeps a 131,076-byte source array live across two nested
callables and checks its complete digest, output values, multiplicity and
padding. Its exact public stack equation is 133,312 bytes on this pipeline.
The preceding frozen DLL returns DEVICE_LOST at the first caller-fence wait;
the production BDA replacement passes all seven execution/reuse phases.
Source Private/Function value and pointer semantics remain supported. Pending
GPU submissions alone do not prove overlapping execution or exclude a shared
per-recording arena: distinct-lease host oracles provide separate ownership
evidence.

The final build and 40/40 offline checks pass. A 24-run GPU matrix passes in both
VVL orders, with ordinary no-native-RT guards, synchronization validation and
zero scanned validation diagnostics. It includes public large arrays, API and
ordinary compute state, shader objects, maintenance6, libraries, resource
boundaries, query replay/acquisition and typed BDA/pointer components. Forced
`(2,2,64)` dispatch tests separately pass Submit2 and disabled optimization;
each observes 182 dispatches with maximum `(2,1,1)`, including the short tail.
These artificial limits are not an ordinary mandatory-limit proof. After a
fixture-only SBT-address alignment correction, 4/4 final public GPU runs pass,
including both VVL orders and both forced-tile variants. The same production DLL
passes **50/50 external CTS** from `D:/Codes/VK-GL-CTS`, with 125,113 complete
guard boundary checks, zero rejections and no scanned validation diagnostics.
The new checkpoint/manifest bind the DLL, CTS revision and complete raw evidence;
these are separate observations from the preceding checkpoint's snapshots.

The observed large-frame production blocker now has a tested replacement.
Complete resource qualification remains open: native buffer/allocation size
preflight and compute push-descriptor/descriptor-buffer state restoration are
known source gaps without GPU reproducers in this checkpoint. Destination
PrivateArray diagnostic code is now unused by production and can be retired
without deleting source Private semantics or historical failure evidence.

## 2026-10-28 software discovery and compatibility checkpoint

One software feature/property/resource profile now drives AS/RQ/RTP discovery
and device creation without native RT capability truth. The profile supplies
builder-supported AS vertex formats, pipeline-library enumeration, public
entry points for disabled optional commands, genuine private SPIR-V 1.4
modules and Vulkan 1.1 backend extension aliases. Destination PrivateArray
and its unused diagnostics are retired while source Private semantics remain.

Native pool buffer/allocation size preflight, owned push/template replay,
global descriptor-buffer restoration and six-class debug/PrivateData object
metadata are implemented. Seven newer optional object interfaces have explicit
discovery/feature/device gates rather than new virtual adapters. Ordinary
native-only EXT_BDA device creation is preserved. These changes follow the
user's narrower optional compatibility scope.

The [checkpoint](rt_software_discovery_2026-10-28.md) and
[manifest](rt_software_discovery_2026-10-28.json) separate the preliminary
masked 12-run GPU/50-case CTS snapshot from final-source validation, retain
the actual descriptor-buffer VUID failure and its repair, and state the
remaining qualifications. The software discovery candidate has public masked
execution evidence; complete queue/group/protected qualification and physical
no-RT validation remain open. No M0-M5 milestone is marked complete.
Unified offline checks passed 46/46, selected GPU runs 30/30, and the final
formatted DLL's external CTS slice 13/13 with 27302 native-boundary checks and
zero rejections/validation markers. The preliminary 50-case observation is a
different frozen DLL and overlaps the thirteen cases; it is not 63 unique cases.

## 2026-10-28 Event-based query checkpoint

Mandatory AS queries no longer use private timestamps on the normal core
Event-backed path. Each replay operation owns a fresh host-queryable native
event and coherent payload; the CS-to-HOST barrier and real CS sentinel remain.
SET permits harvesting, while completion fences/proofs still govern retirement.
Native errors and known-complete-but-RESET observations fail explicitly and
remain visible through void cleanup paths.

The shared profile selects Event without private hostQueryReset/alias injection.
Only portability devices without native Event support select the timestamp
fallback, with actual host reset and complete usable-compute-family timestamp
qualification. Missing required native entry points no longer silently disable
queries. Software RT multi-device groups now fail explicitly before native
creation; their implementation remains open.

The [checkpoint](rt_query_events_2026-10-28.md) and
[manifest](rt_query_events_2026-10-28.json) retain all preliminary observations
and exact final sources/DLL. Offline checks passed 47/47; final public GPU runs
passed 11/11 with three native controls, plus an expected old-DLL failure.
Thirteen external CTS cases passed with native RT fully masked and timestamps
injected as unavailable: 27282 boundary checks, zero rejections/timestamp
writes/validation markers, and 20 actual Event signals/status observations.
This is not physical timestamp-free/no-RT qualification.

The obsolete query-pool GPU fixture is removed after extracting its valid
command-buffer/pool-reset result-lifetime checks into the new public fixture.
Replay/acquire tests retain stronger independent result, padding, occurrence,
overlap, host-reader and narrow-wait coverage. Its incorrect compacted-size
growth and old rejection assumptions, missing transfer dependency and unsafe
host-gate structure remain documented with raw evidence rather than retained
as current regressions. Full M0-M5 remain open.

## Mesa pipeline and mandatory boundaries: October 29

The [implementation and evidence record](rt_mesa_pipeline_2026-10-29.md) pins
the Mesa RTP comparison and records source physical pointers, typed aggregate
bridges and matrix access ancestry, unified public/private allocation-count
admission, and independent compute-insensitive native-query scopes.
The final standalone/parent RT builds, 49 offline checks, 9 public GPU
observations and 24 external AS/RQ/RTP CTS cases pass. The CTS run uses masked
native RT discovery and timestamp-zero injection, with 32,875 checked calls,
zero native-RT rejections, timestamp writes or validation markers. Native and
previous-DLL controls remain separately bound to their original binaries.

These are component results at that baseline. Physical-pointer-bearing cross-stage
interfaces are implemented in the subsequent checkpoint below. Source
VulkanMemoryModel, full resource delivery, compute-statistic/native-stage
semantics, group/protected support and complete qualification remain open.
No M0-M5 exit is newly claimed and the development branch is not merged.

## Physical RT interfaces and production PLOC: October 29

The [integration checkpoint](rt_pointer_interfaces_ploc_2026-10-29.md) extends
the actual typed interface copy plan with cycle-aware physical-pointer type
equivalence. The public six-stage fixture retains real address identities across
TraceRay, ReportIntersection and nested Callable activations, including true
aliases and committed-attribute snapshots. It also connects Mesa PLOC to the
public builder and the existing shared BVH4 encoder, with explicit device/count
eligibility and LBVH/refit fallbacks. The PLOC correctness path uses one persistent
workgroup and a subgroup-independent local scan; performance and HPLOC remain open.

The original native physical-interface control crashes in pipeline creation,
including without validation or the software layer. It remains an unresolved,
unqualified native observation; it is separate from successful software oracles
and native builder controls. Final result counts and binary/source bindings are
recorded in the integration checkpoint and its JSON: 49 offline checks, six
positive public GPU runs, and 16/16 external software CTS plus 16/16 matching
native CTS pass. The software CTS has masked native RT discovery and zero
timestamp injection, with 16,116 checked calls and zero rejected boundaries,
timestamp writes or validation markers. No full milestone closes.

## Next work

Execution-owned BDA RTP arenas, generic replay and the software discovery
candidate have public observations. Next work prioritizes mandatory queue and
query qualification, complete resource budgets and remaining compiler/build
semantics. Newer optional consumers can remain explicitly unsupported; complete
optional interoperability and an all-green test suite are not requirements.
The following are exit checks, not implemented-feature claims.

| Priority | Deliverable | Evidence needed before closing it |
| --- | --- | --- |
| Next object qualification | Complete other public object namespaces and consumers | ShaderModule ownership is unified and its public lifetime/forwarding/rollback controls pass. Qualify mixed native/software Pipeline and PipelineLayout identities and the remaining resource/interoperability paths; the supplemental Pipeline reproduction has no execution evidence |
| 1 | Complete mandatory profile/resource qualification | Qualify protected execution, multi-device groups and pool-specific native query scopes; account for private/native resource budgets without reducing the mandatory public contract |
| Parallel M4 | Complete logical-stage synchronization semantics | Source semaphore/barrier/event/ownership scopes, native-query interactions and qualified queues/groups. Query COPY uses the proved native bridge. CPU schedules and early-signal timing alone establish neither a correctness failure nor a requirement for a general queue worker |
| Parallel M0/M1 | Complete no-RT resource and construction contract | Extend the shared software candidate profile to complete resource budgets and queue qualification; qualify production indexed construction and XYZ dispatch, then physical no-RT execution |
| Parallel compiler | Remaining typed compiler memory/resource semantics | Access operands and source memory-model rules, multiple real descriptor roots and NonUniform arrays, large values and execution modes, with independent GPU oracles in the production BDA transport and applicable component transports |
| Replay integration | Remaining mandatory command schemas and native-query interactions | Mixed native scopes, full state preservation and simultaneous public execution qualification; optional transports/consumers may keep documented restrictions |
| Final qualification | Complete mandatory behavior and platform matrix | Requirement-to-evidence mapping, broader AS/RQ/RTP CTS, targeted original failures, builder/count/numerical cases, cross-vendor physical no-RT and POSIX runs |

The preceding large-PrivateArray production failure has a BDA replacement;
its old native GLSL and diagnostic failures remain retained. AS/RTP/native
compute share a physical stage while source scopes differ. Complete mapping
still requires proof, but no mandatory forward-progress counterexample currently
establishes that a queue worker is the only solution. The query acquire bridge
closes its specific narrow-wait gap. Passing the execution-only native-RT guard
does not establish no-RT discovery, because that mode retains native discovery
for CTS selection. The new mask-version-1 mode separately proves masked
selection/execution on this GPU; it still does not qualify physical no-RT GPUs.

The October 29 checkpoint adds actual GLSL450 source BDA pointer identities
and aggregate bridges, a unified public/private native allocation ledger, and
compute-insensitive native-query classification. These close their demonstrated
counterexamples without reducing public memory limits or permitting compute
statistics to count hidden query dispatches. They do not close complete resource
delivery or native-query virtualization. Physical-pointer-bearing cross-stage
metadata now has a production implementation and public GPU oracle. The next
concrete compiler step is source memory-model and execution-mode semantics;
the subsequent ub-mode checkpoint implements and executes a bounded source
VulkanMemoryModel/ShaderCall path. Remaining memory operands and execution modes
still require independent qualification.
Matrix access paths
require their dedicated structural proof in addition to SDK SPIR-V validation.

The [no-RT qualification audit](rt_no_rt_qualification_gates_2026-10-28.md)
identifies discovery/property gaps and qualification gates at its audited
baseline. Indexed construction, mandatory-query feature prerequisites and the
shared software profile supersede those particular source findings. Complete
queue/resource and M0/M1 qualification remain open.

Detailed remaining work, grouped by subsystem rather than priority:

1. Complete application resource budgets for the now-shared AS resolver and
   RQ/RTP object/address model. Aligned suballocation values and GENERIC
   same-buffer differences are now implemented and tested. Complete one
   capability/device/object translation path for genuinely no-RT
   drivers. Device stripping and buffer/barrier/submit/event/timestamp/render-pass
   masks and synchronous deferred operations are now tested; the software base
   profile exists, with full substrate qualification still open. Known
   buffer-requirement/external-buffer query
   paths now have host and actual public GPU evidence.
2. Extend value-preserving RQ lowering beyond the now-passing AS array/helper
   CTS slice and the now-integrated shared BVH4 representation to complete
   opcode/stage semantics and full resource budgets. A bounded fast traversal
   path can be added ahead of the proven constant-space parent-walk path.
   Address-cell transport still consumes an SSBO per AS descriptor. Native
   storage is now pooled and same-offset aliases share a value cell; complete
   application/private allocation budgets still require proof.
3. Expand external CTS beyond the passing typed RTP selection. Image atomics,
   BDA root classification and resource/value aggregate types are now fixed;
   traditional descriptor-backed opaque values now cross suspension. Qualify the
   new logical StorageBuffer pointer Phi/Select/helper route with
   multiple descriptor roots and real descriptor-array NonUniform execution
   beyond the passing SSBO/BDA matrix; the production BDA replacement and
   destination PrivateArray retirement are now complete component changes.
   Complete remaining pointer arithmetic/memory variants, mutable opaque encodings
   and the full argument/return ABI. Complete mandatory descriptor disturbance
   and pipeline option/API coverage before claiming the full typed-continuation
   M0 proof. Cached native pipelines now
   use a separate internal context-push layout with public state replay.
   Shader-object binary imports now preserve original descriptor/sampler/push
   interfaces and pass the null/different-layout CTS cases. Restrict claims to
   supported optional peers and explicitly exclude unsupported combinations;
   qualify unknown extension chains and the compiler environment before claiming
   general binary compatibility.
4. Extend the now-shared RQ/RTP builder, BVH4 traversal and AS object model.
   Four-child GPU collapse and topology-preserving refit execute in production.
   The production IR v3/XYZ changes remove the old 4-GiB byte-offset
   and X-only dispatch gates. Complete integration, actual large-allocation
   qualification where resources permit, additional UPDATE/numerical cases and
   large construction IDs; synthetic address tests alone do not close these.
   Eligible PLOC now has a complete host contract and public GPU evidence.
   HPLOC now has checked range initialization, 64-bit MSB handling, workgroup
   collectives and release/acquire range publication, with 32/64-bit and XYZ
   public GPU regressions. Qualify larger counts, transformed/extreme geometry
   and builder throughput separately from this correctness path. M1 remains incomplete.
5. Complete public AS queries beyond the now-tested execution-owned route. Its
   actual GPU properties, repeated/overlapping replay and native completion
   acquisition execute selected external CTS. Complete native synchronization
   scopes, remaining command schemas, native query interaction and queue/device
   qualification. The Event-backed query implementation now handles timestamp-zero
   compute queues; only its explicitly qualified no-Event portability fallback
   requires timestamps. Multi-device/protected combinations remain unsupported
   and need further implementation/profile qualification.
   Generic reset/copy queue flags do not alone establish a legal AS-pool case
   on graphics/video-only families; query-type family restrictions also apply.
   Preserve source wait scopes rather than imposing whole-submit waits. The
   historical startup failures remain distinct from the newer native
   physical-interface pipeline-creation crash. Investigate each with its exact
   binaries while retaining original failures and recheck evidence. Do not
   count skips as passes.
6. Qualify the complete capability profile and threading/queue-family behavior
   on physical no-RT GPUs and POSIX. SSBO dynamic/nonuniform indexing and
   variablePointersStorageBuffer and compatible coherent resolver memory are
   now explicit private prerequisites. Active builder subgroup/float controls,
   workgroup bounds and native Vulkan 1.2 are checked. Vulkan 1.1 application
   aliases have a shared admission path and public evidence; applications below
   Vulkan 1.1 are outside the current software candidate profile.

Follow the goal's [iteration and stop policy](ray_tracing_fallback_goal.md#iteration-and-stop-policy).
