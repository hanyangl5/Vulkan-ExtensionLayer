<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Ray Query inside RT stages: October 29

Work starts from `e6df5bfee89b53bd8c44d63c41a6235667659603` on
`codex/ray-tracing-fallback`. This is a checkpoint draft, not goal completion.
Current HEAD is `92dbd60f0a241ffb2ad4a12875b71490bbdd1777`, which separately
commits the QueryPool allocator correction. The RT-stage query/compiler changes
described here are working-tree changes unless an evidence snapshot says otherwise.
The complete goal and all M0-M5 exits remain open. This iteration follows new
external CTS failures without changing the mandatory baseline.

The current state-frame candidate has its first real public software GPU PASS:
two live indexed Ray Queries survive TraceRay and produce the expected results.
The seven-case external CTS rerun has five passes and two watchdog terminations.
Final complete offline qualification is running, and the final parent build
has not yet run against this candidate. Historical
passes below belong to their recorded snapshots, not automatically to current
HEAD or the latest compiler source.

## Reproduced mandatory gap

Seven exact external CTS cases exercise committed intersection type in compute
and all six RT shader stages. The baseline software layer passes compute and
fails all six RT cases at pipeline creation with `module-Private storage has no
concrete typed layout`. The matching native control passes 7/7. These are real
CTS results, not inferred coverage from a local compiler test.

