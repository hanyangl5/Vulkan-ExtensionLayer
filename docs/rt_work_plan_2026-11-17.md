<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RTP fallback: working plan (November 17)

Branch `codex/ray-tracing-pipeline-only`. This page records where the branch
stands after the November 17 fixes and the ordered next steps. Status
evidence lives in the [status page](ray_tracing_fallback_status.md), the
[expanded CTS checkpoint](rt_cts_expanded_2026-11-15.md) and the
[throughput page](rt_traversal_throughput_2026-11-16.md).

## Current state

**The branch head does not build.** `0861570` is a work-in-progress save of the
watertightness fix; `sw_traversal.glsl` still calls the removed
`sw_lexicographic_compare`.

| Commit | Change | Verification |
| --- | --- | --- |
| `e1057b9` | Stop zeroing trace nodes | Benchmark miss 6.15 → 4.78 ns/ray, hit 13.96 → 13.1; offline 49/49, software GPU 75/75, typed RTP 46/47, CTS 60-case slice 60/60 |
| `9239ef6` | Accept zero-stride SBT regions | `indexing_miss` 12/12; 53 sampled `shader_binding_table.*` + 60-case slice, 0 failures |
| `a1c8fcb` | Closest-hit hit kind reads the committed hit | `hitkindext` 4/4; 271 sampled `builtin.*` + 60-case slice, 0 failures |
| `1537590` | Procedural hit groups serve triangle hits | Local matrices pass; CTS run interrupted at 36/252 (0 failures), the three `tri_and_box` targets not yet run |
| `0861570` | WIP: replace the triangle top-left rule | Does not compile |

Note on `e1057b9`: it exposed a latent bug. The closest-hit and any-hit paths
never set `want_committed`, so without the zero fill it was read from stale
arena memory. `a1c8fcb` initializes it explicitly; `e1057b9` on its own carries
the stale read.

### Re-run of the November 15 failure list at `a1c8fcb`

| Case(s) | Result |
| --- | --- |
| `builtin.hitkindext.chit_triangles` | pass (fixed by `a1c8fcb`) |
| `shader_binding_table.indexing_miss.*` (12) | pass (fixed by `9239ef6`) |
| `builtin.launchidext.ahit_{128_128_128,2048_4096_1,317_3331_1}` | pass; already passing at `e1057b9`, fixed by earlier performance work |
| `builtin.launchidext.call_2048_4096_1` | pass (1.8 s; was a timeout) |
| `build.gpu.level_instances.triangles_65536_1_1` | pass in 46.9 s, still above the default 30 s watchdog interval |
| `pipeline_no_null_shaders_flag.gpu.tri_and_box.*` (3) | fail; addressed by `1537590`, unverified |
| `watertightness.closedFan{,2}.{256,1024}` (4) | fail (one timeout); addressed by the WIP rule rewrite |
| `builtin.indirect.*` (11) | not run; `vkCmdTraceRaysIndirectKHR` is not implemented |

### Watertightness root cause

`closedFan` counts any-hit invocations per pixel and primitive. Each fan
triangle is its own geometry, so `gl_PrimitiveID` is 0 for all of them and a
ray through the shared centre vertex or a shared edge passes only if exactly
one triangle reports it. Misses are only a quality warning. The current rule
fails this in two ways:

1. It evaluates top-left on world-space `x/y` and `dir.x/dir.y` instead of the
   projected (`kx`, `ky`) coordinates the edge functions use, so the two
   triangles sharing an edge do not make complementary decisions.
2. It sums ±1 votes across the zero edges and requires a positive total. A
   vertex hit needs every zero edge to own the point (AND), as in
   rasterization.

Planned rule: for each edge with `e == 0`, take the projected edge direction
`d = b - a` and own the point iff `d.y < 0 || (d.y == 0 && d.x > 0)`, inverted
for negatively oriented triangles (`det < 0`). A shared edge is traversed in
opposite directions, or with opposite orientation, by its two triangles, and
both its edge function and `d` are exactly negated in floating point, so
ownership is complementary. Accept the hit only if every zero edge owns it.

## Next steps

1. **Finish the triangle rule** in `sw_traversal.glsl` (removes
   `sw_top_left_rule*`), rebuild.
2. **Verify both open fixes in one pass**: offline, `no_native_rt`,
   `typed_rtp`; the four `watertightness.closedFan*` cases; the 252-case
   regression list (`no_null_shaders_flag`, `shader_binding_table`, procedural,
   AABB, `hitkindext` and the 60-case slice); the benchmark, because the
   triangle test is on the hot path.
3. **Clean up history**: replace the WIP commit with a real one. That rewrites
   the pushed `0861570` and needs a force-push, so confirm first; otherwise
   land the fix as a new commit on top.
4. **Re-baseline**: re-run the full 1,405-case expanded sample and update the
   expanded CTS checkpoint and the status page. Expected open items: the 11
   indirect cases and the 65,536-triangle build time.

After that:

- **Correctness**: `ray_tracing_extended` device lost; implement
  `vkCmdTraceRaysIndirectKHR` and advertise
  `rayTracingPipelineTraceRaysIndirect`.
- **Performance**: the 65,536-triangle GPU build (46.9 s); the M1 items on the
  [throughput page](rt_traversal_throughput_2026-11-16.md) (register-resident
  payloads, compiler-guaranteed frame initialization).

## Operational notes

- `python` on `PATH` is the Windows Store stub and silently does nothing. Run
  `scripts/run_rt_cts.py` with the interpreter from `build-rtp-only/CMakeCache.txt`
  (`Python3_EXECUTABLE`).
- `docs/rt_cts_rtp_only_cases.txt` has CRLF line endings; strip `\r` before
  merging it with other case lists, or the runner rejects duplicates.
- Do not rebuild while a CTS run is in progress; the run loads the staged
  layer DLL from `build-rtp-only/layers`.
- On November 17, `vkrt.rtp_api.libraries.software`,
  `vkrt.rtp_api.library-stages.software` and
  `ray_tracing_fallback_pointer_qualified` failed (with a device lost) on every
  commit tested after `f6ed969`, then passed 10/10 later the same day at the
  same commits. Treat a sudden cluster of GPU failures as a possible host GPU
  state problem and re-run before bisecting.
