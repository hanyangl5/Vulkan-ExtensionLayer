<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# Software opacity micromap implementation status

Updated 2026-10-29. The active goal is the complete native-RT EXT implementation
in [the specification](opacity_micromap_layer.md), following
[the implementation plan](opacity_micromap_plan.md). No full milestone is complete.
The experimental loader now connects public micromap build/copy/query operations,
native AS metadata and descriptor-sourced compute Ray Query consumers. Default
operation remains native pass-through. The layer does not add
OMM discovery to devices without native OMM while the full consumer contract is
unfinished. The complete application-facing adapter remains in progress.

## Workspace and separation

Development branch: `codex/software-opacity-micromap`, starting at the existing
design commit `f9b9a6cdb6d4f2d03bf07e2e7e06b50d9ea08aa3`. The managed worktree is
`C:/Users/hylu/.codex/worktrees/software-opacity-micromap/Vulkan-ExtensionLayer`.
The separate RT fallback checkout and its in-progress shader-binary changes are
untouched. All OMM implementation and fixtures are under `layers/raytracing/omm`;
there is no `vkrt_layer` hierarchy or fallback build dependency.

## Implemented components

| Component | Behavior and evidence |
| --- | --- |
| CPU opacity evaluator | Packed 2/4-state decode, special-index recognition at stored width before base addition, EXT implicit/uint16/uint32 association, logical opacity and culling |
| Barycentric mapping | Scalar bird-curve forward map, exhaustive independent CTS inverse centroids through level 9, deterministic samples through level 16, separate boundaries and invalid inputs |
| GPU opacity evaluator and lookup | Resource-independent GLSL, packed state/index association and bird mapping; executes at native query candidates |
| Storage | Checked 64-bit planning, canonical CPU reference, actual GPU-generated input and parallel device build, clone/input-lifetime regressions |
| Native RQ mechanism proof | Actual native BLAS/TLAS builds and traversal; transparent filtering, opaque auto-confirm, logical culling, unknown/force-two-state, ordinary triangles and AABB candidates |
| Native RTP mechanism proof | Native TraceRay, injected Any-Hit, procedural Intersection guarding, payload/attributes, Ignore/Terminate and legal zero triangle hit groups |
| Query compiler preparation | SPIR-V shadow state for query objects, fixed arrays/structs and function parameters; logical flags saved at initialization and restored by getter rewriting |
| Automatic RQ consumer | Typed parallel state, inline Proceed filtering and flags/AABB getter restoration; production descriptor provenance and bound-set pipeline variants now connect compute RQ |
| Native metadata component | Checked BVH-independent RootCell/TLAS/BLAS layout, normalized 64-bit association, GPU-owned OMM snapshots, native AS interception and GPU root publication |
| Packed classifier composition | Resource-free classifier closure linked into ordinary application SPIR-V; actual GPU packed-data/native-RQ chain passes 4,506 traces with explicit scene RootCell |
| Experimental public API | Public create/destroy/sizes/build/clone/compact, size queries, GPU serialization/deserialization and compatibility, buffer/state/synchronization translation |
| AS provenance compiler | Descriptor arrays, scalar AS phi/copy, integer conversions and helper argument/return propagation; compiler provider ABI passes 230 checks, integer runtime provider remains open |
| Descriptor contexts | Stable GPU-published identity cells, shadow ABI v2 null/unwritten/ready states, descriptor update-before-bind, ordinary writes/copies and immutable per-binding contexts; no extra application set or push range |
| Native AS lifecycle | Device BUILD for BLAS/TLAS, create-before-bind, GPU instance capture/sanitization, replay tokens and private-timeline per-AS retirement; update/copy/serialization remain open |

The CPU evaluator reports 396,693 checks and the storage suite reports 786 checks.
Storage tests include truncated and corrupted blobs, size overflow, source-input
mutation/release, canonical padding and round trips. Planner arithmetic limits
are not advertised device limits. The internal storage blob is not the Vulkan
public serialization envelope. The experimental device adapter now tracks
objects and connects public build/copy/query commands; GPU serialization, deserialization and compatibility are now connected. Complete
public lifecycle and feature-interaction qualification remains required.

