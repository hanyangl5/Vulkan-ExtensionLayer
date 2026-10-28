<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Query capture foundations and typed pointer memory, 2026-10-28

This checkpoint advances the full software AS/RQ/RTP goal. It does not complete
M0-M5, simultaneous query execution, logical-stage synchronization, conformance,
or physical no-native-RT qualification. Production source remains inside the
existing five `layers/raytracing` directories.

## Typed logical pointer memory

The continuation compiler now represents supported StorageBuffer pointer values
inside Function/Private memory and aggregates with numeric route/selector
records. Loads and stores address those records through the same persistent
frame offsets as the containing source variable. Dynamic array cells retain
their own identities; native logical pointer bytes are never spilled.

Supported `OpPtrAccessChain`/`OpInBoundsPtrAccessChain` operations retain typed
Element arithmetic and pointer ArrayStride. Same-view loop advancement folds
into an accumulated selector, keeping route sets finite. Same-pointee
ArrayStride changes are explicitly rejected before route fixed-point analysis:
their general byte-offset normalization remains unfinished. A source-validated
zero-displacement stride-changing loop checks this rejection rather than
allowing unbounded route growth.

Array frame codecs now use dynamic typed loops instead of unrolling every
element. The source and generated SPIR-V validators remain enabled. Logical
pointers cannot be direct operands of arbitrary composite construction or
extraction; the new valid fixture uses pointer-member stores, whole-aggregate
loads/stores and member pointer loads.

The executed GPU oracle covers Function and Private source storage separately:
8193 pointer cells, dynamic indices `8170 + invocation`, branch-selected alias
stores, whole-aggregate SSA across suspension, and signed loop Element
arithmetic. Each uses 16 invocations, two activations, three ReportIntersection
requests per activation and an independent comparison of all 256 output words.
The previous 32-invocation Phi/Select/helper cases remain enabled. These new
executions use the standalone StorageBuffer frame harness. Physical-address and
PrivateArray variants are compiled/validated and checked for consistent layout;
this is not evidence of their new pointer-memory GPU execution. The 65537-element
codec check establishes compilation, validation and bounded generated code size,
not execution of that large value on GPU.

`OpCopyMemory`, volatile/pointer-memory operand semantics, other pointer storage
classes/views, real descriptor-array NonUniform execution, complete resource
limits and numerical/execution-mode qualification remain open.

## Owned command capture and layout lifetime

The selected SDK registry/header generate 311 typed `vkCmd` wrappers and 247
structure schemas, plus a per-command coverage manifest. Capture owns supported
input arrays, strings, nested pNext structures, AS geometry/range inputs and
descriptor payload branches. Three command entries and additional runtime
schemas explicitly reject capture; the counts do not imply universal support.
Unknown/opaque shapes poison the journal while the application's ordinary
recording continues through its original translating/native hook.

GIPA/GDPA return a recorder only when the direct hook/native entry exists. Alias
names are preserved. Internal translation calls bypass the recorder, and
command-buffer begin/reset/free/device cleanup own the journal lifecycle.
Ignored primary inheritance pointers are removed from the captured copy only.
Secondary rendering inheritance carries flags/renderPass context, and disabled
NV viewport inheritance never dereferences its ignored pointer. Unsupported
conditional schemas fail before reading their opaque payload.

Named pipeline layouts are captured as recording-local semantic identities
with shared immutable definition owners. Each future execution inherits its
own table after Begin; source reset and public layout destruction cannot retire
those private native owners. Re-emission preserves semantic keys for translating
hooks and converts native-only consumers to genuine ICD handles in independent
temporary storage. Host checks cover source reset, handle reuse, aliases and
actual descriptor-bind forwarding, including failure without an ICD call when
an identity is missing.

Different simultaneously live RT definitions that collapse to one native public
layout handle are detected and rejected. Complete transparent public layout
virtualization remains required for that case. Inline maintenance6 layouts
remain unsupported by capture until descriptor-set-layout definitions are
materialized and retained at recording time. Ordinary existing inline commands
still use their current path. The manifest also lists platform/beta commands
without generated wrappers: future replay admission must account for them and
cannot rely only on a successful journal status.

**Submissions still use the bounded query backend.** Fresh per-execution native
command buffers, query payloads/markers, loader dispatch initialization, native
completion retirement and transactional rollback are not connected here.
Simultaneous query recordings remain rejected. The journal foundation is not an
implementation of query submission replay.

## Logical-stage planning

The separate [planner contract](../layers/raytracing/docs/QUERY_STAGE_SCHEDULING.md)
and host oracle preserve source logical domains, waits across future
submissions, explicit dependencies, scoped signals and completion joins. The
oracle includes a legal host/timeline feedback schedule that deadlocks if a
TRANSFER wait is broadened to physical compute, even after splitting before the
query copy.

The planner does not execute native submissions or discover dependencies from
recorded bytes. The metadata frontend, exact state preambles, native/external
semaphore acquire bridges, native-query/render-pass handling and resource
retirement remain required. Existing unsafe-wait rejection is retained.

## Validation and retained failures

The [machine-readable record](rt_query_replay_pointer_memory_2026-10-28.json)
binds artifacts, logs and the external CTS snapshot to their SHA-256 hashes.
The external run is a regression of the existing pipeline-library and UPDATE
selection; it does not exercise a completed query replay executor.

The final offline selection passes **34/34**. The 11 distinct GPU checks have
passing observations across the initial 10/11 selection and the separately
corrected property-fixture recheck (1/1). These include software/native cross-queue
queries, maintenance6, both software/native library modes, source CFG and typed
frame execution. The initial failed selection is not relabeled as a clean run.

External VK-GL-CTS passes **32/32** on the frozen software layer: all 20 existing
library and 12 UPDATE/RQ cases, with zero failures, NotSupported results,
crashes, timeouts or missing results. All 32 downstream guard audits complete
with zero rejections. QPA/stdout scans find no `Validation Error`, `VUID-` or
`SYNC-HAZARD` diagnostics. This uses the same CTS binary/revision as the prior
checkpoint, including its separately retained native controls.

The software-layer SHA-256 is
`8874bf5d26df27d4411b51a1e459e408c99757117e2399d73b08d87f3f516b19`.
The immutable external run is
`build-rt-cts-evidence/query-pointer-guarded-2026-10-28`; local logs are under
`build-query-pointer-2026-10-28`. Canonical and parent RT-target builds pass,
and all 188 pinned references remain unchanged.

Retained intermediate observations include the original NonUniform test's
obsolete requirement for an emitted CopyObject, the invalid logical-pointer
CompositeConstruct source, and the first offline run's 33 pass/1 fail caused by
a stale forwarded-call counter after adding a new fixture. The final checks
retain the actual-load NonUniform requirement and valid source validation;
the native passthrough assertion now samples its own baseline counter.

The first GPU selection records 10 pass/1 fail: the property component's old
oracle expected masked instances to be removed. Its corrected oracle derives
retained instance count from non-null input references, checks the independent
unmasked sequence 2/1/0, compares queries against required sizes/input counts,
and verifies each retained ID, mask, SBT flags and public BLAS reference. The
original failing run is retained; the focused recheck is a separate observation.

The parent project's RT target is checked separately from its default all-target
build. The attempted default build also reached unrelated synchronization2 and
shader-object sources and failed on existing Windows `min`/`max` macro expansion;
those sources were not changed. The historical 226-case result remains 220 pass,
2 NotSupported and 4 unresolved crashes. New passing slices do not rewrite it.
