<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# GPU refit, logical pointer continuations and RTP libraries, 2026-10-28

This checkpoint adds topology-preserving GPU UPDATE, typed logical
StorageBuffer pointer transport across suspension, and RT pipeline-library
linking to the shared software AS/RQ/RTP implementation. It also adds a test-only
downstream native-RT rejection boundary. These changes advance individual
components; they do not complete M0-M5, Vulkan conformance, or qualification on a
physical GPU without native RT.

## Topology-preserving GPU UPDATE

[`vkrt_build.c`](../layers/raytracing/src/vkrt_build.c) now records UPDATE through
the leaf producer and the refit branch of
[`bvh4_mesa_encode.comp`](../layers/raytracing/shaders/bvh4_mesa_encode.comp).
UPDATE omits Morton sorting, hierarchy construction and binary-tree collapse.
The final runtime BVH4 topology supplies the retained node IDs, child IDs,
parents and leaf placement.

The shared 80-byte refit push ABI describes two phases. Phase zero copies the
source AS when the destination is distinct and initializes one completion
counter per retained internal node. Phase one rewrites each retained leaf from
the new geometry, using its original primitive/geometry/instance ID and a
geometry-prefix table to find the source record. Dense runtime leaf order is
not assumed to match application order. Updated records retain complete flags,
TLAS transforms and both public and resolved BLAS references.

Each updated child publishes its bounds before incrementing its parent's
device-scope acquire/release counter. Only the final child computes the union
and proceeds to the next ancestor. No invocation polls for an unscheduled
workgroup, and the host does not read back active counts or topology. The two
phases use the existing chunked dispatch and intervening compute barriers.
Out-of-place UPDATE preserves the source bytes; in-place UPDATE retains the AS
address and representation layout.

The runtime leaf producer now retains non-null zero-mask instances and
references to valid empty BLAS objects. Their degenerate bounds are represented
at the transformed origin, so a subsequent legal UPDATE can make them visible
without creating a new topology leaf. A null AS reference remains inactive.
This distinction follows the specification's separate treatment of inactive
and degenerate instances in
[acceleration-structure update rules](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html).

The implementation still requires valid UPDATE inputs and unchanged active
primitive membership. It does not establish all mandatory count limits,
ill-conditioned transform behavior, additional Mesa builder variants or
cross-vendor numerical/performance qualification. COMPACT still retains the
existing representation size. GPU refit reduces reconstruction work; no
performance improvement is claimed without measurements.

## Logical StorageBuffer pointers across suspension

[`state_compiler.cpp`](../layers/raytracing/src/state_compiler.cpp) represents
supported logical pointers as a route tag plus typed dynamic selectors. A route
identifies a descriptor root, or null, and its exact access path. Constant
selectors retain their module identities. Dynamic integer selectors of up to
64 bits are captured at the pointer SSA definition, with separate storage for
each SSA value. Equal routes share tags, while loop-carried values preserve
their own selector snapshots.

StorageBuffer pointer Phi/Select and supported access/copy operations propagate
these routes. Pointer Phi inputs are captured with parallel assignment semantics
before destination snapshots are overwritten. Inlined pointer helpers use the
same representation. The continuation planner opts into this backend-owned
transport instead of recursively replaying pointer dependencies after a
suspension. Native logical pointer bytes are never stored in the continuation
frame.

On resume, the generated control flow selects the route and executes the actual
Load, Store, atomic or pointer comparison within that route's branch. Native
pointers remain rooted in that branch; numeric result registers merge the
operation's result. Only the selected memory operation executes. NonUniform
provenance is carried into the reconstructed access. Null comparison routes are
preserved; undefined null dereferences do not issue a native memory access.

This form replaces an earlier generated native StorageBuffer pointer Phi form
that validated as SPIR-V but caused a driver access violation during compute
pipeline creation on the local NVIDIA installation. The retained original
shader is `build-source-layout/typed-pointer-before.spv`, SHA-256
`c7271c970373f0e897307ceb2759a6b79eb548bc91baf5203f6a6fff4b1baa95`.
`typed-pointer-before-evidence.json` in that directory binds the shader and CDB
logs. First- and second-chance observations include a read of address `0x8` in
`nvoglv64.dll`; this is an observed driver failure, not a proved diagnosis of
its internal compiler defect. The original executable was subsequently rebuilt,
so its hash is not inferred from the current executable.

