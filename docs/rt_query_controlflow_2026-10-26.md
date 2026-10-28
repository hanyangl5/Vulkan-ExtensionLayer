<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Public AS queries and expanded control-flow evidence

This is a worktree progress record after `b9cf53b`. The full
[implementation goal](ray_tracing_fallback_goal.md) remains active. In particular,
the bounded query route does not complete M4, and shader-object binary/device
compatibility retains demonstrated interface gaps. Historical failures remain evidence.

## External CTS observations

All cases use the actual `D:/Codes/VK-GL-CTS` checkout at
`d18e4c68543103371715845cd914c71e5859963e`, the same executable and resource archive
as the previous checkpoint. Each case runs in its own process, with a 60-second
limit, disabled CTS shader cache and Khronos validation. Layer snapshots and
binary hashes are recorded by the runner.

| Selection/run | Canonical pass | Fail | Crash | NotSupported | Native control |
| --- | ---: | ---: | ---: | ---: | --- |
| Former RQ compaction/serialization failure pair | 2 | 0 | 0 | 0 | Both passed previously |
| Expanded base RTP AS operations | 24 | 0 | 0 | 0 | 24 pass |
| New control flow, original `b9cf53b` | 36 | 10 | 0 | 0 | 46 pass |
| New control flow, Private backing fix | 46 | 0 | 0 | 0 | 46 pass |
| Complete RQ/AS 56-case first rerun | 53 | 0 | 3 | 0 | 56 passed at `b9cf53b` |
| Exact three crashed cases, same DLL snapshot | 3 | 0 | 0 | 0 | Available in prior 56-case control |
| Optional maintenance1 property-query selection | 0 | 0 | 0 | 28 | 8 pass, 12 fail, 8 NotSupported |
| Unified-source RT matrix (226 cases) | 224 | 0 | 0 | 2 | Prior exact controls match both skips |
| Final standalone RT smoke | 3 | 0 | 0 | 0 | Prior exact native controls |
| Top-level build RT smoke | 3 | 0 | 0 | 0 | Prior exact native controls |
| Shader-object binary interface | 2 | 2 | 0 | 0 | 4 pass |
| Final rebuilt DLL: RT smoke plus binary interface | 5 | 2 | 0 | 0 | Same interface failures |

The unified matrix concatenates the 56 RQ/AS cases, 52 state/spill cases,
48 execution cases, 46 control-flow cases and 24 AS-operation cases without
changing CTS. Its only skips are SBT alignment 1 and optional longVector, matching
the original native controls. No VUID/synchronization diagnostic appears in its
stdout. In this run all 56 RQ cases pass; the earlier 53-pass/3-crash run remains
unchanged. Binary null/different-layout imports fail with VUIDs 10383/10384 on
valid outer BINARY calls. Native controls pass both. These failures remain open.

The [24 AS cases](rt_cts_as_operations_cases.txt) cover ordinary/sparse backing,
BLAS/TLAS and triangle/AABB geometry with GPU copy, compaction and serialization.
The [46 control-flow cases](rt_cts_controlflow_cases.txt) are additional to the
previous 100-case RTP selection. The [28 property cases](rt_cts_query_cases.txt)
require optional ray-tracing maintenance1, which is deliberately not exposed.
Their skips do not count as passes. Twelve native pointer-count cases report
SDK validation VUID 12425 because they include BLAS objects in a query now
required to use TLAS objects; this is retained as an external test/validation
mismatch, without changing CTS or suppressing validation.

The three first-rerun crashes were consecutive cases with process exit
`0xc0000008` (STATUS_INVALID_HANDLE), empty stdout and zero-byte QPA files. They
occurred before any layer debug output. All three pass when rerun against the
identical snapshot. This does **not** make the first 56-case run a clean pass.
The historical early startup failure has therefore recurred and remains open.
Source inspection identifies pre-Vulkan WGL platform initialization before the
CTS crash handler as a hypothesis; it is not a proven cause. Windows debugger
diagnostics did not reproduce it: calibrated same-snapshot canonical reruns pass
30/30 and native controls pass 9/9, with no target first-chance exceptions. A
WGL-only process created 100 hidden contexts without a failure. An intentional
invalid CloseHandle calibrated the stack capture tool separately; it is not CTS
causal evidence. The original result and empty artifacts are unchanged. Raw
diagnostics are in `build-rt-cts-evidence/startup-debug/README.md` and
`diagnostic-summary.json`.

