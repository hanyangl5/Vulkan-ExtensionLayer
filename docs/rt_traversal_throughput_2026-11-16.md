<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RTP traversal throughput: baseline and target (November 16)

Target set for this effort: **4.77 G ray/s on the RTX 3080**, i.e. the measured
HIPRT CUDA throughput on the same GPU. This page records the measured baseline,
the measurement limits, and the staged plan.

## Measured reference points

All numbers on this host (RTX 3080 + AMD Radeon integrated GPU), Windows x64.

| What | Workload | Rays | Time | Throughput | ns/ray |
| --- | --- | --- | --- | --- | --- |
| HIPRT CUDA, RTX 3080 | `AoRayCornellBox` 512², 512 spp × 32 AO | 4.295e9 | 901 ms | **4.77 G ray/s** | 0.21 |
| HIPRT HIP, AMD iGPU | same | 4.295e9 | 13,299 ms | 323 M ray/s | 3.1 |
| Layer, `ray_tracing_basic` | 1280×720 vs 2560×1440 slope | — | — | ~23–25 M ray/s | 39.9 |
| Layer, `ray_tracing_reflection` | 1280×720, 60 frames | 1.84 M/frame | 185 ms/frame | ~10 M ray/s | ~100 |
| Layer, CTS `launchidext.ahit` | 2.097 M invocations | 2.097e6 | +0.38 s over 1-invocation baseline | ~5.5 M ray/s | 181 |
| Layer, CTS `launchidext.ahit` | 8.389 M invocations | 8.389e6 | +1.34 s over baseline | ~6.3 M ray/s | 160 |

The HIPRT CUDA backend uses HIPRT's own template traversal
(`hiprt_device_impl.h`), not NVIDIA RT cores: it is a software traversal, so the
0.21 ns/ray reference is a like-for-like software comparison, although the
Cornell-box AO workload is the most favorable case (tiny scene, short coherent
rays, 16k rays/pixel).

Only the Cornell box mesh is available locally (the Bistro/Hairball LFS assets
are absent), so a heavier-scene HIPRT comparison is not possible yet.

## Per-ray cost structure in this layer

`rt_push_shader` in `shaders/typed_runtime.glsl` performs, for every
`OpTraceRayKHR`:

- a 128-byte node allocation plus the stage frame allocation from the arena,
- ~10 header word writes,
- a copy of the ray payload into the new frame,
- a copy of the hit attribute into the new frame,
- a scheduler transition (`rt_top = node`), and `rt_writeback` copies the
  payload back when the shader returns.

A shadow ray therefore costs on the order of a hundred bytes of GPU memory
traffic plus an arena allocation, before any BVH node is visited. Traversal
itself is scalar per lane over BVH4 nodes with the stack in the state block.
This continuation model is what makes the layer general (arbitrary stage
graphs, suspension, callables), and it is also the dominant per-ray overhead
for high-throughput tracing.

## Measurement blockers

- **Nsight Compute cannot profile the Vulkan dispatch here.** It profiles the
  HIPRT CUDA kernels normally, but reports "No kernels were profiled" for the
  Vulkan sample even with `--target-processes all` and no kernel filter.
- **Nsight Systems Vulkan tracing fails** on this host: the Vulkan extension
  JSON registration needs registry write permissions (administrative).
- Therefore M0 must measure with in-application timestamp queries, not with
  external GPU profilers.

## Plan

| Milestone | Work | Acceptance |
| --- | --- | --- |
| M0 | Dedicated benchmark fixture: timestamped `vkCmdTraceRaysKHR` over a fixed AS, ray loop per invocation, pure traversal (no shading) vs payload/shading variants; report ns/ray and M ray/s | repeatable within 5%, numbers committed |
| M1 | Remove per-ray payload/attribute frame round-trip where the payload fits registers; cut arena allocation from the hot path for leaf shaders | ≥3× M0 baseline, CTS 60-case slice + samples unchanged |
| M2 | Traversal rework: BVH8 or wider nodes, stack residency, node prefetch, any-hit early exit | ≥10× M0, `ray_tracing_reflection` ≥ 50 fps |
| M3 | Warp-coherent traversal/scheduler specialisation | reassess against 4.77 G/s |

The 4.77 G/s figure implies ~0.21 ns/ray aggregate; at 68 SMs that is a few
hundred warp-cycles per 32-ray warp. Reaching it requires the per-ray memory
traffic above to be essentially eliminated, not just tuned. M1 and M2 are
prerequisites for judging whether the final target is reachable in this
architecture.

## Next work

1. Build the M0 benchmark fixture with timestamp queries (device time only,
   excluding setup), and record it in this file.
2. Use it to separate traversal, payload/attribute marshal, and scheduler cost
   by compiling variants of the same raygen.
