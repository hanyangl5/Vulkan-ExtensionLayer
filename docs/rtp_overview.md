<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# PA RTP: software ray tracing pipeline layer

The single entry point for this layer: what it is, how to build and run it,
where it stands, and what comes next. The contract and exit criteria are in
the [goal](ray_tracing_fallback_goal.md). Design and ABI notes live in
[`layers/raytracing/docs`](../layers/raytracing/docs). Dated checkpoints and
JSON evidence records from October/November 2026 were folded into this page
and removed; they remain in Git history (last present at `6bc7c24`).

Last updated: 2026-11-17, branch `codex/ray-tracing-pipeline-only`.
**The complete fallback is not implemented.**

## What it is

`VK_LAYER_PA_RTP` implements `VK_KHR_acceleration_structure` and
`VK_KHR_ray_tracing_pipeline` on Vulkan 1.2+ devices without native ray
tracing. Applications keep their ordinary AS objects, SPIR-V shaders, SBTs and
`vkCmdTraceRaysKHR` calls.

| Part | Implementation |
| --- | --- |
| AS build | Mesa leaf / Morton / LBVH / PLOC / HPLOC on GPU compute, collapsed to a portable BVH4 (FP32 child bounds); refit, clone, compact, serialization |
| Traversal | Layer-owned Woop watertight ray/triangle test and BVH4 walk (`shaders/sw_traversal.glsl`), compiled into each RT stage |
| RT pipeline | Typed SPIR-V continuation lowering of the six RT stages into one compute program with a scheduler, BDA arenas, nested traces and callables |
| API | Virtualized discovery and device creation; RT extensions, features, stages and accesses are stripped before the driver |

Device requirements: Vulkan 1.2 with buffer device address, scalar block
layout, int8/16/64, **64-bit buffer atomics (`shaderBufferInt64Atomics`)**,
the Vulkan memory model and compute subgroup ballot. Without them the layer
reports `accelerationStructure`/`rayTracingPipeline` as false. Intel Xe-LP iGPUs
such as UHD Graphics 770 lack 64-bit atomics and are not eligible. The AS
builder (LBVH/PLOC/HPLOC/encode), query publication and the RTP arena allocator
use them.

`VK_KHR_ray_query` is out of scope: it is not advertised, and shader modules
that use it are rejected with `VK_ERROR_FEATURE_NOT_PRESENT`.

Layer identifiers (renamed on 2026-11-17 from `VK_LAYER_VKRT_SOFTWARE_RAY_QUERY`):

| Item | Name |
| --- | --- |
| Layer | `VK_LAYER_PA_RTP` |
| Library / manifest | `VkLayer_PA_RTP.dll` / `VkLayer_PA_RTP.json` |
| Enable / disable env | `ENABLE_PA_RTP=1` / `DISABLE_PA_RTP=1` |

## Build

Requirements: 64-bit C11/C++17 compiler, CMake ≥ 3.22.1, Ninja, Python 3, and a
full Vulkan SDK (glslc, spirv-val, spirv-link, SPIRV-Tools and glslang
libraries). SDK 1.4.363.1 is known to work. Older SDKs can lack the
`spirv/unified1` header layout or newer feature structure sizes.

From the repository root, in an x64 developer prompt with `VULKAN_SDK` set:

```powershell
cmake -S layers/raytracing -B build-raytracing -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-raytracing --parallel
ctest --test-dir build-raytracing -L offline --output-on-failure
```

Use Release: the SDK's SPIRV-Tools import libraries use the release CRT.
Multi-config generators (Visual Studio) also work. For separate
Vulkan-Headers, add `-DVulkan_INCLUDE_DIR=<checkout>/include` and
`-DVKRT_VULKAN_REGISTRY=<checkout>/registry/vk.xml`.

Output: `build-raytracing/layers/` (layer) and, with `BUILD_TESTING=ON`,
`build-raytracing/no-native-rt-guard/` (test-only guard layer).

## Run an application (e.g. Vulkan-Samples)

```powershell
$env:VK_LAYER_PATH = "<repo>/build-raytracing/layers"
$env:VK_INSTANCE_LAYERS = "VK_LAYER_PA_RTP"
vulkan_samples.exe sample ray_tracing_basic --stop-after-frame 120 --screenshot 119
```

On a GPU that has native RT, add the guard so the application cannot fall
through to the native implementation:

```powershell
$env:VK_LAYER_PATH = "<repo>/build-raytracing/layers;<repo>/build-raytracing/no-native-rt-guard"
$env:VK_INSTANCE_LAYERS = "VK_LAYER_PA_RTP;VK_LAYER_VKRT_NO_NATIVE_RT_GUARD"
$env:VKRT_NO_RT_GUARD_MASK_DISCOVERY = "1"
$env:VKRT_NO_RT_GUARD_LOG = "$PWD/guard.jsonl"   # optional: checked/rejected call counts
```

`VKRT_TRACE_FILE=<path>` writes a layer trace, useful when pipeline creation
fails. Applications must request Vulkan 1.1 or later.

External CTS: `scripts/run_rt_cts.py` (`--help` for the full interface) runs
exact case lists in isolated processes and records evidence. Case lists live in
[`scripts/rt_cts_cases`](../scripts/rt_cts_cases).

## Current state

### Local tests (RTX 3080, Windows)

