<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# Native compute Ray Query consumer integration

The experimental software backend now runs unmodified public compute Ray Query
CTS shaders against native acceleration structures with software opacity
micromaps. This is a partial implementation of the full goal in
[the specification](opacity_micromap_layer.md). No complete milestone or extension
conformance is claimed. Native pass-through remains the default; discovery is
not extended to unsupported physical devices yet.

## Production path

Public AS creation retains the native handle/address and allocates an independent
RootCell. BLAS BUILD creates a checked GPU metadata body containing ordinary
geometry flags, normalized association and a self-contained micromap snapshot.
TLAS BUILD reads the application's actual GPU instance data, preserves logical
flags/identity, and passes a private sanitized instance copy to native AS build.
Root publication follows metadata construction; the final dependency joins it
with native AS build before consumers. Native BVH bytes are never read or
interpreted. Source and scratch reuse does not change
the captured OMM contents.

AS descriptors retain a coherent shadow pair of native address and RootCell.
Allocation uses prepare/commit/abort around native set allocation; valid updates
allocate no memory. Ordinary writes and ordered copies, variable counts, layout
aliases and compatible layout disturbance are tracked. Immutable bound-set
contexts are embedded in private pipeline variants as ordinary BDA constants.
They consume no additional application descriptor set or push-constant range.
Executable command buffers retain the variants, contexts, AS/property recordings
and secondary command-buffer dependencies they actually use.

SPIR-V composition imports resource-free descriptor/classifier helpers, propagates
AS provenance through legal scalar data flow and helper calls, then wraps native
query Proceed. Logical ray flags and AABB opacity are restored. Descriptor AS
loads fetch one coherent native/RootCell pair before converting the native
integer address to an AS value; opaque AS objects are never cast to integers.
Original modules, specialization data and layouts survive their application's
permitted destruction through owned copies. The native shader-module placeholder
is fully rewritten too, so stripping the EXT capability never leaves an invalid
constant Force2State operand for lower-layer validation.

Submission tracking retains AS writers before the native submit, uses a private
fence on the same submit when no application fence is present, and retires only
after overlapping writers complete. Blocking native submit/wait calls run outside
the tracker mutex. Private resources use concurrent sharing across physical
queue families. AS buffer dependencies include a global metadata memory barrier
because the private allocations lie outside the application's AS buffer range.

Compacted and serialization sizes come from actual GPU canonical headers, using
native timestamp queries for availability. Host and GPU result paths support
32/64-bit values, availability, partial/no-wait/wait, strides and reset. GPU copies
finish with a native buffer copy to preserve the public TRANSFER_WRITE scope.
Serialization size is the planned 32-byte UUID envelope plus active canonical
bytes; serialization execution and compatibility are still unfinished.

## Verification

LLVM 20.1.3, Vulkan SDK/validation 1.4.357.0, RTX 3080, driver 596.49 and Vulkan
1.4.329 were used. Component execution enables core and synchronization
validation. Current standalone results are 26/26 component CTests, with no skips
or failures: 12 offline and 14 native GPU. The public loader test separately
passes 54 checks in each validation-layer order. Parent configuration builds all
OMM targets and passes 12/12 offline tests; the host-only configuration passes
3/3 with native proofs disabled. A parent all-target attempt fails in unrelated
existing layers on Windows min/max macros; it is not reported as a successful
full-repository build.

New component evidence includes 230 AS-provenance checks, 1,495 descriptor checks,
178 consumer checks, 1,493 native AS assertions, 169 submission checks, 317 sync
checks and 44 native GPU query assertions. Consumer execution covers different
CB-bound contexts, destroyed original modules, specialization ID 0, layout
disturbance, cache churn and native triangle traversal. These component checks
supplement the external CTS rather than replacing it.

Independent integration review also fixed allocation escaping from graphics
query detection, the two-vector descriptor-bind OOM transaction, and derivative
batch handling when the base pipeline fails. A child is then compiled independently
without inventing an invalid native derivative base; flags2, feedback and early
return semantics are retained. One hundred BLAS/TLAS record/reset loops without
submission now release every abandoned private buffer/allocation. Unsubmitted
bodies have only recording ownership, while completed bodies acquire AS ownership
before submission tokens are released. GIPA also exposes software micromap hooks
when the downstream driver has none, matching GDPA without changing unrelated
procedure availability.

