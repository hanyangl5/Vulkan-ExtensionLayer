<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Execution-owned RTP BDA arenas

This checkpoint replaces the production destination PrivateArray frame
transport with execution-owned physical-address arenas. The complete
[AS/RQ/RTP goal](ray_tracing_fallback_goal.md) remains active. This change does
not complete M0-M5, software discovery, or mandatory synchronization coverage.
The source base is `8af0161682b83906b266bde741e1ab8a5edede36`.

## Problem and observed replacement

The preceding checkpoint retains large destination PrivateArray failures and
native GLSL controls that return DEVICE_LOST. Those observations remain failures;
no particular driver defect or Private-memory size limit has been established.

A new public Vulkan fixture initializes a 32,769-element uint source array
(131,076 bytes), keeps it live across two nested callable activations, then
checks every element through an independently calculated full-array digest.
Each original three-dimensional launch ID also contributes checked callable
values, a count, and an input value to an output row. Padding is checked. The
fixture uses a 3 x 3 x 3 launch and exactly the public stack equation
`raygenStack + 2 * callableStack`: 133,312 bytes on the software pipeline, without
an added hidden stack allowance.

The frozen preceding final DLL, SHA-256
`3c4ed8ae1e3c240e38080fd4f147f895c2312b162491d3f1435fd99c3c7994eb`,
returns DEVICE_LOST at the first caller-fence wait for this same fixture. Its
raw log is `build-rtp-bda-2026-10-28/old-private-production-baseline.log`.
The production BDA replacement passes all seven fixture phases. The old result
is a real public production baseline, separate from earlier compiler diagnostics.

Source SPIR-V Private storage semantics remain supported. Its initialized
values, pointer identity, aliases and cross-suspension persistence use typed
frame backing; replacing the destination transport does not remove source
Private or Function variables. Destination PrivateArray diagnostics remain in
this checkpoint and are candidates for a separate retirement change.

## Production execution and ownership

- One native compute variant is compiled and cached per RT pipeline. The
  current dynamic stack budget is execution data, rather than a native Private
  array size and a separate native compilation per budget.
- Source recording validates and captures Trace state but allocates no mutable
  execution context or arena. A generic replay requirement now includes RTP,
  independently of optional query initialization.
- Every submitted primary occurrence and every secondary occurrence receives
  its own replay clone and mutable context/arena leases. Caller fences and
  source submit error handling keep their original native semantics. Private
  completion observation retains leases until completion is proved. Failed
  pre-submission clones roll back; unknown completion remains quarantined.
- Host-coherent chunks hold immutable per-stage push snapshots and one context.
  Separate device-local-preferred chunks hold the arena and are not mapped or
  CPU-cleared. The runtime initializes each logical activation's frame state.
  Compatible native memory types remain an allocation-failure fallback.
- Pool allocation solves physical-address and buffer-offset alignment
  independently, checks overflow and rejects an unsatisfiable base alignment.
  Old descriptor-backed address-cell callers keep their existing contract.
- The 64-MiB tile target is an allocation strategy, not a public stack cap.
  Oversized per-invocation budgets retain a complete invocation arena. OOM
  shrinks tile width down to one; it does not shrink the selected stack budget.
- Each tile issues the exact one-dimensional native dispatch count, including
  a short tail. A private 16-byte push carries the context address and original
  launch base, preserving the source three-dimensional launch ID. Context
  storage is O(stage count), independent of tile count.
- HOST_WRITE-to-compute visibility covers the actual context slice. Adjacent
  tiles use an arena-slice compute read/write dependency before reusing it.
  Application compute pipeline or shader object, ordinary descriptor bindings,
  and push constants are restored after each translated Trace.

The generic pool registry is owned by execution replay instead of query state.
Destroy removes the old metadata before native Destroy makes a numeric handle
available for reuse. Tracking-failure rollback uses native Destroy directly
because it never acquired metadata ownership. A reentrant native stub creates a
new pool with the same handle during Destroy and independently checks both same
and different family/flags. The old-order mutation fails those oracles.

## Validation observations

Raw integration evidence is in `build-rtp-bda-2026-10-28`. Independent allocator,
launch/runtime and generic-execution CPU evidence is under
`build-source-layout`; the [manifest](rtp_execution_arenas_2026-10-28.json)
binds source, artifact and evidence hashes.

