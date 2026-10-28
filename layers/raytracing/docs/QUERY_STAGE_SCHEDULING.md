<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Query logical-stage scheduling

Status: independently tested planning component; **not connected to native queue
submission**. `vkrt_stage_schedule.c` does not remove the query backend's unsafe
wait rejection. Command replay and per-execution query storage are necessary
building blocks, but they do not provide a correct native scheduling executor.

## The progress constraint

An AS property write executes logically at `ACCELERATION_STRUCTURE_BUILD`. A
query-result copy executes logically at `COPY`/legacy `TRANSFER`, even though its
private result formatter uses `COMPUTE_SHADER`. AS building and ray tracing also
use physical compute in this layer. Physical stage equality therefore does not
imply a source-program execution dependency.

Consider one source queue and a host-signaled timeline semaphore S:

1. Submission A waits for S at `TRANSFER`. Its commands write an AS property to
   query P, then copy P's result to a buffer.
2. Submission B, arriving later, independently writes an AS property to query Q.
3. The host waits for Q's availability, then signals S.

The source stage scopes leave B's AS write ungated by S. Promoting A's wait to
compute creates a cycle: S -> compute -> Q availability -> host signal S.
Splitting A immediately before its copy protects P, but still leaves the same
cycle if the suffix's unresolved compute wait is enqueued before B. This failure
does not require simultaneous command-buffer use, aliasing query pools, or reuse
of a query payload.

The first host test executes the legal feedback schedule and uses a separate,
small physical-engine event simulation to demonstrate the widened suffix's
deadlock. It is a progress oracle for the planner, not a claim about GPU behavior
or a substitute for CTS.

## Normative constraints used by the interface

Submission order alone creates no execution dependency. Wait scopes reach
commands in their batch and later submissions; source and destination stage
scopes include logically earlier and later stages respectively. The frontend
must expand those scopes before graph construction. See the Khronos
[synchronization chapter](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-pipeline-stages-order)
and [legacy submit scopes](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html).

Submit2 limits each signal's first scope with its stage mask. It cannot always be
implemented by waiting for whole-submission completion. See
[vkQueueSubmit2](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit2.html)
and [VkSemaphoreSubmitInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkSemaphoreSubmitInfo.html).

Binary waits consume a signal, and host counter queries apply to timeline
semaphores. The planner consequently makes no binary-readiness inference. Fence
joins must also preserve semaphore/fence signal ordering. See
[semaphore waiting](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-semaphores-waiting)
and [fences](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-fences).

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
block could suppress legal vertex-stage progress. Grouping native query scopes
or multi-stage indirect operations has the same rule. Rejection is independent
of whether a particular wait happens to be ready at graph construction time.

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

A correct future executor must obtain these acquisitions without placing an
unresolved wait on a physical stage used by logically ungated work. Possible
infrastructure includes nonblocking observation plus a proved-safe acquire
bridge, or genuinely isolated native execution resources with truthful queue
virtualization. Merely having an extra native queue on one development GPU is
not a supported-device predicate or a general solution. An internal completion
token must also identify the correct signal point; waiting for an entire
producer submission can recreate the prefix-progress deadlock.

Each binary semaphore must be consumed once. The executor can then fan out an
internal dependency token to multiple slices instead of issuing one binary wait
per slice. Imported semaphore payloads and external signal sources require an
explicit readiness/acquisition path. Until that exists, their unresolved waits
remain deferred or the source submission is rejected before native side
effects. A host boolean is not an acquisition certificate.

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
  gating or explicit rejection when the planner cannot split it.
- A state preamble for each independently replayable suffix: bound pipeline and
  shader objects, descriptor state and dynamic offsets, push descriptors,
  descriptor-buffer bindings/offsets, push constants, dynamic graphics state,
  vertex/index bindings, transform feedback, conditional state, and inherited
  secondary state as applicable. Starting a new render pass halfway through an
  old one is not state restoration: attachment load/store, transitions, subpass
  dependencies, and query scopes have observable effects.
- Exact per-submit wait and signal operation ordering, stage-scope expansion,
  external/binary/timeline signal-point tracking, fence joins, and native
  completion-token fanout. Source signals must not use a private full-submit
  fence when their source scope permits earlier signaling.
- Independently owned query payloads/markers and native replay CBs for each
  execution, with resource retention and failure rollback. Repeated secondary
  execution and recording reuse must not deduplicate these instances.

Private lowering barriers must be included in executor review. A broad private
`ALL_COMMANDS -> COMPUTE` dependency can spread a stalled native transfer stage
into a logically independent AS action even if the planner's source graph is
correct. The executor must preserve required private-resource dependencies
without adding those cross-domain progress cycles.

## Local validation

The standalone host test is `tests/test_stage_schedule.c`. It covers the timeline
feedback counterexample, later-submission wait history, AS/RTP/native-compute
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

No CMake wiring, native queue execution, GPU test, or CTS result is claimed by
this component. Runtime qualification must remain bounded until the missing
capture, acquire/progress, and retirement contracts are implemented and tested
against Vulkan workloads.
