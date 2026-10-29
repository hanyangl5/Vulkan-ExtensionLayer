<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# Software OMM lifecycle and serialization checkpoint

This is progress toward the complete mandatory device contract, not a completed
milestone or EXT conformance claim. The goal remains active. Source stays under
`layers/raytracing/omm`; the independent RT fallback checkout is unchanged.
Default operation remains native pass-through. Experimental software selection
is explicit and captured at instance creation; automatic discovery on devices
without native OMM remains disabled until the consumer contract is complete.

## Implemented behavior

AS creation and descriptor updates no longer query a native AS address before
ordinary storage is bound. Each tracked AS owns a stable 96-byte allocation:
the original 64-byte RootCell followed by a 32-byte identity cell. GPU build
publication writes the native AS address and RootCell address into that cell.
Descriptor contexts use shadow ABI v2, retaining a 16-byte record that points to
the cell and distinguishes unwritten, explicit-null and populated records.
Compute RQ resolves the published cell at execution time. Both buffer-binding
APIs, record-before-bind, descriptor update-before-bind, replay and source-release
paths have dedicated native AS/GPU tests. The TLAS registry ignores unrelated
unbound BLAS objects instead of issuing an illegal address query.

The AS-address eligibility check separately preserves the SPARSE_BINDING
exception. Tracker tests cover that branch and checked ranges without issuing a
native address query. They do not establish real QueueBindSparse/GPU coverage;
complete sparse-resource interactions remain required.

Public GPU micromap serialization and deserialization now execute through an
embedded compute shader. The 32-byte envelope contains the actual downstream
physical driver's UUID and a fixed layer storage compatibility UUID, followed by
the execution-time active canonical storage. Compatibility checks compare those
32 bytes. Commands validate tracked spans and alignment before recording;
the shader checks headers, sizes and overlap before writing. The implementation
adds no temporary submit, GPU-to-host readback or command-owned allocation.
Application compute pipeline and push-constant state are restored through the
same command-state path as build/copy operations.

Address-span lookup considers all buffers with overlapping BDA and selects the
largest logical remainder, retaining accessible size from that same candidate.
A real same-memory, same-BDA short alias regression keeps the smaller, newer
buffer live through serialization, restoration and replay.

When native timeline semaphores are supported, the software device privately
enables that feature. Each queue has a private timeline observer. AS-write
submissions append one signal to the final original submit structure in the same
native QueueSubmit/QueueSubmit2 call. Application fences, existing signal/wait
values, device-group indices and protected-submit data are preserved. Private
counter completion remains independent of application fence export, import,
reset, destruction and payload reuse. Reservation and allocation failures occur
before native submission; actual native-submit failures leave safe value gaps.
The native maxTimelineSemaphoreValueDifference bounds reservations using a
nonblocking counter query. No extra submit or wait is injected.

FD and Win32 fence hooks preserve native output/failure semantics. The fallback
can observe a duplicated SYNC_FD and conservatively retains generations when
observation is unavailable; sharing/import/reset/destruction does not invent a
completion proof. Later proven work on the same queue or explicit idle can retire
those generations. Linux's real fcntl/poll/close path has not been exercised on
this Windows host. The complete no-timeline external-payload matrix, including
acquire-fence interactions, remains unqualified.

Synchronization translation now maps Transfer read/write access to the shader
access performed by the codec when the original scope includes MICROMAP_BUILD,
or ALL_COMMANDS on a compute-capable queue. It preserves required transfer stages,
original resource ranges and ownership fields. ALL_COMMANDS on a queue without
compute does not acquire unsupported shader access, and unrelated transfer-only
scopes stay unchanged.

## Verification

Final results and exact file hashes are recorded in the
[companion JSON manifest](omm_lifecycle_serialization_2026-10-29.json).
The frozen software DLL SHA-256 is
`c4be10fc58c446e772d3c631caadb0eb345a759e1de8427292e3eae00d2eb870`.