The route mechanism covers the supported finite StorageBuffer root/access
representation. It does not establish arbitrary pointer storage classes,
pointer arithmetic, pointers embedded in aggregates, all resource provenance or
all continuation execution modes. Unsupported source operations remain explicit
compiler failures. Subgroup/derivative scheduling and conflicting stage
execution modes remain separate work.

## Pipeline-library ownership and final linking

[`vkrt_rtp.cpp`](../layers/raytracing/src/vkrt_rtp.cpp) now stores a library's
typed stage metadata, stage kinds, entry names, groups, interface sizes and
stack requirements. Libraries may have no local stages, no local groups, or
no raygen stage. Library creation retains these definitions without constructing
a native RT pipeline or a bound final compute program.

Linking appends local stages/groups first, then each referenced library's
already-flattened stage/group lists in `pLibraries` order. Group stage indices
are rebased to the appended stage range. Repeated library references deliberately
append repeated lists; dependencies are not deduplicated as a graph. The linked
pipeline owns copied metadata and shader source instead of borrowing caller
create-info memory or shader-module handles. Applications still retain linked
libraries for the lifetime required by Vulkan; public GPU fixtures retain the
complete library chain through execution and destroy descendants first.
Final creation uses
the shared typed runtime planner and compute-backed program builder. This list
ordering follows
[`VkRayTracingPipelineCreateInfoKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/VkRayTracingPipelineCreateInfoKHR.html).

The implementation checks library identity, recursion depth, declared payload
and hit-attribute sizes, inherited no-null requirements and checked list-size
arithmetic. It retains library stack sizes for stack queries. Shader-group
handle retrieval remains a final-pipeline operation: the optional
`pipelineLibraryGroupHandles` contract, including stable handles across links,
is not implemented by this change. The distinction is specified by
[`vkGetRayTracingShaderGroupHandlesKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetRayTracingShaderGroupHandlesKHR.html).
Complete pipeline options, capture/replay, extended creation chains, layout
interoperability and full link/stack/resource qualification remain open.

Maintenance5 `VkPipelineCreateFlags2CreateInfoKHR` replaces the legacy flags
field. The backend checks the full 64-bit value before narrowing supported bits
and does not modify the caller's chain. The first merged 32-case CTS attempt
retained two failures in maintenance5/library cases because this chain was
previously rejected. Both cases pass the focused flags2 recheck; the complete
first attempt remains 30 pass and 2 fail rather than being rewritten as clean.

## Test-only downstream rejection and evidence audit

[`VK_LAYER_VKRT_NO_NATIVE_RT_GUARD`](../layers/raytracing/tests/no_native_rt_guard_README.md)
sits below the software layer. Its execution-only mode preserves native RT
discovery so unmodified CTS can select RT cases, then rejects observed native
RT feature/extension requests, entrypoints and RT resource/stage/access use
after translation. API/feature tables are generated from a pinned Vulkan
registry, and SPIR-V capability/extension/instruction/model tables from a pinned
core grammar. Their hashes are recorded in the generated header.

The guard inspects shader modules, shader-object SPIR-V and maintenance5 inline
pipeline modules. Opaque shader binaries, shader identifiers and pipeline
binaries fail closed because their instructions cannot be inspected at this
boundary. Missing evidence destinations and evidence-write failures cannot
produce passing observations. A result-returning rejected API returns an error;
a rejected void command terminates its test process.

Schema 2 JSONL records include one PID and distinct DLL-load identities. The
auditor accepts CTS discovery/unload/execution reloads within that PID only when
each load segment has its own start record, monotonic counters, balanced
instances/summaries and zero rejections. At least one complete segment must
exercise device creation and inspected shaders. Mixed PIDs, reused load IDs,
truncation or a successful process exit without complete boundary evidence are
rejected. The CTS runner freezes the guard DLL/manifest and auditor alongside
the software-layer snapshot.

