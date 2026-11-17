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

## M1: four-invocation workgroups (landed)

The typed runtime used one *thread* per workgroup (`local_size = 1`), so each
workgroup carried one thread and memory-level parallelism per SM was minimal.
A workgroup-64 experiment was mixed (hit improved, but `ray_tracing_basic` fell
from 24.6 to 9.2 fps), so it was reverted and the size swept instead. The runtime
now uses **four invocations per workgroup**, with the layer dispatching
`ceil(tile/4)` groups and the launch mapping counting invocations
(`NumWorkGroups * WorkGroupSize`) rather than groups.

| workgroup | benchmark hit | `ray_tracing_basic` |
| --- | --- | --- |
| 1 | 56.3 ns/ray | 35.9 fps |
| 2 | 32.9 ns/ray | 58.5 fps |
| **4** | **18.5 ns/ray** | **73.5 fps** |
| 8 | 16.2 ns/ray | 44.9 fps |
| 64 | 35.9 ns/ray | 9.2 fps |

Four is the balance point: wider groups help the many-rays-per-thread benchmark
but lose on the one-ray-per-invocation sample, and very wide groups lose
everywhere, consistent with per-group resource pressure.

Final numbers with four-invocation workgroups:

| Measurement | before | after |
| --- | --- | --- |
| benchmark hit plane | 56.3 ns/ray | **18.5 ns/ray** (3.0×) |
| benchmark all miss | 19.8 ns/ray | **9.2 ns/ray** (2.2×) |
| `ray_tracing_basic` | 35.9 fps | **73.5 fps** (2.0×) |
| `ray_tracing_reflection` | 8.8 fps | **17.8 fps** (2.0×) |

Verification: offline 49/49 (the binding fixture now expects one dispatch group
for its two-invocation tiles), software GPU matrix 75/75, typed RTP 46/47 (the
known native-control segfault only), CTS 60-case slice **60/60**, screenshots
match native (basic mean channel difference 0.0, reflection 0.012).

Two measurement notes: the benchmark timestamps must be read with only the layer
active (the no-native-RT guard distorts the query values), and an accidental
native run of the same benchmark measured the hardware RT path at **7.2 G ray/s
(0.14 ns/ray)** on this scene.

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

## M1: word-wise payload/attribute copies (landed)

`rt_copy` moved one byte at a time with a read-modify-write per byte, for the
payload, the hit attribute, attribute relocation and the copy-back path. It now
takes a word path when source and destination share 4-byte alignment (the
common case) and keeps the byte path only for the unaligned tail.

| Measurement | before | after |
| --- | --- | --- |
| benchmark hit plane | 68.8 ns/ray | **56.3 ns/ray** (1.22×) |
| benchmark all miss | 32.7 ns/ray | **19.8 ns/ray** (1.65×) |
| benchmark loop only | 1.3 ns/ray | 1.3 ns/ray |
| `ray_tracing_basic` | 24.6 fps | **35.9 fps** (1.46×) |
| `ray_tracing_reflection` | 6.7 fps | **8.8 fps** (1.32×) |

Verification: offline 49/49, software GPU matrix 75/75, typed RTP 46/47 (the
known native-control segfault only), CTS 60-case slice **60/60**, screenshots
match native (basic mean channel difference 0.0, reflection 0.335).

## M1: stop copying the inherited system frame (landed)

Each activation copied the parent's 256-byte system frame and then overwrote the
per-activation fields, while `rt_hit_system` already writes the ray-specific
system fields for the stage being entered. The copy is removed; the allocator's
zero fill provides the defaults and only launch id/size, record, push address
and context are written per activation.

| Measurement | before | after |
| --- | --- | --- |
| benchmark hit plane | 17.9 ns/ray | **17.3 ns/ray** |
| benchmark all miss | 9.3 ns/ray | **7.6 ns/ray** (1.2×) |
| `ray_tracing_basic` | 71.5 fps | **81.6 fps** |
| `ray_tracing_reflection` | 17.9 fps | **19.9 fps** |

