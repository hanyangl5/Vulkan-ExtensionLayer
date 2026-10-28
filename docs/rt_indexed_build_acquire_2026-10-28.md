<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Indexed construction and query acquisition, 2026-10-28

This checkpoint follows `a379681` on `codex/ray-tracing-fallback`. The complete
mandatory AS/RQ/RTP fallback and M0-M5 remain unfinished. All implementation,
shaders and tests remain in the canonical `layers/raytracing` tree.

## Construction

Production construction now writes an indexed v3 envelope. The Mesa scalar
header and record sizes remain unchanged; child IDs contain a record index and
two kind bits, decoded with 64-bit byte arithmetic. Internal storage starts
after the capacity-sized leaf region, including inactive holes. The encoder,
refit decoder and CPU reference retain v2 input support. Public BVH4 and
serialization formats are unchanged. See the
[v3 contract](../layers/raytracing/docs/IR_CONTRACT_V3.md).

Checked XYZ dispatch replaces the leaf/tree/bounds X-only admission limit.
Shaders flatten group coordinates before bounds checks; sort uses logical job
counts rather than padded grid dimensions. The maximum-count host checks cover
`0x1fffffff` leaves. Private build/copy/serialization barriers now describe their
actual compute accesses; public source dependencies retain translation.

The GPU address codec passes 21 reads, including 15 synthetic offsets above
4 GiB. Its safe 32-bit-truncation mutation fails. An initial negative harness
failure also reported leaked test buffers; those diagnostics remain retained,
and the corrected negative run releases buffers before checking the oracle.
Synthetic offsets are not physically allocated giant AS storage.

Public BUILD/RQ tests pass with artificial dispatch limits `(2,2,64)`, including
21 multidimensional dispatches among 1,068 checked dispatches. The observed
maximum grid is `(2,2,5)`; 10,218 downstream checks reject no native RT usage.
This special guard mode is explicitly excluded from ordinary CTS proof. Three
encoder, indexed-address and Mesa-builder GPU components also pass with their
validation option enabled. Large physical allocations, additional builder
variants and complete numerical qualification remain open.

## Query COPY acquisition

The former query-copy guard rejected legal narrow-stage waits even though the
formatter already recorded a native acquire-to-compute memory bridge. The new
public control first establishes that gap: native execution accepts all ten
cases, while the old software guard rejects eight and accepts two already
completed binary-history cases.

The final route keeps the original native semaphore waits and the COPY
formatter's `ALL_COMMANDS / MEMORY_READ|WRITE -> COMPUTE / SHADER_READ|WRITE`
barrier. Their intersecting scopes form an execution and memory dependency
chain. The following compute-to-transfer barrier protects the formatter output
before native buffer copying. No extra semaphore consumption, host readiness
query, queue, asynchronous source acceptance or ordinary queue/device idle is
needed for this route. The relevant fixed specification is
[Vulkan-Docs synchronization](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/synchronization.adoc),
including execution/memory chains and semaphore scopes; the
[submission scope rules](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/cmdbuffers.adoc)
also cover earlier waits extending to later submissions.

The obsolete rejection, unsafe-wait epochs, query completion checkpoints,
query-copy metadata walker, wait-only observer batches and dead copy flag were
removed. Per-queue source admission, per-execution query storage, real native
completion fences, settled/coalesced resource retention and OOM/UNKNOWN/LOST
handling remain. Journal metadata remains a separately tested component; it is
not a production logical-stage executor.

RESET preserves the implicit order after prior query COPY, including native
transfer completion across command buffers. Its private dependency is
`COMPUTE|TRANSFER -> COMPUTE`; PROPERTY remains compute-only. Actual public COPY
and publication host tests verify these contracts. COPY-only and RESET-only
compute-scope mutations fail their CPU oracles at both O0 and O2.

The public acquire matrix passes ten native and ten software cases, with
legacy/Submit2, same-batch/history waits, legally completed binary producers and
two-queue timeline publication. COPY intentionally omits WAIT_BIT; final values,
nonzero availability and padding are checked against a separately completed
host query. Host timeline release is unconditional, so early GPU progress is
not a correctness assumption. Both VVL orders pass the final software matrix
and repeated/overlapping query replay with synchronization validation and
complete zero-rejection downstream guard logs.