| Matrix | Result |
| --- | --- |
| Offline (`-L offline`) | 49/49 |
| Software GPU (`-L no_native_rt`) | 75/75 at `e1057b9` (RTX 3080). On an Intel UHD Graphics 770 host, 4 `query_*` tests fail at `vkCreateDevice` and the `rtp_api` cases skip: that GPU has `shaderBufferInt64Atomics = false`, so the layer does not advertise AS/RTP there (see Device requirements) |
| Typed RTP GPU (`-L typed_rtp`) | 46/47; the failure is a native-control segfault without the layer |

### Vulkan-Samples (2026-11-15, with guard, 0 rejected calls)

| Sample | Result |
| --- | --- |
| `ray_tracing_basic` | Pass; matches native except the UI text |
| `ray_tracing_reflection` | Pass after the scalar-block-layout continuation fix |
| `ray_tracing_extended` | **DEVICE_LOST** after the TLAS build; native passes |
| `ray_queries`, `ray_tracing_position_fetch` | Not applicable (not advertised) |

### CTS (`dEQP-VK.ray_tracing_pipeline.*`)

- 60-case slice: 60/60, native control 60/60.
- 1,405-case stratified sample (2026-11-15): 917 pass, 33 fail, 386
  NotSupported, 3 timeout over 1,339 cases; 1,472,224 guard checks, 0
  rejected. Native control: 1,184 pass, 221 NotSupported, 0 fail.

Status of the 33 failures and 3 timeouts:

| Cases | Status |
| --- | --- |
| `shader_binding_table.indexing_miss` (12) | Fixed `9239ef6`: zero-stride SBT regions are valid |
| `builtin.hitkindext.chit_triangles` (1) | Fixed `a1c8fcb`: hit kind reads the committed hit |
| `builtin.launchidext.ahit_*` (3), `call_2048_4096_1` timeout | Pass after the traversal performance work |
| `pipeline_no_null_shaders_flag.gpu.tri_and_box.*` (3) | Addressed by `1537590`, not yet verified |
| `watertightness.closedFan{,2}.{256,1024}` (4) | Edge-ownership fill rule written locally, not yet committed or verified |
| `builtin.indirect.*` (11) | Open: `vkCmdTraceRaysIndirectKHR` is not implemented |
| `build.gpu.level_instances.triangles_65536_1_1` | Passes in 46.9 s; still over the 30 s watchdog |
| `misc.recursiveTraces_*_15` (2, heavy group) | Open: DEVICE_LOST |
| `large_shader_set.gpu.4096` (heavy group) | Open: over 300 s, shader-compile bound |

### Triangle fill rule (in progress, 2026-11-17)

`closedFan` fails if more than one fan triangle reports a ray through a shared
edge or vertex. The old Avalon-derived top-left rule tested world-space x/y and
summed per-edge votes. The branch does not build since `0861570`, which removed
`sw_lexicographic_compare` but still calls it. The planned replacement in
`sw_traversal.glsl`, written locally but not yet committed,
(`sw_fill_rule` / `sw_edge_owns`) works in the projected `kx`/`ky` frame of
the Woop test. For each zero edge `a -> b` with `d = b - a`, a positively
oriented triangle owns the point iff `d.y < 0 || (d.y == 0 && d.x > 0)`. A
negatively oriented triangle tests the reversed edge. A hit is accepted only if
every zero edge owns it. This equals evaluating every triangle at the same
infinitesimally perturbed point, so exactly one triangle of a closed mesh
owns a shared edge or vertex. The edge functions are `precise` so FMA
contraction cannot break their exact negation across a shared edge.

### Performance

The target is 4.77 G ray/s, the HIPRT CUDA software traversal on the same
RTX 3080. Measured with this layer: about 23–25 M ray/s
(`ray_tracing_basic`) and about 10 M ray/s (`ray_tracing_reflection`). The main
per-ray cost is the continuation model: each `TraceRay` performs an arena
allocation, a 128-byte node and payload/attribute copies before any BVH work.
Nsight Compute and Nsight Systems cannot profile the Vulkan dispatch on this
host, so measure with timestamp queries. The plan:

1. Add a benchmark fixture.
2. Keep payloads in registers and take arena allocation out of the hot path
   (≥3×).
3. Rework traversal: wider nodes, stack residency, prefetch (≥10×).
4. Specialize warp-coherent traversal and scheduling.

Compile cost: targeted SPIR-V passes replaced the full performance pipeline.
The generated module shrank from 118 k to 41 k words, and host assembly went
from 4.3 s to 0.29 s.

## Milestones

None are complete; see the [goal](ray_tracing_fallback_goal.md) for exit
criteria.

- [ ] M0: capability, no-native-RT, resource and continuation proofs
- [ ] M1: shared AS backend (Mesa builder, BVH4 encode/refit)
- [ ] M2: complete typed RT pipeline compiler and runtime
- [ ] M3: mandatory API, resources, synchronization, lifecycle
- [ ] M4: qualification on supported configurations and physical no-RT GPUs

## Next steps

1. Verify the open fixes: the four `watertightness.closedFan*` cases, the three
   `tri_and_box` cases, then the regression lists and the 60-case slice. Run
   the benchmark too, because the triangle test is on the hot path.
2. Decide whether to support GPUs without 64-bit buffer atomics (e.g. Intel
   Xe-LP): replace the 64-bit atomics with 32-bit/CAS-pair alternatives.
3. Replace the WIP commit `0861570`. Rewriting it needs a force-push;
   otherwise land the fix on top.
4. Diagnose the `ray_tracing_extended` and `recursiveTraces_*_15` device
   losts.
5. Implement `vkCmdTraceRaysIndirectKHR`.
6. Re-run the 1,405-case sample and update this page.
7. Qualify on a physical GPU without native RT and on a second vendor.