Verification: offline 49/49, software GPU 75/75, typed RTP 46/47 (known native
segfault), CTS 60-case slice 60/60, screenshot parity unchanged (0.0 / 0.012).

## M1: materialize only the system fields a stage reads (landed)

`rt_hit_system` used to write the whole hit system frame (ray flags, cull mask,
world/object rays, hit kind, instance/geometry/primitive ids, and both 48-byte
matrices) for every hit activation, whether or not the entered stage declared
those builtins. The typed compiler now records a `system_mask` per stage from
the input builtins it actually maps, the runtime emits it as `rt_sys_mask`, and
`rt_hit_system` writes only the requested fields.

| Measurement | before | after |
| --- | --- | --- |
| benchmark hit plane | 17.3 ns/ray | **16.75 ns/ray** |
| benchmark all miss | 7.6 ns/ray | **7.34 ns/ray** |
| `ray_tracing_basic` | 81.6 fps | **86.2 fps** |
| `ray_tracing_reflection` | 19.9 fps | **20.6 fps** |

Verification: offline 49/49, software GPU 75/75, typed RTP 46/47 (known native
segfault), CTS 60-case slice 60/60, screenshot parity 0.0 / 0.012.

## M1: zero only the observable frame regions (landed)

`rt_alloc` used to zero the whole 128-byte header plus the entire stage frame
for every activation. The allocator now has a raw variant, and `rt_push_shader`
zeroes only what can be observed before the stage writes it: the header, the
system frame plus traversal-state prefix (up to 304 bytes or the frame size), the
control block, and the full payload and attribute destination regions.

| Measurement | before | after |
| --- | --- | --- |
| benchmark hit plane | 16.75 ns/ray | **17.10 ns/ray** (noise) |
| benchmark all miss | 7.34 ns/ray | **6.73 ns/ray** (1.09×) |
| `ray_tracing_basic` | 86.2 fps | **95.4 fps** (1.11×) |
| `ray_tracing_reflection` | 20.6 fps | **20.1 fps** (noise) |

The fixtures that previously rejected a fixed 448-byte prefix now pass, because
the attribute and payload regions are zeroed wherever their offsets land. The
remaining unzeroed bytes are stage locals, which the frame codec initializes
before use. Verification: offline 49/49, software GPU 75/75, typed RTP 46/47
(known native segfault), CTS 60-case slice 60/60, screenshot parity 0.0 / 0.012.

## M1: no zeroing for activation headers (landed)

The activation header is fully written by `rt_push_shader` and the system frame
by `rt_system`/`rt_hit_system`, so the header and system/traversal zero fill is
gone from the activation path. Only the control block and the payload and
attribute destinations are zeroed now; `node+48/56` are always written so
`rt_writeback` never reads a stale payload length.

| Measurement | before | after |
| --- | --- | --- |
| benchmark hit plane | 16.75 ns/ray | **16.78 ns/ray** (noise) |
| benchmark all miss | 6.73 ns/ray | **5.91 ns/ray** (1.15×) |
| `ray_tracing_basic` | 95.4 fps | **105.2 fps** (1.10×) |
| `ray_tracing_reflection` | 20.1 fps | **20.5 fps** |

Verification: offline 49/49, software GPU 75/75, typed RTP 46/47 (known native
segfault), CTS 60-case slice 60/60, screenshot parity 0.0 / 0.012.

## Where the remaining time goes: scene scaling

The benchmark now takes an optional grid size (`cells`, default 8) so the
traversal cost can be separated from the per-activation cost. Same dispatch
(256x256, 64 spp), hit mode vs all-miss mode:

| grid | triangles | hit ns/ray | miss ns/ray |
| --- | --- | --- | --- |
| 1 | 2 | 9.54 | 6.02 |
| 4 | 32 | 13.47 | 5.92 |
| 8 | 128 | 16.80 | 5.89 |
| 16 | 512 | 20.23 | 6.04 |