The GPU mutation narrowing the acquire bridge to compute-only still passes its
two cross-queue cases on this driver. It therefore supplies no evidence that
this GPU detects the missing scope. Its DLL and log remain retained. The
specification chain and failing host barrier mutation are separate evidence;
the GPU observation must not be presented as a successful negative control.
Earlier invalid-binary and outer-VVL rejected-submit fixture diagnostics are
also retained; final tests no longer depend on rejected-submit bookkeeping.

This removes the specific narrow-wait query-copy limitation. It does not prove
every public stage/access/event/ownership interaction. The separately tested
planner and fixed-time early-signal observations do not establish that a full
queue worker is necessary, or that all physical-stage coarsening is correct.

## Capabilities and typed memory

The public RT resource predicate now requires the selected query backend's
`hostQueryReset` and `shaderBufferInt64Atomics`. Missing either rejects RT
admission and masks its candidate discovery. Ordinary backend/shader-object
eligibility stays independent. Positive tests confirm private feature enabling
without requiring the application to request those internal features. Complete
software AS/RTP properties, extension/base-feature synthesis, queue contracts
and real no-RT admission remain open.

Typed pointer-frame loads/stores preserve applicable Volatile and Nontemporal
accesses; alignment is reproved for the repacked words. CopyMemory source and
target masks remain separate and native/persistent copies preserve complete
typed values and pointer identity. Two basic BDA GPU cases pass. Four enhanced
SSBO/BDA cases pass with real independent buffers and descriptor arrays; the
NonUniform cases use 16 invocations in one workgroup. These optional enhanced
requirements do not become universal fallback prerequisites.

The new large PrivateArray case does not pass. Exact, first-report-only,
zeroed-frame, production-optimized and no-memory-hint variants all return
DEVICE_LOST after successful native pipeline creation/submission. Native GLSL
controls with the same 32,994-word frame also fail, including a Logical
GLSL450 variant without BDA. Device-fault capture reports an instruction-pointer
fault and an invalid write for both the lowered and native controls. CPU
interpretation finds no observed out-of-bounds or uninitialized access, but is
diagnostic evidence only. This does not identify a driver defect or prove a
particular Private-memory limit.

Production RTP still uses PrivateArray, so this is a real outstanding delivery
gate. The next implementation must provide a scalable arena path, preserving
large values and execution isolation; passing BDA components alone does not
close production integration or justify deleting the failing case.

## Evidence and remaining work

Raw integration evidence is under `build-stage-execution-2026-10-28`; independent
CPU controls and their mutations are under `build-source-layout`. The frozen
pre-query-profile/cleanup DLL passes 50/50 external CTS with 50 complete guard
audits, 80,291 boundary checks, zero rejections and no scanned validation
diagnostics. Its SHA-256 is
`1479d4538898f8e17a774621808948f4f2f87f8d8613475cbf65c6984d7045bb`.
The later final-DLL run separately passes 13/13 relevant CTS with 13 complete
guard audits, 24,176 boundary checks and zero rejections. No validation
diagnostics are found by the recorded scan. Its evidence is under
`build-rt-cts-evidence/acquire-bridge-final-2026-10-28` and its DLL SHA-256 is
`3c4ed8ae1e3c240e38080fd4f147f895c2312b162491d3f1435fd99c3c7994eb`.
The canonical full build, 38/38 offline checks and parent RT target pass. The
unrelated parent default-build Windows macro failures from the preceding
checkpoint are not reclassified by a successful RT target build.

The [manifest](rt_indexed_build_acquire_2026-10-28.json) binds source, artifact
and evidence hashes. The four final public query runs add 4,560 downstream
checks with zero rejections; their two VVL orders explicitly enable
synchronization validation. The thirteen final CTS cases do not establish
that all fifty earlier cases were rerun against the final DLL.

All CTS runs use the user's `D:/Codes/VK-GL-CTS` checkout at declared revision
`d18e4c68543103371715845cd914c71e5859963e`. Execution-only guard mode retains
native discovery for case selection. It is not masked-discovery or physical
no-RT qualification. Older failures, including the historical 226-case matrix,
remain separate from these new observations.

The full goal still requires complete resource/capability contracts, scalable
RTP arena execution, mandatory shader/API and synchronization coverage, remaining
builder/numerical checks, broader CTS, physical no-RT hardware, other vendors and
POSIX. No milestone or full-goal completion is asserted here.
