<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# Software opacity micromap implementation status

Updated 2026-10-26. The active goal is the complete native-RT EXT implementation
in [the specification](opacity_micromap_layer.md), following
[the implementation plan](opacity_micromap_plan.md). No full milestone is complete.
No emulated extension, feature or Vulkan entry point is advertised by this checkpoint.
The second implementation checkpoint adds device-side storage/lookup and compiler
preparation; the complete application-facing adapter remains in progress.

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

The CPU evaluator reports 396,693 checks and the storage suite reports 786 checks.
Storage tests include truncated and corrupted blobs, size overflow, source-input
mutation/release, canonical padding and round trips. Planner arithmetic limits
are not advertised device limits. The internal storage blob is not the Vulkan
public serialization envelope. Object tracking and the public micromap commands
are still required.

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
The new compiler preparation pass separately saves logical flags and rewrites
`GetRayFlags`; actual GPU arrays/helpers/reinitialization pass in both capture-only
and candidate-exposure modes. Exposure is an internal preparation step that still
requires a candidate wrapper before general shader execution. Native OMM
capability removal and AS provenance are not yet integrated. Its logical AABB
opacity result is computed by the wrapper; the native getter is not mistaken for
the original logical value. Original logical instance flags in the sweep come
from fixture metadata; this does not test GPU-generated instance capture.

The native RTP proof passes 37 checks, including eight legal zero-group controls
and replacements. Opacity-culling guards execute before application Intersection
side effects. Its explicit BDA context uses one launch and trace depth; it does
not establish general SBT/context transport, nested traces or callable behavior.

Independent query graph tests pass 720 checks across 15 valid graph shapes and
four Vulkan-prohibited pointer graphs. Coverage includes Function/Private mixed
structs and nested arrays, helper signatures, signed flags, vertex, fragment and
ray-generation entry points. Array/helper/reinitialization behavior also executes
on the GPU. Mixed-struct runtime coverage is still open: this glslang build emits
an invalid UniformConstant object for the attempted GLSL struct fixture. The
invalid output is retained; valid hand-assembled struct inputs exercise the
compiler preparation without accepting that invalid frontend output.

## Verification and external CTS

Standalone CMake build with LLVM 20.1.3 and Vulkan SDK 1.4.357.0 passes all nine
integrated CTest targets. Parent VEL configuration builds the new targets and
passes four offline targets; the host-only configuration passes two without
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

The current RTX 3080 driver exposes native OMM with subdivision limits 12 for both
formats. A forced-emulation development selector will therefore be necessary to
separate software results from native pass-through.

## Open milestone gates

- [ ] M0: automatic AS provenance, resource limits, complete query state,
  procedural side effects, zero/null hit groups, native RTP context/SBT transport.
- [ ] M1: device object/storage API, public build recording/state preservation,
  complete association and input-lifetime/synchronization proof. Component GPU
  build and lookup now pass their isolated matrices.
- [ ] M2: native AS sidecars and general RQ SPIR-V instrumentation.
- [ ] M3: native RTP instrumentation, pipeline libraries, shader records,
  recursion/callables, indirect and GPU-generated SBTs.
- [ ] M4: public copy/query/serialization/compatibility and AS relocation,
  barriers/events/queues, replay, feature interaction and lifetime matrix.
- [ ] M5: complete supported-contract CTS/regressions and unmodified applications.

Next, implement descriptor/integer AS provenance and connect the query compiler
preparation to general candidate wrapping and the public API. Extend native RTP
context transport to nested/callable/indirect execution. Keep the shared
core independent of native BVH internals and the separate software RT backend.

## Reproduction

```powershell
cmake -S layers/raytracing -B build-omm -G Ninja `
  -DCMAKE_MAKE_PROGRAM=C:/FILES/ninja-win/ninja.exe `
  '-DCMAKE_CXX_COMPILER=C:/Program Files/LLVM/bin/clang++.exe' `
  -DCMAKE_BUILD_TYPE=Release -DVulkan_ROOT=C:/VulkanSDK/1.4.357.0
cmake --build build-omm --parallel 8
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_PATH = 'C:/VulkanSDK/1.4.357.0/Bin'
Remove-Item Env:VK_LAYER_ENABLES -ErrorAction SilentlyContinue
$env:VK_LAYER_VALIDATE_SYNC = '1'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
ctest --test-dir build-omm --output-on-failure
```

Use `-DOMM_BUILD_NATIVE_PROOFS=OFF` for the host-only core without Vulkan SDK/GPU
dependencies. The parent VEL configuration can opt in with
`-DBUILD_OPACITY_MICROMAP=ON`; it is off by default while the adapter is incomplete.
