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

## M0 benchmark (implemented)

`tests/test_rtp_bench.c` plus `tests/rtp_bench.{rgen,rmiss,rchit}`: a 128-triangle
grid BLAS, one instance, an RTP pipeline whose raygen casts `spp` rays per
invocation, GPU timestamps around `vkCmdTraceRaysKHR`. A push constant selects
"hit plane" or "all miss" so traversal and hit-stage costs separate. It runs as
`vkrt.rtp_bench` (labels `gpu;benchmark`; not part of the conformance matrices):

```powershell
ctest -R vkrt.rtp_bench -V   # 256x256 x 64 spp = 4,194,304 rays, 5 timed runs
```

Measured on this host, layer active, 5 runs each (best):

| Mode | ns/ray | M ray/s | Meaning |
| --- | --- | --- | --- |
| loop only (no TraceRay) | **1.3** | 768 | the raygen hash/jitter loop itself |
| all miss | **32.7** | 30.5 | traversal + miss stage + payload chain, no hit group |
| hit plane | **68.8** | 14.4 | adds closest-hit stage, attribute/payload copies and the scheduler transition |

Repeatability is within ~2% run to run. Subtracting the loop cost, one
`TraceRay` costs about **31 ns** on a miss and **68 ns** when it hits, in a scene
whose BVH is only 128 triangles. The per-ray stage machinery, not the loop or
the arithmetic, is the cost centre.

## Workgroup-size experiment (reverted)

The typed runtime dispatches one *thread* per invocation with
`local_size = 1`, so a workgroup carries one thread and the GPU can keep only a
limited number of threads resident per SM. Raising the workgroup to 64
invocations (and dispatching `ceil(tile/64)` groups, with the launch mapping
fixed to count invocations rather than groups) was tried:

| Measurement | local_size = 1 | local_size = 64 |
| --- | --- | --- |
| benchmark all miss | 32.7 ns/ray | 47.0 ns/ray |
| benchmark hit plane | 68.8 ns/ray | 35.9 ns/ray |
| benchmark loop only | 1.3 ns/ray | 2.8 ns/ray |
| `ray_tracing_basic` | 24.6 fps | 9.2 fps |
| `ray_tracing_reflection` | 5.4-6.7 fps | 7.0 fps |

The results are mixed and, for the simplest sample, a 2.5× regression, so the
change was reverted. The likely explanation is per-thread resource pressure
(registers/local memory in the generated scheduler): packing 64 threads into one
group multiplies the group's resource footprint and can reduce resident groups
instead of increasing occupancy. Workgroup size alone is not a free win; M1 has
to reduce what each ray does, not just how many threads are in flight.

## Frame-zeroing experiment (reverted, but the largest measured lever)

`rt_alloc` zeroes every 16-byte-rounded node and stage frame on each TraceRay,
and the arena stride is about 2.1 KB for the benchmark pipeline. Zeroing only the
node header plus the traversal-state prefix (448 B) gives:

| Mode | full zero | header + prefix only |
| --- | --- | --- |
| hit plane | 68.8 ns/ray | **30.1 ns/ray** |
| all miss | 32.7 ns/ray | **14.2 ns/ray** |
| loop only | 1.3 ns/ray | 0.6 ns/ray |

That is a 2.3× win, and the benchmark's hit count stayed correct. However
`vkrt.rtp_api.physical_interfaces.software` and
`vkrt.rtp_api.memory_model_interfaces.software` then fail with mismatched output
words: the fixtures observe fresh-frame zeros (attribute/padding bytes and
locals beyond the prefix), so a zeroed frame is part of the layer's defined
stage semantics, not an implementation detail.

Vectorising the zero loop into 16-byte stores changed nothing (67.8 ns/ray), so
the path is bandwidth-bound: roughly 8.6 GB of zero writes for 4.19 M rays with
a 2.1 KB stride.

Conclusion: capturing this 2.3× requires making frame initialisation
*guaranteed by construction* rather than defensive — the compiler must prove
every frame byte is written before it is read (or shrink the frame), after which
`rt_alloc` can stop zeroing. That is the top M1 item.

## Next work

1. Extend the benchmark with a larger payload variant and a multi-hit variant
   (any-hit, shadow ray) so the payload copy and hit-stage costs are separable.
2. M1: remove the per-ray frame round-trip for register-sized payloads and
   re-run this benchmark plus the CTS 60-case slice and the two samples.