This observes a pinned set of downstream interfaces on the actual Vulkan driver.
It is not physical no-RT-device qualification. The separate discovery-mask mode
removes native RT visibility; the software layer's remaining emulation-discovery
gap can yield NotSupported there. Such a skip is not an execution pass. Future
or unknown extension interfaces require their own boundary audit.

## Intermediate evidence and final integration

The earlier offline integration log
`build-source-layout/rt-integration-2026-10-28-offline.log` records 30/30 passes.
The actual `--pointer-only` GPU fixture subsequently executes both element and
array pointer shapes: 32 invocations, parallel loop Phi swaps, Select/null,
inlined pointer helpers, independent root/index snapshots and repeated
suspend/resume. Its log is
`build-source-layout/rt-integration-2026-10-28-pointer-route-gpu.log`; validation
reported zero errors for that run. These observations do not replace final
merged-artifact validation.

The 12 selected refit CTS cases in
[`rt_cts_refit_cases.txt`](rt_cts_refit_cases.txt) passed under the guard in
`build-rt-cts-evidence/refit-guarded-first/summary.json`. That intermediate run
used software-layer SHA-256
`b6f6ee903c39acf0475799d8d57d8b69635e7e69ff0a361e7714589e00096b1d`.
The executable came from `D:/Codes/VK-GL-CTS`, declared revision
`d18e4c68543103371715845cd914c71e5859963e`, executable SHA-256
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
These are selected intermediate results, not a result for the final merged DLL
or the larger pipeline-library selection.

The rebuilt flags2 checkpoint passes **32/32 external CTS cases** in one frozen
software-layer/guard snapshot: all 20 selected pipeline-library cases and all 12
UPDATE/refit cases. There are zero failures, NotSupported results, crashes,
timeouts or missing results. All 32 schema-2 guard audits complete with zero
rejections, and stdout/QPA scans contain no `Validation Error`, `VUID-` or
`SYNC-HAZARD` diagnostics. The separate native controls pass **20/20 library**
and **12/12 UPDATE** cases; they execute without the software layer or guard.

The final software-layer SHA-256 is
`1dbe2045dc0daecd9526bb8821ef4540ed2458244a365a3ad73d47228b2944ed`.
The guard SHA-256 is
`e9334c97f171ab4933d80a2f1955e97dbd0573c8a1e4cd14dadd0d8d79fd9fb8`;
the frozen auditor SHA-256 is
`d224732f36b7a7d3cdf045d6217ba30c0300ecdeb980c76a1c541d32cd508fcb`.
The complete run is `build-rt-cts-evidence/refit-library-guarded-flags2-final`.
Its exact selection concatenates the tracked library and refit case lists,
without duplicates. The [machine-readable record](rt_refit_pointer_libraries_2026-10-28.json)
binds per-case QPA, stdout and guard logs to their hashes.

Canonical and parent-project builds pass. The final offline suite passes
30/30, and a fresh component-only build passes 5/5 tests with validation enabled
for its GPU checks. The earlier public integration regression passes 8/8,
including the complete BUILD fixture, software/native refit, library modes and
source CFG execution. Its test-executable hashes are retained separately from
later cosmetic Win32 macro-name rebuilds. The final layer also passes **11/11**
GPU rechecks: software/native library modes, aligned AS values, software/native
UPDATE, complete source CFG/pointer execution and the three GPU components.
Their current executable hashes and actual artifact scope are in the
machine-readable evidence. The earlier full default BUILD result remains a
separate observation.

Retained intermediate evidence includes the 30/32 first merged CTS attempt, the
original generated-pointer driver exception, the initial native library fixture
crash before correcting its retention lifetime, and the BUILD fixture's old
expectations that pruned recoverable degenerate instances. The first guarded
smoke run passed both QPA cases but failed its too-strict one-load audit; those
results remain failures of the original proof gate. Schema-2 identity and reload
controls were added before the new passing run. The historical 226-case run
remains 220 pass, 2 NotSupported and 4 crashes; none of these new slices rewrites
that result or proves the root cause of its intermittent process failures.