Device build now runs two GPU phases: validate and plan records, then encode
payload words in parallel with an intervening compute dependency. Twenty-five
actual cases include GPU-generated records/data, unaligned strides, overlapping
input data, invalid-input canaries and a level-12 two-format 8,389,344-byte build.
GPU clone and source overwrite/destruction precede independent result checks.
Inputs require known word-readable backing; handling a legal application buffer
whose final logical byte has no readable word tail remains an adapter obligation.
Large triangle-record count scalability beyond the tested matrix is unqualified.

The GPU lookup suite consumes that actual device-built blob after destruction of
all build inputs. It passes 748,406 checks across levels 0-12, with independent CTS
inverse centroids, boundaries, association/special-index and corruption cases.
Association index data remains a separate live fixture input; native AS metadata
snapshotting and its input lifetime are not proved by this test.

The native proof runs 13 explicit scenes through each of descriptor-sourced and
integer-derived AS operands, then 1,000 CPU/GPU comparisons of state, ray flags,
logical instance flags and candidate visibility. The sweep has one possible
triangle intersection, making auto-confirm and visible-candidate counts exact
without assuming native traversal order. The mixed scene uses duplicate custom
instance indices; sidecar lookup uses the native instance ID instead.
An additional 96 cases obtain real native intersection barycentrics, decode a
canonical packed micromap on the GPU and apply the opacity decision. This smaller
native-query fixture uploads CPU-reference storage; the separate large lookup
suite exercises the GPU producer-to-consumer chain.

Metadata is accessed through BDA specialized into this proof's pipeline. It adds
no descriptor binding or push-constant range. The fixture still explicitly supplies
the association between the scene and its metadata. This is not yet an automatic
AS provenance transform, a full-layout resource proof, or a cache/binary ABI.
The candidate helper is manually written GLSL, not a general SPIR-V rewrite.

The manual proof observes rewritten native ray flags using the native getter.
The compiler preparation pass separately saves logical flags and rewrites
`GetRayFlags`; actual GPU arrays/helpers/reinitialization pass in both capture-only
and candidate-exposure modes. Exposure is an internal preparation step that still
requires a candidate wrapper before general shader execution. This older fixture
predates the production provenance/composition bridge. Its logical AABB
opacity result is computed by the wrapper; the native getter is not mistaken for
the original logical value. Original logical instance flags in the sweep come
from fixture metadata; this does not test GPU-generated instance capture.

The native RTP proof passes 37 checks, including eight legal zero-group controls
and replacements. Opacity-culling guards execute before application Intersection
side effects. Its explicit BDA context uses one launch and trace depth; it does
not establish general SBT/context transport, nested traces or callable behavior.

The automatic consumer transform now replaces ordinary source Proceed operations
with inline filtering, keeping query pointers out of new function calls. Twelve
valid graph variants and negative cases pass 116 checks. Actual GPU execution
passes 1,020 dispatches/2,040 traces, including two independent queries, arrays,
helpers, reinitialization, first-hit and AABB behavior. EXT declarations are
removed before execution on a logical device without native micromap enabled.
Its fixture callback consumes encoded CPU-oracle decisions; this does not prove
automatic AS provenance or the production packed-data classifier bridge.

The next component fixture now replaces that encoded callback with the real
packed-data classifier. It runs GPU canonical build, GPU association/snapshot
production, source-buffer destruction, classifier composition and automatic
Proceed filtering before 4,506 actual native traces. Metadata host tests pass
1,643 checks; the GPU producer passes 35 assertions and eight invalid-input cases,
including replay after active source size changes from 2608 to 1104 bytes.
That component fixture still explicitly supplies RootCells and CPU-known TLAS
identity metadata. The newer production integration separately implements
descriptor provenance, GPU-generated TLAS capture, root publication and private
allocation/retirement. Integer-derived AS runtime lookup remains required.
See [the metadata/classifier report](omm_metadata_classifier_2026-10-27.md) and
[exact CTS consumer bridge audit](omm_cts_consumer_bridge_2026-10-27.md).