- Standalone non-loader attempt: 25 Pass, 2 Timeout at 120 seconds. The two
  timeouts are the older native RQ and classifier proofs; both executables are
  byte-identical to the previous passed checkpoint. The native RQ shader is also
  byte-identical. Both passed a targeted retry (16.98 and 2.12 seconds), so the unchanged
  frozen artifact has passing coverage for all 27 components, with the original
  25-Pass/2-Timeout attempt retained.
- Public loader API: 93 checks, zero failures, validation errors or warnings in
  each validation-layer order. This includes actual GPU serialize/deserialize,
  compatibility, active-length/canary checks, replay, state restoration and
  restoration after source micromap/storage/build-input destruction.
- Parent OMM targets build successfully; all 12 parent offline tests pass.
  Host-only core build and all three tests pass without Vulkan dependencies.
- Submission transaction/generation tests pass 528 checks; synchronization tests
  pass 326 checks. Native AS integration passes 1,597 assertions; descriptor contexts pass 2,862
  checks; codec GPU tests pass 81 assertions and confirm actual equal-BDA aliases.

The preliminary pre-freeze integration attempt was aborted after long native
fence waits, with raw output and noninvasive CDB stacks retained. The first
bounded frozen run still timed out in the two older proofs, while all other
13 GPU components and all 12 offline components passed. GPU runtimes varied
substantially: the metadata fixture took 83 seconds rather than the prior
checkpoint's subsecond time, while later components returned to ordinary speed.
A wait-stack sample and missing stdout do not prove absence of GPU progress.
The cause of the observed slowdown/timeouts is unresolved. Neither aborted
attempts nor timeouts are counted as passes or silently replaced by retries.

External CTS uses the unmodified checkout at `D:/Codes/VK-GL-CTS`, revision
`d18e4c68543103371715845cd914c71e5859963e`, executable SHA-256
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
The exact 36-case list contains 34 compute RQ cases and two size-query cases;
each layer order receives an immutable snapshot of the frozen DLL. Exact commands,
environments, QPA, stdout and result/hash records remain in per-run summaries.
The pinned CTS does not execute micromap serialization/deserialization; its
serialization-size query is not codec execution coverage. Its copy.compact case
records CLONE, so it is not external COMPACT coverage. Software RTP rendering
remains unsupported. The first current run with SDK VVL 1.4.357 returned 36 InternalError results:
the host driver changed from 596.49/API 1.4.329 to 621.01/API 1.4.357, and generic
CTS device creation now enables VK_EXT_cooperative_matrix_maintenance1, whose
structure 1000659000 is unknown to that VVL. A native-only ordinary triangle CTS
control reproduces the error without the OMM layer. No CTS case or extension was
filtered and no validation category was disabled.

A pre-existing official VVL 1.4.366 nightly (commit
655424a5b0b64fb96ada958c224701653cc02c42) was copied read-only into this worktree's
evidence directory. Its DLL SHA-256 is
`f05bdd58e2f8247fb64b0e0267aeff9c0bab925b7de4c190f8594ae1eb2c31df`.
The same OMM DLL passes a two-case preflight and both 93-check public API orders
with that VVL. The full external reruns pass **36/36 in each layer order**, with
zero validation errors. They retain the old incompatible run. The actual codec
passes 81 assertions at subdivision level 6 with SDK VVL and at level 12 with
this newer VVL, including the real equal-BDA alias. Exact current results are in the manifest.

## Remaining required work

Integer-derived AS runtime lookup remains open. TLAS registry snapshots also
need to handle BLAS objects created or bound after the TLAS command was recorded. Graphics RQ and production native RTP still require context/SBT transport,
nested/callable/indirect execution and pipeline-library integration. AS UPDATE,
copy, serialization, relocation, generic and arrayOfPointers paths remain open.
The full feature, queue, event, sparse-resource and lifetime matrix, legal query
queues without compute/timestamps, lower extension-only environments and actual
non-native-OMM hardware qualification remain required. Word-readable input tails
and larger triangle-record-count scalability also retain their prior limits.
All M0-M5 milestone boxes stay open.