The miss path is flat (the root box rejects the away-going rays), so the ~6 ns
floor is activation/push/pop cost. The hit path grows by about
**0.021 ns per triangle** over this range: at 512 triangles traversal is roughly
10.7 ns of the 20.2 ns hit cost, already more than the activation machinery.
For a real scene this term dominates, which makes the next target explicit: the
BVH4 walk (`sw_bvh4_first` / `sw_bvh4_after` re-test child boxes while walking
parent links) rather than more activation trimming.

## M1: skip redundant box tests in the BVH4 walk (landed)

`sw_traversal_proceed` and `sw_bottom_traversal` tested the box of every cursor
handle and then, for internal nodes, `sw_bvh4_first`/`sw_bvh4_after` tested the
children again. A local `trusted` flag now records that the handle was returned
by `first`/`after` (already box-tested) so the cursor test is skipped once per
descent.

| Measurement | before | after |
| --- | --- | --- |
| benchmark hit, 128 triangles | 16.80 ns/ray | **15.92 ns/ray** |
| benchmark hit, 512 triangles | 20.23 ns/ray | **15.99 ns/ray** (1.27×) |
| benchmark all miss | 5.89-6.04 ns/ray | 6.05 ns/ray |
| `ray_tracing_basic` | 105.2 fps | 105.0 fps |
| `ray_tracing_reflection` | 20.5 fps | **21.5 fps** |

The gain grows with scene size, as expected for a saved slab test per descent,
and it also flattens the 512-triangle case to the 128-triangle cost.

Verification: offline 49/49, software GPU 75/75, typed RTP 46/47 (known native
segfault), CTS 60-case slice 60/60, screenshot parity 0.0 / 0.012.

## Lazy hit-system loads (reverted)

Replacing the full `rt_load_q` in `rt_hit_system` with direct field loads from
the trace node measured 16.68 ns/ray (hit) and 7.24 ns/ray (miss), but the
compiler/GPU test `ray_tracing_fallback_typed_runtime` failed with "Shared BVH4
instance/object ray builtins changed": the raw offsets do not reproduce the
candidate/committed selection used by the `sw_get_intersection_*` helpers for
every stage kind. Reverted; the per-stage system mask (previous section) is the
version that ships.

## Zeroing ablation at four-wide workgroups

Re-measured after the workgroup change: removing the frame zero entirely takes
the benchmark to 7.2 ns/ray (hit, traversal broken) and 2.9 ns/ray (miss), so
zeroing still costs roughly 6 ns/ray on the miss path and more on hits.
Vectorising the zero loop to 16-byte stores changes nothing, and capping the
zero at 256 B only helps while corrupting traversal state. The remaining path to
that cost is compiler-guaranteed frame initialisation, as above.

## M1: no zeroing for trace nodes (landed)

`rt_begin_trace` allocated each 512-byte trace node plus its two attribute
buffers through the zeroing allocator. Every header word read from a trace node
is written by `rt_begin_trace`, `rt_trace_step` or `rt_report` before use, and
`sw_traversal_initialize_data` defines every traversal field read before a hit
sets it, so only the two attribute buffers are still zeroed.

Measured back to back in one session (best of 5, two runs each):

| Measurement | before | after |
| --- | --- | --- |
| benchmark hit plane | 13.96 ns/ray | **13.1 ns/ray** (1.07×) |
| benchmark all miss | 6.15 ns/ray | **4.78 ns/ray** (1.29×) |

Verification: offline 49/49, software GPU 75/75, typed RTP 46/47 (known native
segfault), CTS 60-case slice 60/60. The sample screenshots were not re-run for
this change.

## Next work

1. Extend the benchmark with a larger payload variant and a multi-hit variant
   (any-hit, shadow ray) so the payload copy and hit-stage costs are separable.
2. M1: remove the per-ray frame round-trip for register-sized payloads and
   re-run this benchmark plus the CTS 60-case slice and the two samples.