Ray Query is legal in any shader stage: see the
[SPIR-V extension](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_ray_query.html)
and [Vulkan ray traversal rules](https://docs.vulkan.org/spec/latest/chapters/raytraversal.html).
This is mandatory RQ/RTP composition, not an excluded newer optional peer.

The public regression in `layers/raytracing/tests/rtp_query_suspend.rgen`
retains two dynamically indexed queries across a real TraceRay. One query has
a candidate intersection; the other has not traversed. After return it checks
candidate distance/type, confirms the hit, completes both queries, and checks
reinitialization. The ordinary public RTP fixture checks SSBO values, replay,
layout lifetime and compute-state restoration. Native execution passes; the
baseline software DLL fails at pipeline creation with the same diagnostic.

## Implementation

The typed pipeline frontend specializes and selects the RT entry, then applies
the existing shared-BVH4 Ray Query lowering before continuation analysis. Query
state becomes ordinary typed data owned by each logical RT invocation. Original
AS descriptor provenance is retained by set/binding and remapped to final source
IDs after linking. The device-owned AS resolver address is embedded before
pipeline metadata is retained.

The lowerer has an explicit intermediate RT mode. It removes RQ instructions
while preserving RT entry points and temporarily reconstructing each TraceRay
AS operand from that operand's translated address. The continuation compiler
then removes the remaining RT operations before native submission. Ordinary
RQ lowering retains its no-native-RT boundary contract.

The traversal contains early returns, which prevented exhaustive helper
inlining. Dead-branch elimination and return merging now precede that existing
inlining pass. A separate valid VulkanMemoryModel input exposed a linker model
conflict with the GLSL450 traversal library. The injected library alone is
upgraded with SPIRV-Tools; source memory operands and shader-call scopes remain
available to the existing typed memory-model translation.

Pipeline metadata now retains the compiled typed stage. Native pipeline
assembly reuses that stage and changes only its export name through
`RenameTypedPipelineStage`, rather than compiling the same source again.
Specialization, AS/context identity, frame layout and interfaces remain those
of the retained stage. The rename regression initially had a fixture bug:
sequential substring replacements changed the newly inserted long export name
when renaming the second stage. The scheduler fixture now emits declarations
and calls directly from each stage's export name. The missing-stub failure was
retained and classified as a fixture defect; the linker was not weakened.

Source cleanup after inlining addresses glslc Function temporaries that the
alias-safe frame planner must otherwise persist. A frozen simple-query probe
went from 3,838 locals and a 25,384-byte frame to 74 locals and 3,224 bytes with
SSA rewrite plus aggressive DCE. Production requests preservation of entry
interfaces. The isolated compile time fell from 23.434 to 1.251 seconds.
Those measurements use the probe's
frozen compiler and CLI cleanup; they are not final GPU timings. Scalar
replacement and longer arbitrary optimization sequences were not adopted.

The cleanup exposed two genuine ABI defects. Private uint32 request/control
types inserted before cleanup were removed as unused, causing empty stages and
RTP bindings to fail. Required compiler ABI scalar types are now inserted after
cleanup. Also, the two-query suspension fixture produced a spill function with
328 parameters, exceeding SPIR-V's 255-argument limit. Spill now accepts a base
and one typed aggregate of saved values; restore returns the corresponding
aggregate. A 300-value frame-emitter regression exercises this boundary without
discarding live values or changing their ordering.

Volatile preservation required corrections at two frontend boundaries:

- In the state compiler, pinned SDK SSA rewrite removes an unused volatile
  Function load, and aggressive DCE removes an unused volatile resource load.
  `HasVolatileAccess` therefore makes these optional cleanup passes ineligible
  for such source modules, preserving the original executable accesses. The
  detector covers explicit memory operands, decorations/groups and volatile
  memory semantics. The three-read aggregate regression again observes all
  three reads; the separate local-load counterexample is retained.
- Native entry selection also used aggressive DCE. Preserving volatile reads
  there left unused RT types after selecting a compute entry, which caused a
  second valid mixed-entry input to fail validation. The selector now removes
  only unused type declarations through parsed ID dependencies, with their
  unused names/decorations, before capability trimming. It does not delete
  executable operations to repair this case. Five valid input variants pass
  the isolated selector probe, including explicit, decorated and grouped
  Volatile, and a live Ray Query with an unselected TraceRay entry.

An isolated alternative that removed only ordinary store-only local variables
retained the simple-query frame reduction. It is evidence from an experiment,
not the production cleanup. It cannot by itself repair the demonstrated SSA
loss of a volatile Function load. The current production rule is the explicit
volatile eligibility check above.

## Evidence and limits

The failed performance premise was that every source block could become a
dispatcher arm while numeric values remained mutable native Function registers
shared by all arms. The suspension fixture produced 31,999 such registers,
including 10,871 with multiple stores, from 10,868 source Phi instructions.
Both the default optimization path and the public disable-optimization path
failed to finish real software pipeline creation within 600 seconds. The SDK
optimizer also independently overflowed IDs and crashed; replay isolated SSA
rewrite producing an invalid 11,073,039-word module. These are retained failures,
not successful compilation or GPU results.

The structural candidate removes that shared-register assignment for
cross-state numeric values. Ordinary edge-live values and Phi destinations use
typed per-invocation frame slots; suspension-live values reuse their existing
slots. Only state-local temporaries retain native Function storage. Delayed
logical pointer selectors are retained recursively, and Edge still reads every
incoming Phi operand before writing any destination. No optimizer pass is
disabled to hide the register explosion: the candidate completed the full SDK
`-O` pipeline. The separate volatile rule protects observable source accesses.
Moving only Phi results would leave ordinary cross-arm values participating in
the same dispatcher-wide SSA merges, so the candidate includes those values.

Logical pointer snapshots and opaque/matrix recipes retain their existing
representations. Physical pointers use existing uint64 codecs. New state slots
append after invocation/private/recipe storage, before final control/stride
bounds; suspension slots retain their old offsets. Parent and recursive shader
activations have distinct runtime frames. State-local registers remain where
internal pointer-routing branches require multiple stores, avoiding an unsafe
global SSA-value cache. More frame memory, initialization and BDA traffic are
the tradeoff; throughput and large-launch memory costs are not yet qualified.

The frozen `state-frame-profile` records the current candidate's CPU result:

| Phase | Result |
| --- | --- |
| Public two-query raygen typed compilation | 26.086 seconds; 984,227 words; 77,268-byte stage frame |
| Runtime assembly/link with optimization disabled for phase isolation | 6.681 seconds; 1,025,990 words |
| Full SDK `spirv-opt -O --scalar-block-layout` on that runtime | Exit 0; 162.75 seconds; 1,684,081 words |
| Corrected Vulkan 1.2 validator invocation | Exit 0 |

The initial validator command incorrectly used `--target-env=vulkan1.2` instead
of the validator's separate option/value spelling. Its exit-1 usage output is
preserved in `validate.log`; `validate-corrected.json` records the successful
command. This invocation error is not a generated-module failure. The profile
contains frozen source, libraries, commands and hashes in `report.json`; its
state compiler hash is
`363f3a4e1e194c48f934f22d73f14e45ac94c3b1886c901317f3ac56c6a86bd7`.

The actual public GPU run in `gpu-state-frame/summary.json` exits 0 in 281.922
seconds. It checks two iterations, two queues with disjoint contexts, six public
Trace calls across two stack budgets, candidate/committed query results and
reinitialization, replay, layout lifetime and compute-state restoration. The
pinned validation layer is confirmed loaded and has zero validation markers.
The guard verifies 702 checks, zero rejected calls and 25 shader definitions
with native RT discovery masked. The tested DLL SHA256 is
`19fb281f6ddc34edcead2534022330f919d8edaee30f9d5ddf535fb9f318df7a`.
This is one real software GPU regression PASS, not physical no-RT hardware
qualification, all-stage CTS success or complete mandatory qualification.

## Historical validation and pending final checks

The evidence retains each intermediate failure rather than replacing it with
the latest pass. Paths in this table are relative to
`build-rq-rt-stages-2026-10-29` unless stated otherwise.

| Snapshot/evidence | Observed result and boundary |
| --- | --- |
| External CTS `rq-rt-stages-before-*` / `rq-rt-stages-native-*` | Software compute passes and six RT stages fail typed layout; native 7/7 passes. These use baseline binaries. |
| `gpu-before` / `gpu-native` / `gpu-native-vmm` | Baseline software public query fixture fails pipeline creation; matching native and VulkanMemoryModel native controls pass. |
| `offline-existing.log` | Historical existing suite 52/52 passes before later cleanup/state-frame changes. |
| `offline-query-initial.log` | Initial new query-only target passes in 361.74 seconds; the mixed fixture alone takes 235.534 seconds and produces 1,088,760 words with a 98,612-byte frame. This is CPU compilation, not GPU execution. |
| `gpu-after` | Initial query integration times out at 601.954 seconds during software pipeline creation. |
| External CTS `rq-rt-stages-initial-*` | Raygen-only rerun ends after 37.937 seconds with watchdog timeout evidence in stdout/QPA; the runner records `crash` because there is no complete result. Neither classification is a pass. |
| `offline-cleanup.log` | 49/53 pass. Failures are the uint32 ABI type regression (state compiler and RTP bindings), the spill parameter limit, and the scheduler rename fixture bug. |
| `gpu-cleanup` | Exit 1; confirms the 328-parameter validation failure on the public fixture. |
| `offline-spill.log` | 51/53 pass after the scalar/spill/fixture repairs. State compiler still loses a volatile read; typed runtime compile exceeds its 120-second timeout. This predates volatile and state-frame repairs. |
| `gpu-spill` | Exit 3221225477 after 501.328 seconds; retained compilation crash, not a GPU pass. |
| `gpu-noopt` | Native disable-optimization control passes; software public disable-optimization path times out at 600.5 seconds. Changing the public optimization flag did not solve the structural problem. |
| Independent `performance-current` | Frozen full SDK optimization crashes with exit 3221225477; pass replay isolates invalid 11,073,039-word SSA output. Pass-removal/local-cleanup experiments and their timeouts remain diagnostic evidence, not an adopted workaround. |
| `parent-spill.log` | Historical parent build succeeds before the later volatile/state-frame candidate; it does not qualify the final source. |
| `standalone-state-frame.log` and `state-frame-profile` | Current standalone build and frozen profile complete; final full offline/parent-build reruns remain pending. |
| External CTS `rq-rt-stages-state-frame-*` | Original-limit rerun completes with five passes: compute, raygen, closest-hit, any-hit and intersection. Miss and callable each terminate after 32.891 seconds. The preserved runner summary labels both `crash`; stdout explicitly reports watchdog touch-interval timeout and QPA contains `#terminateTestCaseResult Timeout`. They are timeout observations, not wrong-value results or passes. |

That CTS run retains its original 30-second touch interval and 180-second total
limit. The native 7/7 control is the earlier baseline control and was not rerun.
A separate bounded miss/callable rerun with an explicit 60-second interval is
planned after correcting the runner's termination classification; no result is
claimed for it here, and it will not replace the original five-of-seven result.

Independent lowerer evidence covers six RT stages plus compute in both GLSL450
and Vulkan memory models, including distinct query/TraceRay AS roots and exact
source barrier scope/semantics. Frozen before/after tests retain the missing RT
capability and incompatible-library-memory-model failures. The lowerer, ID
helper and existing lowerer tests also pass an isolated ASan run: 14 stage/model
cases plus existing cases, exit 0 in 108.375 seconds with no ASan report. SDK
libraries are uninstrumented; documented MSVC container annotations and leak
detection are disabled. This is not full-source sanitizer or leak coverage.

Applicable clang-tidy safety-family runs and raw diagnostics are retained with
their source hashes. They do not automatically cover subsequent state-frame
edits; final applicable analysis must bind the final sources. Earlier broad
lowerer analysis also reports an unchanged padding advisory, so there is no
claim that every analyzer category is warning-free.

Final complete offline tests, separate miss/callable CTS reruns, final parent
build and source-bound final analysis still need to be recorded. The new
delayed-selector regression has source assembly/validation evidence and is
included for compiler/GPU qualification; its independent source review is not
an executed GPU result. Additional VulkanMemoryModel software execution and
other optional peer combinations must be described by their actual results.

Evidence roots are `build-rq-rt-stages-2026-10-29`, external CTS snapshots under
`build-rt-cts-evidence/rq-rt-stages-*`, and independent reviews under
`build-rq-typed-stage-2026-10-29`, `build-rt-stage-rq-lower-2026-10-29` and
`build-native-query-audit-2026-10-29`. The lowerer `report.json`, `perf-report.json`,
`fixture-report.json`, `asan/report.json`, mixed-volatile review and state-frame
profile preserve their own version bindings; evidence from different snapshots
must not be combined into an invented all-green final run.

The parallel resource audit did not establish a new descriptor-budget defect:
the current profile already reserves the 16 AS address cells across stage,
set and aggregate budgets. Mixed resource-class/stage qualification remains
open. The separate QueryPool audit did reproduce a mandatory allocator callback
defect: compatible creation/destruction callbacks A/B send layer-owned frees
to A instead of the current B. Two A/A controls pass and two A/B controls fail.
The fix is now committed separately as `92dbd60`: native and virtual AS pool
destruction use the current compatible callbacks, while creation rollback and
defensive device cleanup retain their appropriate callbacks. Both the frozen
probe and the complete host publication test pass after repair. Root reran the
publication suite successfully in `query-allocator-root.log`. These are actual
host allocation/registry/destruction paths with native query spies, not GPU or
CTS allocator coverage. See `docs/rt_query_allocator_2026-10-29.json` for frozen
failures, source bindings and actions.

## Action log

1. Revalidated clean HEAD, full goal, baseline review, current status and ub-mode.
2. Split independent resource, compiler and query-contract audits; root retained
   sole ownership of shared builds and GPU/CTS execution.
3. Ran the seven-case external software matrix and matching native control;
   retained all six software failures and the compute control.
4. Added an independently checked public query-state suspension oracle. Its
   initial helper-parameter spelling was rejected by GLSL/SPIR-V; the fixture
   was corrected and validated before native/software comparisons. Those
   authoring errors were not treated as production defects.
5. Reproduced the actual typed compiler failure offline, then integrated the
   existing RQ lowerer instead of adding another traversal implementation.
6. Reproduced and fixed the helper-return and memory-model linking failures,
   retaining intermediate inputs, diagnostics and source snapshots.
7. Ran crapper, dryer and mutator on the changed native source. These pinned
   versions support no C++/GLSL input; their no-supported-files results are
   recorded, with no native CRAP, similarity or mutation score claimed.
8. Removed repeat typed-stage compilation by retaining metadata and renaming
   exports; diagnosed and corrected the new scheduler fixture's substring
   collision without changing production stub validation.
9. Profiled surviving Function locals after RQ expansion. Added minimal source
   SSA/dead-store cleanup; retained the before/after CPU metrics and the initial
   public/CTS timeouts that motivated the investigation.
10. Ran the 53-test cleanup suite, reproduced its four failures, moved private
    ABI type injection after cleanup and replaced the unbounded spill parameter
    list with one typed aggregate. Added the 300-value boundary regression.
11. Retained the later 51/53 result and investigated its volatile-read failure
    rather than weakening expected counts. Reproduced both SDK volatile pass
    defects, added eligibility checks, and repaired the distinct native-entry
    leftover-type failure with isolated five-variant evidence.
12. Ran isolated lowerer ASan and applicable actual-flag static analysis,
    recording uninstrumented-library/container-annotation boundaries and raw
    warnings. These results are scoped to their frozen sources.
13. Audited and separately committed QueryPool allocator routing after honest
    A/A controls and A/B failures; reran the complete host publication test.
14. Reproduced public default/disable-optimization failures and replayed the
    SDK optimizer on frozen runtime SPIR-V. Identified dispatcher-wide native
    registers, rather than just source temporary count, as the failed premise.
15. Moved cross-state numeric values to existing/new typed frame slots,
    retaining parallel Phi copies, pointer snapshots and recursive delayed
    selectors. Independent review found no confirmed blocker and added a
    selector-only alias regression for qualification.
16. Built the candidate and captured phase timings, full SDK optimization and
    successful corrected validation. Kept the initial validator option error.
17. Ran the actual public masked-discovery GPU oracle to PASS, with complete
    validation/guard summary and tested binary hashes. Completed the original
    external CTS matrix with five passes and two explicit watchdog terminations;
    retained its runner labels and raw timeout records. Final offline tests are
    running; final parent build, separate bounded miss/callable reruns and other
    runtime results will be appended by the shared-build owner.
