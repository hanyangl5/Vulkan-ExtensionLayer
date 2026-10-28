<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Execution-owned AS query replay, 2026-10-28

This checkpoint connects owned command capture to fresh native AS-query
executions. It also extends typed pointer memory and pools query allocations.
The full [AS/RQ/RTP goal](ray_tracing_fallback_goal.md) remains active; no complete
M0-M5 milestone is closed. The [machine record](rt_execution_replay_2026-10-28.json)
binds the observations below to artifacts and retained logs.

## Execution and lifetime

Each occurrence of a primary with virtual queries gets a fresh native command
buffer, query payloads, completion markers and formatter scratch. Every
secondary occurrence inside that replayed primary is cloned independently,
including ordinary children. Pool family/flags, inheritance, source usage,
device masks and caller submission data are retained. Loader dispatch data is
initialized before native recording. Source recordings capture virtual query
operations without sharing execution payloads.

The original native submission keeps its batch count and caller fence. After
successful source submission, a separate empty native submission observes
completion through a private fence. No private semaphore signal is appended to
the source stream. Prefix query availability uses its own native marker; it
does not wait for the entire execution's retirement fence.

| Outcome | Handling |
| --- | --- |
| Source qualification, clone or preparation failure | No source work is submitted; roll back preparation before discarding private recordings |
| Original native OOM | Roll back and discard; no observation is submitted |
| Original native LOST/UNKNOWN | Quarantine private resources; do not assume an observation was queued |
| Source success, observation OOM/UNKNOWN | Preserve source success and retain resources; a later successful same-queue observation can cover already-settled executions |
| Source success, observation LOST | Preserve accepted operation ownership and latch/report device loss |

Settled stamps are published only after all native calls using an execution
have returned. A successful observer's cutoff excludes other observations still
in flight. Queue/device idle proofs use checkpoints captured before the native
idle call. Counter saturation never wraps into an older completion proof.

Source qualify/native-submit/result-publication is serialized per queue, without
holding the query-list lock across native calls. Other queues can progress;
observation and native idle do not hold this admission lock. Wait history has a
separate publication epoch, so an old idle callback cannot erase a newer wait.
Pipeline initialization failure no longer frees device-owned wait history.

Device destruction has an additional obligation: an application may wait only
its original caller fence, leaving a later private observation pending. Exclusive
execution teardown therefore drains submitted private work through native
DeviceWaitIdle before destroying its objects. Only SUCCESS or DEVICE_LOST
permits cleanup. Other errors retain objects and retry with a one-millisecond
backoff; a fixed retry count is not a completion proof. Normal submission,
collection and query-result paths do not use this teardown drain.

## Capture, pooling and pointer semantics

The SDK357 registry now has wrappers for all 317 known command-buffer commands,
247 struct schemas, no unavailable command entries and no ABI-generation
rejections. Nine commands remain explicitly unsupported for replay. Six of
these have checked ABI-only forwarding wrappers for declarations absent from
the selected C headers: their original calls are forwarded without dereferencing
unknown payloads, while actual use poisons replay admission. This is coverage
of the pinned registry, not proof for unknown future commands or all schemas.

Query storage, payloads and native-copy scratch use the shared private pool.
Physical-BDA alignment and native buffer-offset alignment are accounted for
separately. Host publication/reset uses byte-safe copies, and the GPU formatter
writes exact bytes, allowing legal unaligned 32/64-bit result locations without
overwriting availability fields, gaps or guards. The 13-layout component checks
include unaligned output and subsequent native transfer copies.

Typed Function/Private pointer cells and aggregates now support OpCopyMemory
without memory operands. The compiler also preserves actual stride-4/stride-8
array origins through an unadorned pointer view and restoration of the matching
stride. This does not permit arbitrary nonzero stride changes. Acceptance by
a source validator alone does not establish that a view's stride matches its
origin array.

The GPU oracle checks all 384 output words, including stride-8 padding, in the
new mixed-view forms. It covers 8193 pointer cells, aliases, aggregate clobber
and restoration, helper/Phi/Select passage, unsigned 64-bit negative-one bits,
and NonUniform on the actual consumer. These pointer-memory GPU forms use the
standalone SSBO frame harness. Their BDA/PrivateArray variants and the large
array codec still have compilation/validation evidence only.