The public fixture covers first execution, serial reuse, three primary
occurrences in one submit, one simultaneous primary pending twice, one
secondary twice inside a parent, two pending parents sharing a simultaneous
secondary, and different pending contexts/stack budgets. Timeline gates keep
both submissions pending before host release. Output atomics give defined
multiplicity for repeated execution; host tests separately assert disjoint
leases rather than depending on a particular GPU overlap schedule.

Forcing native dispatch limits to `(2,2,64)` produces 182 dispatches for the
13 Trace occurrences: fourteen tiles per 27-invocation launch, including a
one-invocation tail. Submit2 and disabled-optimization variants both pass all
seven phases with zero validation errors and complete zero-rejection guard
logs. The guard observes maximum dispatch `(2,1,1)`. These are artificial-limit
strategy tests, not ordinary mandatory-profile qualification.

The final integrated build and 40/40 offline checks pass. Independent host
checks cover O0/O2 allocation classes/alignment/failure cleanup, concurrent
leases, wide launch arithmetic, tile fallback, generic replay without query
initialization, retained completion and same-handle pool reuse. Alignment,
class-mixing and old pool-destruction-order mutations fail as expected.

The 24-run ordinary-limit GPU matrix passes in both VVL orders, with
synchronization validation, 14,304 downstream guard checks, zero rejections and
no scanned VUID/Validation Error/SYNC-HAZARD diagnostics. It covers the public
large-array fixture, ordinary compute state, maintenance6, shader objects,
derivatives/libraries, descriptor/resource boundaries, query replay/acquisition,
the eight-stage BDA runtime and four qualified SSBO/BDA pointer combinations.
The matrix fixture executable is retained as
`build-rtp-bda-2026-10-28/test_rtp_arenas.pre-sbt-align.exe`, SHA-256
`93497aa684604e2b30ec1eed3889468e465586e19e04bd33166283afe0c1692a`.

A final portability correction aligns the public SBT address independently of
the native buffer base and deliberately uses a nonzero suballocation. The final
fixture separately passes 4/4 GPU runs: both ordinary VVL orders, forced-tile
Submit2 and forced-tile disabled optimization. Both forced runs again record
182 dispatches and maximum `(2,1,1)`, all with zero validation errors/rejections.
Production DLL bytes did not change for this fixture-only correction.

The frozen production DLL separately passes **50/50 external Vulkan CTS**,
with zero failed, NotSupported, crashed, timed-out or missing results. The fifty
complete execution-only guard audits contain **125,113 boundary checks and zero
rejections**. The recorded stdout scan has no VUID, Validation Error or
SYNC-HAZARD hits. This is new BDA implementation evidence, separate from the
preceding checkpoint's 50-case and 13-case snapshots.

- Evidence: `build-rt-cts-evidence/rtp-execution-arenas-2026-10-28/summary.json`.
- Software DLL SHA-256:
  `bfae2245ba92cfc67b48c4a9277f904eaaf1b80fc50dae826b6595758b43b6df`.
- CTS checkout: `D:/Codes/VK-GL-CTS`, declared revision
  `d18e4c68543103371715845cd914c71e5859963e`.
- CTS executable SHA-256:
  `8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.

The case list includes twenty pipeline-library cases, twelve RTP/RQ UPDATE
cases, twelve RTP AS operation cases, two RQ compaction/serialization cases and
four cross-suspension spill cases. The parent project's RT target also builds.
The unrelated historical default-parent build failures are not reclassified by
that target result. This slice does not establish complete CTS conformance.

## Remaining boundaries

This is an integrated replacement for the observed large-frame blocker, not a
complete resource proof. Native maxBufferSize and single-allocation limits are
not yet preflighted for a giant private arena plus alignment overhead. Those
limits must be checked before native creation; an invalid native CreateBuffer
is not an acceptable OOM strategy.

Ordinary descriptor-set/compute-push restoration is exercised, but compute push
descriptors and descriptor-buffer offsets are not captured by the RTP restore
state. A translated RT descriptor bind can disturb those prior compute states.
This source-confirmed gap has no GPU reproducer in this checkpoint; complete
interop requires a targeted public reproducer and implementation.

Complete command schemas and native-query interop, public logical-stage
synchronization, mandatory source memory-model and shader/API semantics,
truthful no-RT extension/features/properties, private resource budgets, larger
construction/numerical qualification, physical no-RT hardware, other vendors
and POSIX remain open. The execution-only guard retains native discovery for
CTS selection. It does not establish masked-discovery or physical no-RT
qualification. Historical CTS crashes, timeouts and unsupported cases remain
separate from new passing slices.