The earlier loader/device checkpoint is documented in
[the 2026-10-27 report](omm_layer_api_2026-10-27.md). Its consumer-device admission
gate has now been replaced by operation-level guards. Explicit experimental mode
admits native RQ/RTP features and supports descriptor-sourced compute RQ through
the public API. Graphics RQ, RTP pipelines and shader objects still fail closed at
their respective creation boundaries. These are temporary implementation gaps,
not a smaller final goal. See the current
[consumer integration report](omm_consumer_integration_2026-10-28.md).

Independent query graph tests pass 720 checks across 15 valid graph shapes and
four Vulkan-prohibited pointer graphs. Coverage includes Function/Private mixed
structs and nested arrays, helper signatures, signed flags, vertex, fragment and
ray-generation entry points. Array/helper/reinitialization behavior also executes
on the GPU. Mixed-struct runtime coverage is still open: this glslang build emits
an invalid UniformConstant object for the attempted GLSL struct fixture. The
invalid output is retained; valid hand-assembled struct inputs exercise the
compiler preparation without accepting that invalid frontend output.

## Verification and external CTS

Standalone CMake build with LLVM 20.1.3 and Vulkan SDK 1.4.357.0 has passing
coverage for 27 non-loader component CTests (12 offline, 15 GPU). The first bounded
frozen attempt produced 25 Pass and two native-proof Timeouts; both passed a
targeted retry with unchanged executables. Original failures are retained. The
public loader test separately passes 93 checks in each layer order with both SDK
VVL 1.4.357 and the compatible VVL 1.4.366 nightly. Parent VEL configuration builds the new
OMM targets and passes 12 offline targets; the host-only configuration passes three without
Vulkan dependencies. The native GPU proofs pass with `VK_LAYER_KHRONOS_validation` and
`VK_LAYER_VALIDATE_SYNC=1`, with no validation or synchronization diagnostics.
The generated SPIR-V is checked with the SDK validator.

Evidence is retained under `build-omm` and `build-omm-proof`. The first native
proof run revealed missing BDA usage on the AS storage fixture; that setup error
was corrected. Both its original diagnostic log and the clean rerun remain.
An NVIDIA pipeline compiler failure on runtime 64-bit division in lookup was
isolated and replaced with exact split-product overflow arithmetic and alignment
masks. This preserves the 64-bit association contract. Failed and passing probes
remain in `build-omm-gpu-lookup`.
The checkpoint evidence and limits are summarized in
[the device component report](omm_device_components_2026-10-26.md).

The real external CTS checkout is pinned at
`d18e4c68543103371715845cd914c71e5859963e`. Ten selected EXT native-driver controls
produce **8 Pass and 2 Fail**, with no NotSupported, crash, timeout or missing
result. Both failures occur in micromap size-query cases at `vkCreateQueryPool`
with `VK_ERROR_UNKNOWN`; their cause is unresolved. These are native control
results, not software OMM qualification. Exact cases, executable hashes, raw
evidence location and reproduction are in
[the CTS inventory](omm_cts_inventory_2026-10-26.md).

The RTX 3080 driver is now 621.01/API 1.4.357, replacing the prior
596.49/API 1.4.329 runtime. It exposes native OMM; the software CTS matrix includes
level 12 in both formats. `VK_VEL_OMM_EXPERIMENTAL_SOFTWARE=1` explicitly selects the incomplete
development backend when the EXT extension is enabled. The mode is captured at
instance creation. Its software API fixture checks canonical storage magic to
confirm emulation. The earlier 596.49 native-through-layer CTS retained the 8 Pass/2 Fail
baseline. Current 621.01 software reruns pass 36/36 exact cases in each
validation-layer order with VVL 1.4.366: 34 compute RQ cases and two micromap size queries. In an
earlier below-order run, 34 passed and the final two level-12 processes could not
initialize during Windows logoff
(`0xC000026B`, empty stdout and no QPA), and both passed in a fresh retry. Keep
the original failures and retry evidence; the final full rerun passes all 36.
The software smoke matrix has 8 Pass
and 2 Fail: both actual RTP rendering cases still return
`VK_ERROR_FEATURE_NOT_PRESENT`. The size queries live in the RTP namespace but
exercise no RTP shader. The pinned CTS `copy.compact` case actually records
CLONE; it is not external COMPACT coverage. These results establish the tested
compute RQ path, not complete EXT conformance.