Raw evidence directories under `build-rt-cts-evidence/`:

- `query-public-first`, `as-operations-query-first`, `as-operations-query-native`
- `rtp-controlflow-b9cf53b`, `rtp-controlflow-native`, `rtp-controlflow-private-backing`
- `query-public-full56`, `query-public-startup-recheck`
- `query-types-first`, `query-types-native`
- `unified-source-cleanup-rt`, `unified-source-final-smoke`, `unified-source-top-level-smoke`
- `shader-binary-interface-native`, `shader-binary-interface-software`
- `unified-source-delivery-check`

The [machine-readable record](rt_query_controlflow_2026-10-26.json) retains exact
case outcomes, DLL/manifest identity and per-QPA/stdout hashes for these runs.

## Implemented query route

The [publication report](rt_query_publication_2026-10-26.md) defines the backend.
GPU compute reads the actual built AS header. Each recorded property/reset
operation owns a private immutable payload and native timestamp completion
marker. A device-to-host memory dependency, real sentinel compute action,
timestamp and native query WAIT form the acquisition chain; timestamp numerical
values do not encode properties. Legal non-simultaneous reuse harvests the old
execution before resetting private markers. Per-query GPU sequences preserve
publication order, and host results survive producer CB reset/free.

Submit rollback now affects only operations prepared by that call, identified by
the address of a stack transaction token. Failed preflight allocation cannot
discard old results. Query copies use private compute formatting plus native
transfer copies, preserving destination usage and stride gaps. Single-result
stride is correctly ignored. Protected/noncompute/timestamp-less pools and
active native query regions are rejected by this bounded route.

A query-result copy's private compute formatting must also observe the public
transfer wait. The bounded implementation rejects copy submissions after a
queue wait that does not cover private compute, including such waits in earlier
batches or calls, until successful QueueWaitIdle/DeviceWaitIdle. It does not
widen application wait stages. A real two-queue transfer-wait test passes native
controls and verifies the software rejection/retry boundary with zero validation
errors in both layer orders. Full stage scheduling remains required.

The public GPU fixture performs 64 repeated executions with concurrent host
reads and GPU-changing AS sizes. Its two-batch timeline test retrieves prefix
queries before signaling a host timeline that blocks the second batch of the
same QueueSubmit. This checks forward progress without a whole-submit fence or
an assumption of independent-stage overlap. Both validation orders pass with
zero errors; the latest runs contain 7,354 and 31,831 concurrent reader loops.
Host checks cover acquisition, cached reuse/reset, counter wrap, failure rollback
and private feature gating. Query-containing simultaneous recordings, complete
native-query interaction and logical-stage scheduling remain unfinished.

## Source and creation improvements

The [control-flow report](rt_cts_controlflow_2026-10-26.md) records typed
invocation backing for module-Private variables. Initializers run once at real
entry, resumes preserve state, and a reused frame starts a fresh activation.
Actual external failures involved Intersection globals across ReportIntersection.
The original 46 QPA modules contain no OpPhi instructions, so this slice does not
establish pointer-Phi coverage. The independent component tests execute numeric
Phi swaps and Private/Function state across two activations and 32 invocations.

The [pipeline creation report](rt_pipeline_creation_2026-10-26.md) records base
derivation and disable-optimization support. Both in-call base indices and
separate base handles are exercised, including destruction before dispatch.
Native compute variants receive the supported creation flags; optional runtime
performance passes are skipped when optimization is disabled. Caller pipeline
caches are forwarded only during the creation call, without keeping a handle
for later lazy variants.

Full pipeline creation options, pointer provenance, shader-object binary
compatibility, capability/resource qualification, simultaneous execution and
physical no-native-RT qualification remain open. No complete milestone follows
from these selected tests.

The [source consolidation report](rt_source_consolidation_2026-10-26.md) records
the unified source/build entry, dependency-based deletion of the old prototype,
24 offline checks, 29 GPU checks and five independent BVH4 checks. The large CTS
matrix snapshots the DLL before the Win32 parent-build macro adjustment. Separate
smoke and final seven-case runs retain each later DLL's own hash; final optional
header guards and the same-value result alias are followed by a rebuilt host/GPU
shader-object check. The evidence JSON records every completed run separately.