## Validation and artifact scope

| Check | Observation |
| --- | --- |
| Final canonical build and parent RT target | Pass; parent default all-target build is not claimed |
| Final offline suite | 37/37 pass, including pinned sources and new replay/rollback/concurrency checks |
| Sync submission host fixture | 50 cases pass in strict and NDEBUG configurations |
| Helper host and reduced-clock fixture | O2 pass, including observation failure, in-flight retirement, teardown retry/loss and saturation |
| Public GPU replay, both VVL orders | Pass with zero diagnostics; separate runs explicitly enable synchronization validation |
| GPU property formatter | 13 layouts across three GPU-built input updates pass, with native transfer readback |
| Typed frame/source/request/pipeline/BDA/runtime GPU selection | 6/6 pass; executable hashes remain unchanged through the final query teardown build |
| Explicit pointer-only GPU selection | All six forms pass with retained stdout and independent output checks |
| External guarded CTS before final teardown correction | 50/50 pass |
| External guarded CTS on final teardown DLL | 13/13 pass: all 10 selected compaction/serialization cases, plus library, UPDATE and the historical v3uint64 target |
| Additional matching native CTS controls | 18/18 pass; earlier 20 library and 12 UPDATE controls remain separate evidence |

Both software CTS snapshots have completed zero-rejection guard audits and zero
Validation Error, VUID or SYNC-HAZARD matches in stdout/QPA. They use the actual
CTS binary from D:/Codes/VK-GL-CTS, source revision
`d18e4c68543103371715845cd914c71e5859963e`. The runner retains the internal
30-second touch watchdog and an outer 120-second process timeout.

The 50-case snapshot's layer SHA-256 is
`085fc7f8fe221cca467a1bd1beb15434b0ff13d89138890dd73e804b83cc674f`.
The final 13-case snapshot's layer SHA-256 is
`88af24d458f9812c06afef8fb26332309270984037e4c96b6a9a9bd22b5879dd`.
These are different binaries; the 50-case result is not relabeled as a run of
the final DLL. Raw observations are retained in
`build-query-execution-2026-10-28` and the three
`build-rt-cts-evidence/query-execution-*2026-10-28` directories.

## Retained failures and remaining gates

The old committed layer rejects the new repeated/simultaneous query recording
fixture, retained in baseline-query-replay.log. The intermediate private-timeline
completion implementation is snapshotted separately and is no longer the
production retirement path.

The first signal-order control failed because it required a scoped signal
within five seconds while a gate remained closed; the native control timed out
too, and that process also reported a missing Intel GPA manifest. The corrected
test records early signaling as an observation, releases the gate, and requires
correct eventual completion. Both controls report no early signal on this GPU.
The CPU graph demonstrates an added semaphore-order edge, not a portable
early-progress guarantee. The [scheduler contract](../layers/raytracing/docs/QUERY_STAGE_SCHEDULING.md)
records the pinned normative constraints and remaining executor work.

The historical 226-case observation remains 220 pass, 2 NotSupported and 4
crashes. Its v3uint64 touch-watchdog target passed in both new software snapshots;
the earlier failure's cause and three startup exits are not declared fixed.
Earlier helper PASS raw-log paths were overwritten by actual later PASS runs;
the explicit teardown log note records this limitation. No failure log was
reconstructed or replaced.

Logical-stage execution, binary/external acquisition, remaining replay schemas,
native-query interactions and full resource/API/compiler qualification remain
open. Memory operands, supported source memory-model semantics, multiple real
descriptor roots and descriptor-array NonUniform need further execution work.
The [no-RT audit](rt_no_rt_qualification_gates_2026-10-28.md) identifies complete
software discovery/property/query gates and the production 4-GiB IR and
one-dimensional build-dispatch ceilings. The execution-only guard retains
native discovery; these runs are not masked-discovery, physical no-RT,
cross-vendor or POSIX qualification.