The first current CTS attempt with older SDK VVL 1.4.357 returned 36 InternalError
results because it does not recognize the newly advertised
VK_EXT_cooperative_matrix_maintenance1 feature structure. A native-only ordinary
triangle CTS control reproduced that error. A pre-existing official VVL 1.4.366
nightly was copied into this worktree for the passing reruns; the CTS source,
cases, validation categories and driver capabilities were not filtered or changed.
The same frozen OMM DLL is used in both failed and passing attempts.
See the [lifecycle/serialization report](omm_lifecycle_serialization_2026-10-29.md)
and its hash manifest for current results, runtime provenance and exact evidence.

## Open milestone gates

- [ ] M0: automatic AS provenance, resource limits, complete query state,
  procedural side effects, zero/null hit groups, native RTP context/SBT transport.
- [ ] M1: complete device object/storage API and association/lifetime proof.
  Public build/copy recording, state restoration and component GPU build/lookup
  now pass their scoped matrices; complete public lifecycle coverage remains open.
- [ ] M2: native AS sidecars and general RQ SPIR-V instrumentation.
- [ ] M3: native RTP instrumentation, pipeline libraries, shader records,
  recursion/callables, indirect and GPU-generated SBTs.
- [ ] M4: public copy/query/serialization/compatibility and AS relocation,
  barriers/events/queues, replay, feature interaction and lifetime matrix.
- [ ] M5: complete supported-contract CTS/regressions and unmodified applications.

Next, implement integer-derived AS runtime lookup and graphics RQ, then extend
native RTP context transport to general SBT, nested/callable/indirect execution.
Complete AS update/copy/serialization/relocation and the full
synchronization/lifetime matrix. The current AS registry snapshot also needs a
solution for BLAS objects created or bound after a TLAS command was recorded. Query
execution currently requires compute and timestamps on its command queue; the
fallback for other legal queues remains required. Keep the shared core independent
of native BVH internals and the separate software RT backend.

AS creation before ordinary buffer binding and descriptor update-before-bind are
now covered by real native AS/GPU regressions. External-fence payload generations
use private queue timelines when supported; fallback FD/Win32 transactions have
528 checks, while the complete no-timeline and real Linux external-handle matrix
remains unqualified. Sparse AS address eligibility has tracker coverage, not a
real QueueBindSparse/GPU proof. The new codec passes 81 checks at levels 6 and 12,
including actual equal-BDA aliases. See the lifecycle report for details. Abandoned
record/reset-only AS bodies, consumer bind OOM transactions, derivative batch
failure handling and GIPA software-entry availability are now fixed with
independent regressions.

## Reproduction

```powershell
cmake -S layers/raytracing -B build-omm-integration -G Ninja `
  -DCMAKE_MAKE_PROGRAM=C:/FILES/ninja-win/ninja.exe `
  '-DCMAKE_CXX_COMPILER=C:/Program Files/LLVM/bin/clang++.exe' `
  -DCMAKE_BUILD_TYPE=Release -DVulkan_ROOT=C:/VulkanSDK/1.4.357.0
cmake --build build-omm-integration --parallel 8
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_PATH = 'C:/VulkanSDK/1.4.357.0/Bin'
Remove-Item Env:VK_LAYER_ENABLES -ErrorAction SilentlyContinue
$env:VK_LAYER_VALIDATE_SYNC = '1'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
ctest --test-dir build-omm-integration -LE loader --output-on-failure
```

See the loader report for the explicit software API test environment. Running all
tests without that opt-in makes the loader target skip; it must not be counted as
a software pass. The current embedded kernels require Vulkan 1.2/SPIR-V 1.5;
qualifying a lower extension-only environment remains open.

Use `-DOMM_BUILD_NATIVE_PROOFS=OFF` for the host-only core without Vulkan SDK/GPU
dependencies. The parent VEL configuration can opt in with
`-DBUILD_OPACITY_MICROMAP=ON`; it is off by default while the adapter is incomplete.
