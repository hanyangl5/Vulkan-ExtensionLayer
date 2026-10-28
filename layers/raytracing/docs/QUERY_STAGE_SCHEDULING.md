<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Query logical-stage scheduling

Status: independently tested planning component; **not connected to native queue
submission**. `vkrt_stage_schedule.c` does not remove the query backend's unsafe
wait rejection. Command replay and per-execution query storage are necessary
building blocks, but they do not provide a correct native scheduling executor.

## Scope hazards and progress limits

An AS property write executes logically at `ACCELERATION_STRUCTURE_BUILD`. A
query-result copy executes logically at `COPY`/legacy `TRANSFER`, even though its
private result formatter uses `COMPUTE_SHADER`. AS building and ray tracing also
use physical compute in this layer. Physical stage equality therefore does not
imply a source-program execution dependency.

Consider this dependency-model experiment with one source queue and a
host-signaled timeline semaphore S:

1. Submission A waits for S at `TRANSFER`. Its commands write an AS property to
   query P, then copy P's result to a buffer.
2. Submission B, arriving later, independently writes an AS property to query Q.
3. The host waits for Q's availability, then signals S.

The source stage scopes do not directly gate B's AS write on S. In the model,
promoting A's wait to compute adds a cycle: S -> compute -> Q availability ->
host signal S. Splitting A immediately before its copy still adds that cycle if
the suffix's unresolved compute wait is enqueued before B. This exposes an
extra dependency caused by physical-stage aliasing; it does not prove that the
host may rely on Q becoming available before S on every Vulkan implementation.

The first host test selects a progressing schedule permitted by its explicit
dependency graph and compares it with a widened-wait physical-engine model.
Neither a permitted schedule nor the absence of a graph edge establishes
bounded early signaling, concurrency, or unconditional queue progress. Each
source scenario must satisfy the pinned specification's forward-progress
requirements before it can serve as a mandatory correctness counterexample.

## Normative constraints used by the interface

