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
| [Portable BVH4 components](../layers/common/rt_fallback/README.md) | Checked reference/CPU adapter; generic BDA hierarchy encoder; direct default Mesa GPU IR adaptation/record emission with compact active leaves, full flags and explicit TLAS relocation; actual LBVH build-to-encode in one command buffer | Four-child GPU collapse, broader builder variants/large construction IDs, numerical qualification, runtime traversal integration, refit |
| Typed continuation frames | Checked 64-bit planning; emitted typed scalar/aggregate codecs and per-suspension SSA spill/restore; executed 64 independent frames with persistent local aliases, runtime Phi loops and delayed return injection | Source CFG/state compilation, argument/return and invocation-storage ABI, pointer parameter/Phi provenance, specialization, descriptor snapshots and production integration |
| RQ host translation | Consume AS/RQ device requirements; translate resource/synchronization masks; lifetime-owned AS address cells with allocator/flush/failure handling and enabled-queue-family sharing; private SSBO indexing/variable-pointer prerequisites | Complete emulation capability/limit reporting on no-RT ICDs, deferred operations, buffer-requirement query paths, full-layout descriptor transport and shared-layer object model |
| RQ compiler compatibility | Actual per-query AS roots from scalar/array descriptors and integer conversions; helper/value/Phi flow; direct/grouped NonUniform propagation; preserve application workgroup/specialization metadata and remove native RT declarations | Full resource-limit transparency, shared BVH4 traversal, complete opcode/stage coverage |
| RTP API compatibility | Preserve registry-known device pNext structures including Vulkan 1.4; inject storage16 prerequisites without duplicate aggregates; accept empty dynamic-state lists; generate absolute multi-config-aware manifests | Typed compiler/runtime, arbitrary payloads, complete pipeline options/mandatory API and shared backend integration |
| Base RQ SBT offset | Lower opcode 6021; select candidate/committed instance; return only the low 24 offset bits; retain helper in the library | Shared BVH4 traversal and broader getter coverage remain open |
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
The [latest CTS report](rt_cts_as_values_2026-10-26.md) and
[per-case evidence](rt_cts_as_values_2026-10-26.json) expand RQ to 21 exact cases.
The [earlier 20-case report](rt_cts_results_2026-10-26.md) is retained as history.

The latest RQ batch and matching native controls pass **21/21 each with Khronos
validation enabled**, including dynamic AS indexing, AS function arguments,
multiple queries in compute/fragment stages, three dynamic query-array cases
and shared-memory consistency. No API validation errors, skips, crashes or
timeouts occurred in that batch. These tests consume the production binary-IR
prototype, not the separate shared BVH4 encoder. RTP remains **3 pass, 3 fail**
out of six, with the legacy location-zero vec4 payload restriction unresolved.
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
arity, 32-bit construction IDs, numerical qualification, refit and production
consumer integration remain open.

RQ offline checks now pass **9/9**. The production traversal helper, CPU AS
fixture and software GPU BUILD checks also pass. Shared registry initialization
uses one platform once object across translation units; 32-thread/two-TU stress
passes 20 fresh Release and 20 ASan processes. The unsafe baseline produces 32
initializations in the delayed negative control. POSIX/TSAN execution was not
available, and this defect is not linked to the historical CTS startup crash.
AS-cell spies cover creation before backing binding, descriptor update before
publication, eight-thread publication, noncoherent flush/retry, allocator and
failure cleanup, and immutable deduplicated queue-family sharing. Unused AS
recording metadata was removed to avoid races between command-buffer recordings.

The typed frame emitter passes SPIRV-Tools and actual RTX 3080 execution with
Khronos validation: 64 frames, all restored aggregate fields observable, 257
runtime Phi iterations, local aliases, late ReportIntersection result, untouched
uninitialized storage/padding and stable completed states. It emits type codecs
and each suspension's SSA spill/restore functions; it does not lower the source
CFG or implement a production continuation runtime. Its backend explicitly
rejects frames >=4 GiB and codecs above 4096 aggregate elements while the planner
retains checked 64-bit arithmetic. The full RTP repository regression passes
**12/12**, including this new component; actual RTP CTS failures remain open.

## Next work

Run M0 proof work in parallel with shared-backend construction:

1. Complete one capability/device/object translation path for genuinely no-RT
   drivers. Device stripping and buffer/barrier/submit/event/timestamp/render-pass
   masks are now tested; truthful no-RT base discovery/limits, deferred operations
   and buffer-requirement/external-buffer queries remain open.
2. Extend value-preserving RQ lowering beyond the now-passing AS array/helper
   CTS slice, including the shared BVH4 representation and complete opcode/stage
   semantics. Address-cell transport still consumes an SSBO per AS descriptor
   and an allocation per AS; it does not prove full application resource budgets.
3. Integrate emitted typed codecs into source CFG/call/state compilation and
   the runtime. Address invocation-private storage, pointer parameters/Phi
   provenance, specialization and argument/return ABI before claiming the full
   typed-continuation M0 stress proof or resolving the RTP payload CTS failures.
4. Integrate the now-tested direct Mesa GPU encoder into layer-owned AS objects
   and both consumers, implement four-child collapse/refit, and qualify numerical
   cases and large construction IDs. Same-command-buffer builder/encoder tests
   do not complete production M1 integration. Wire PLOC/HPLOC only with complete
   host contracts.
5. Expand the [external CTS matrix](rt_cts_as_values_2026-10-26.md), including
   new supported configurations and validation. Retain historical failures and
   investigate the old unexplained startup crash if it recurs. Do not erase
   failures or count skips as passes.
6. Qualify the complete capability profile and threading/queue-family behavior
   on physical no-RT GPUs and POSIX. SSBO dynamic/nonuniform indexing and
   variablePointersStorageBuffer are now explicit private prerequisites;
   legacy Vulkan 1.0 extension-dependency coverage remains unqualified.

Follow the goal's [iteration and stop policy](ray_tracing_fallback_goal.md#iteration-and-stop-policy).