The external checkout is `D:/Codes/VK-GL-CTS`, revision
`d18e4c68543103371715845cd914c71e5859963e`; executable SHA-256 is
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
Each run snapshots the layer DLL/manifest and records exact commands,
environment, QPA/stdout, hashes, return codes and bounded timeouts.

| Raw run under `build-omm-cts-evidence` | Actual result |
| --- | --- |
| `as-software-checkpoint-above` | Final checkpoint DLL: 36 Pass, zero validation/sync errors |
| `as-software-checkpoint-below` | Same final DLL: 36 Pass, zero validation/sync errors |
| `as-software-reviewed-above` / `below` | 36 Pass in each order before final macro-format cleanup |
| `as-software-expanded-above` | 36 Pass, zero validation/sync errors |
| `as-software-expanded-below` | 34 Pass; last two processes exited before initialization with Windows `0xC000026B` during window-station shutdown |
| `as-software-level12-below-retry` | Both interrupted level-12 cases Pass, zero validation/sync errors |
| `as-software-smoke-below-final` | 8 Pass, 2 Fail at RTP shader creation with `VK_ERROR_FEATURE_NOT_PRESENT`; no invalid-SPIR-V error |
| `as-native-smoke-final` | 8 Pass, 2 native size-query failures at query-pool creation, matching the prior driver control |

The expanded list is [36 exact cases](omm_cts_rq_consumer_cases.txt): 34 compute
RQ and two size queries. It covers both state formats through subdivision level
12, nonzero base, opaque/disable/force-two-state controls, all special indices,
null micromaps, uint16 association and clone. Each level-12 render launches
16,777,216 rays. The two RTP-namespaced query cases contain no RTP shaders and
do not establish RTP support. The pinned CTS case named `copy.compact` actually
issues CLONE, so public COMPACT currently has dedicated API coverage only.

Failed first attempts remain available. `as-consumer-first` aborted at invalid
physical-device dispatch; passing downstream instance callbacks to private
backends fixed it. `as-consumer-physical-dispatch` passed two RQ cases but failed
query-pool creation before the property adapter existed. The first below-order
RTP smoke also exposed the stripped-capability/Force2State placeholder defect;
the final smoke records the corrected early unsupported result. Failures are not
relabelled as NotSupported or software passes.

## Required remaining work

The production path still needs integer-AS runtime lookup, graphics RQ, native
RTP instrumentation/SBT context and all exposed recursion/callable/indirect paths.
The provenance compiler accepts an integer resolver provider, but the runtime
provider is not connected. Shader objects, binary/identifier paths, AS UPDATE,
copy/serialization/relocation, array-of-pointers instances and generic AS creation
also remain unsupported. A recorded TLAS currently snapshots known BLAS roots;
BLAS objects created later need a correct GPU-time registry solution.

Query execution currently requires compute and timestamp support on its queue;
fallbacks and the full cross-queue availability matrix remain required. Protected,
sparse/external memory, buffer aliasing/tails, continuous-writer reclamation,
multi-device and the complete synchronization/feature-interaction matrix are not
qualified. Optional host/capture features remain false. None of these temporary
limits changes the complete mandatory goal.

Two concrete valid-input defects found in the same review remain open and are
next priorities: AS tracking currently queries the native address at creation,
which is too early when its buffer has not been bound; descriptor updates before
binding need deferred identity resolution as well. External fence payload import
or SYNC_FD copy-export is not integrated with retirement generations; exporting a
pending fence can reset the payload still being polled. Neither combination is
covered by the passing matrix or accepted as complete lifecycle behavior.

Reproduce component tests using `build-omm-integration` and the environment in
[the current status](opacity_micromap_status.md). For the public loader test use
its `omm/layers` directory, both explicit layer names and
`VK_VEL_OMM_EXPERIMENTAL_SOFTWARE=1`; reverse the layer order for the second run.
The external runner is `layers/raytracing/omm/tests/run_omm_cts.py`; use a fresh
output directory with the case list linked above. Exact source/artifact/evidence
hashes are in [the checkpoint manifest](omm_consumer_integration_2026-10-28.json).