The normative references here use Vulkan-Docs revision
[`d184375dcc5da2b06ca375a8d7d1f9d21ca64a76`](https://github.com/KhronosGroup/Vulkan-Docs/tree/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76),
matching the workspace's SDK 1.4.357 contract. Submission order alone creates no
execution dependency. Wait scopes reach commands in their batch and later
submissions; source and destination stage
scopes include logically earlier and later stages respectively. The frontend
must expand those scopes before graph construction. See the pinned
[synchronization rules][stage-rules] and [submit scopes][submit-scopes].

Submit2 limits each signal's first scope with its stage mask; this is a
dependency guarantee, not an early-signal deadline. The stage-substitution note
permits logically later source stages or earlier destination stages while
preserving dependency semantics; it does not justify arbitrary scope widening.
Whole-submission joins therefore require a separate equivalence proof. Timeline
wait-before-signal also does not remove the application's obligation to execute
the corresponding signal before forward progress. See
[VkSemaphoreSubmitInfo][signal-scope] and [Queue Forward Progress][queue-progress].

Binary waits consume a signal, and host counter queries apply to timeline
semaphores. The planner consequently makes no binary-readiness inference. Fence
joins must also preserve semaphore/fence signal ordering. See
[semaphore waiting and host operations][semaphore-waits] and
[signal operation ordering][signal-order].

The native control in
`build-query-execution-2026-10-28/signal-order-native.log` timed out before gate
release with both a null and non-null caller fence. This is not a software-only
failure or proof of an ICD defect. `tests/test_query_signal_order.c` now records
whether the scoped signal arrived early, accepts either early success or timeout,
releases the gate, and requires signal/fence completion and correct query/buffer
results afterward. Its CPU model demonstrates the added earlier-semaphore-signal
edge; it does not turn that edge into a portable early-progress test.

## Planner representation

The planner has four node kinds: wait, work, signal, and fence. IDs identify
executions. Inserting the same recording cookie twice produces two IDs. The
planner copies all input arrays; caller-owned recordings may subsequently be
reset without changing the graph. The caller still owns everything referred to
by the cookies and completion tokens.

Each work item describes an indivisible, independently replayable slice. It has
separate logical and physical stage masks, a replay/state cookie, and explicit
predecessor IDs. Its dependency and replay completeness certificates are
mandatory. No source-order edge is invented between unrelated work items.

Each wait retains its original API mask and a frontend-expanded logical
destination scope. Only individual stage bits appear in the latter: for example,
legacy `TRANSFER` expands to the enabled transfer stages. The frontend must also
apply the pipeline's logical-stage ordering and the enabled extension rules.
`TOP_OF_PIPE`, `BOTTOM_OF_PIPE`, `ALL_COMMANDS`, `ALL_GRAPHICS`, `VERTEX_INPUT`,
`ALL_TRANSFER`, and `PRE_RASTERIZATION_SHADERS` are rejected as unexpanded scope
bits. No stage-normalization implementation is hidden in this planner.

Insertion associates a work item with every preceding wait whose logical scope
covers that item. These associations persist across future appends, including
later submissions. A wait does not affect earlier work. Work may pass an older,
blocked item only if its own gates and explicit predecessors permit it. An
explicit dependency on a blocked copy correctly blocks an otherwise ungated AS
item too.

`vkrt_stage_schedule_signal` selects preceding work in the expanded logical
first scope and adds the frontend's explicit signal/wait ordering predecessors.
It preserves the original signal mask separately. The frontend must distinguish
signals in one batch from signals in earlier batches when supplying those
ordering predecessors. Generic exact joins remain available. Full joins include
every preceding node, including wait-only operations and signal nodes; they are
appropriate for full completion fences, not arbitrary stage-scoped signals.

The planner refuses any operation or signal scope that only partially covers
an indivisible work item. For example, a vertex-plus-fragment render-pass block
under a fragment-only wait returns `VKRT_STAGE_PARTIAL_SCOPE`; delaying the entire
block adds a gate absent from that source scope. Grouping native query scopes or
multi-stage indirect operations has the same rule. This rejection marks an
executor representation gap, not permission to reject these legal applications
in a complete fallback. Native rendering can remain intact with its original
stage gates; supporting that route requires stage-specific completion and
dependency information beyond the planner's current indivisible-work model.

## Emission and acquire tokens

`next` skips unresolved waits and returns an unissued work/join node whose
predecessors have emission tokens and whose waits have acquire tokens. Issuing
the node records its nonzero GPU completion token. It does **not** assert that
the GPU work has completed. The native executor must connect all dependency and
wait tokens to the emitted operation with real execution and memory
dependencies. This permits asynchronous GPU dependencies without payload
readback or host waiting for each action.

`acquire` has a stronger contract: its token certifies an already completed
native semaphore acquisition, including the required device memory dependency.
An observed timeline counter value alone is insufficient. This component never
enqueues the acquisition, reads a GPU result, waits for queue idle, or guesses
that a binary semaphore is ready. It cannot verify an executor's certificate.

A candidate first prototype observes timeline readiness without blocking, then
performs a real native wait on the same payload and value to establish the
device memory dependency. It must supply a valid acquire certificate, preserve
wait history across later submissions, and prove that existing physical waits
or whole-prefix completion do not introduce an extra gate. The counter
observation itself is not the acquire. This remains an executor proof obligation,
not a completed route from this planner to full transparency.

Isolated native resources may help particular devices, but an extra queue is
not a portable assumption. Internal completion tokens must identify the
required signal point, rather than silently replacing a scoped producer with a
whole-submission dependency. Private barriers and signal operations require the
same dependency review; a selected CPU schedule is not a native progress proof.

Each binary semaphore must be consumed once. A layer-owned producer may provide
a tracked signal token, which can fan out after acquisition instead of consuming
the binary payload once per slice. Arbitrary imported payloads have no general
host readiness query; they require a separately proved acquisition path. An
empty wait plus fence can inherit older work and is not automatically an
independent acquire bridge. Binary/external acquisition and partially gated
native blocks remain architecture gates. Rejecting legal inputs is only an
explicit intermediate limitation, not a final solution. No impossibility proof
for public Vulkan is claimed here.

The graph is intended to live with a bounded set of retained submission
records. It does not yet garbage-collect completed prefixes or own native
semaphores, fences, resource references, or command buffers. Native integration
must add lifetime/retirement handling and all-or-nothing submission admission.
The insertion methods are transactional individually, not across an entire
multi-batch queue submission.

## Capture and executor work still required

The replay journal currently supplies typed immutable command payloads and emit
callbacks. The scheduler needs a metadata extraction layer on top of that data:

- Logical action stages, lowered physical stages, and execution identities for
  each occurrence of primary and secondary work. Unknown stage semantics must
  fail admission instead of defaulting to `ALL_COMMANDS` or compute.
- Exact source/destination scopes and access masks for barriers and events,
  resource ranges, layouts, queue-family transfers, dependency flags, and
  implicit ordering edges. Cross-command execution dependencies need explicit
  graph edges; copying barrier bytes alone does not discover them.
- Native-query begin/end, conditional rendering, render-pass/subpass and dynamic
  rendering boundaries, plus inheritance and device masks. A native active-query
  or rendering scope must stay intact unless a separately proved transformation
  preserves its behavior. Native multi-stage work needs stage-granular native
  gating; current rejection when the planner cannot split it remains an
  implementation gap.
- A state preamble for each independently replayable suffix: bound pipeline and
  shader objects, descriptor state and dynamic offsets, push descriptors,
  descriptor-buffer bindings/offsets, push constants, dynamic graphics state,
  vertex/index bindings, transform feedback, conditional state, and inherited
  secondary state as applicable. Starting a new render pass halfway through an
  old one is not state restoration: attachment load/store, transitions, subpass
  dependencies, and query scopes have observable effects.
- Exact per-submit wait and signal operation ordering, stage-scope expansion,
  external/binary/timeline signal-point tracking, fence joins, and native
  completion-token fanout. Replacing source signals with a private full-submit
  fence join needs an equivalence proof, including signal operation ordering
  and the progress guarantees actually required by the pinned specification.
- Independently owned query payloads/markers and native replay CBs for each
  execution, with resource retention and failure rollback. Repeated secondary
  execution and recording reuse must not deduplicate these instances.

Private lowering barriers must be included in executor review. A broad private
`ALL_COMMANDS -> COMPUTE` dependency can spread a stalled native transfer stage
into a logically independent AS action even if the planner's source graph is
correct. The executor must preserve required private-resource dependencies;
adding a cross-domain dependency needs proof beyond physical stage equality.

## Local validation

The standalone host test is `tests/test_stage_schedule.c`. It covers the timeline
feedback dependency model, later-submission wait history, AS/RTP/native-compute
domain separation, blocked-predecessor propagation, exact COPY versus CLEAR
scopes, expanded full scopes, scoped signal first-scope joins, wait-only full
joins, partially covered native scopes, 130 simultaneous waits, duplicate
recording cookies, immutable inputs, and invalid/incomplete capture rejection.

On Windows with the SDK used by this workspace:

```powershell
& 'C:/Program Files/LLVM/bin/clang.exe' -std=c11 -Wall -Wextra -Werror `
  -I 'C:/VulkanSDK/1.4.357.0/include' -I layers/raytracing/src `
  layers/raytracing/src/vkrt_stage_schedule.c `
  layers/raytracing/tests/test_stage_schedule.c `
  -o build-source-layout/test_stage_schedule.exe
& ./build-source-layout/test_stage_schedule.exe
```

The planner is wired as the offline CMake test `vkrt.stage_schedule`. It does
not establish native executor, GPU progress, or CTS qualification. Runtime
qualification must remain bounded until the missing
capture, acquire/progress, and retirement contracts are implemented and tested
against Vulkan workloads.

[stage-rules]: https://github.com/KhronosGroup/Vulkan-Docs/blob/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/synchronization.adoc#L749
[submit-scopes]: https://github.com/KhronosGroup/Vulkan-Docs/blob/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/cmdbuffers.adoc#L1713
[signal-scope]: https://github.com/KhronosGroup/Vulkan-Docs/blob/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/cmdbuffers.adoc#L2041
[queue-progress]: https://github.com/KhronosGroup/Vulkan-Docs/blob/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/cmdbuffers.adoc#L2868
[semaphore-waits]: https://github.com/KhronosGroup/Vulkan-Docs/blob/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/synchronization.adoc#L4132
[signal-order]: https://github.com/KhronosGroup/Vulkan-Docs/blob/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/synchronization.adoc#L1875
