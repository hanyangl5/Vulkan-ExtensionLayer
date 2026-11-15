<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Expanded RTP CTS: November 15

Expansion of the 60-case RTP-only selection
([first run](rt_pipeline_only_cts_2026-11-15.md)) to a stratified sample of the
full `dEQP-VK.ray_tracing_pipeline` namespace, run against the optimized
software runtime.

## Selection

[`rt_cts_rtp_expanded_cases.txt`](rt_cts_rtp_expanded_cases.txt): **1,405 exact
cases**. Built from the locally dumped namespace (22,711 cases total) by taking
up to 3 deterministically spaced cases from every depth-5 group. Excludes
`dEQP-VK.ray_query.*` (out of scope) and the optional
`opacity_micromap` / `linear_swept_spheres` namespaces, which the layer does not
advertise; those would only report NotSupported.

66 cases whose group prefix matched `large_shader_set`,
`misc.maxrtinvocations` or `misc.recursiveTraces` were run separately
(see below); the remaining 1,339 were split into six batches so a runner
restart could not lose more than one batch.

## Software result (1,339 cases)

| pass | fail | NotSupported | timeout | crash |
| --- | --- | --- | --- | --- |
| **917** | 33 | 386 | 3 | 0 |

Downstream guard audit across the batches: **1,472,224 checked calls, 0
rejected**, 0 timestamp writes; masked native-RT discovery active, Khronos
validation enabled, 30 s watchdog interval.

### Failures

| Count | Class | Meaning |
| --- | --- | --- |
| 12 | `shader_binding_table.indexing_miss` | SBT miss-record indexing is not implemented correctly. |
| 11 | `builtin.indirect.*` | `vkCmdTraceRaysIndirectKHR` returns `FEATURE_NOT_PRESENT`; `rayTracingPipelineTraceRaysIndirect` is not advertised, but these CTS cases do not gate on the feature bit. |
| 3 | `builtin.launchidext.ahit_*` | Any-hit semantics diverge for large launch grids. |
| 3 | `pipeline_no_null_shaders_flag.gpu.*` | Null-shader SBT entries with `RAY_TRACING_NO_NULL_*` flags are mishandled. |
| 3 | `watertightness.closedFan{,2}` | Watertight triangle rejection/intersection precision. |
| 1 | `builtin.hitkindext.chit_triangles` | Closest-hit `gl_HitKindEXT` value diverges. |

One of these (`limits.accel_struct_props`, not in the table) was fixed during
this run: the software profile clamped the update-after-bind acceleration
structure descriptor limits to its reserved budget instead of the Vulkan
500000 minimum. The case passes with the fix (commit `724918c`).

### Timeouts (30 s watchdog, all just over the limit)

`build.gpu.level_instances.triangles_65536_1_1` (32.1 s),
`builtin.launchidext.call_2048_4096_1` (32.3 s),
`watertightness.closedFan2.1024` (32.8 s). These are performance-bound, not
semantic failures.

## Heavy-group diagnostic (12 representative cases)

Run with a 120 s watchdog interval and a 300 s per-case budget:

| Result | Count | Cases |
| --- | --- | --- |
| pass | 9 | `large_shader_set.gpu.1024` (27.7 s), `misc.maxrtinvocations_{AABB,tri}` (110 s / 107 s), `recursiveTraces_{AABB,tri}_{12,13,14}` |
| timeout | 1 | `large_shader_set.gpu.4096` exceeded 300 s (shader-compile bound) |
| crash | 2 | `recursiveTraces_{AABB,tri}_15`: **DEVICE_LOST** at `vkWaitForFences` after ~5 s |

The depths 12-14 cases pass, so the depth-15 fault is a hard boundary, not a
gradual slowdown. It is consistent with a single software dispatch exceeding
the Windows TDR budget at that recursion depth; the layer advertises
`maxRayRecursionDepth = 31`, which is therefore not achievable for this scene
on this host. This remains an open limitation.

The other 54 heavy-group cases were not run in this pass.

## Native control (all 1,405 cases)

| pass | fail | NotSupported | timeout | crash |
| --- | --- | --- | --- | --- |
| **1,184** | 0 | 221 | 0 | 0 |

The smaller NotSupported count than the software run (221 vs 386) reflects
optional capabilities the native driver advertises and the layer does not
(SER, position fetch, capture replay, indirect AS/indirect trace, host
commands, and similar).

## Evidence

- Aggregated per-case record:
  [`rt_cts_expanded_2026-11-15.json`](rt_cts_expanded_2026-11-15.json)
  (software batches, heavy diagnostic, native control).
- Raw runs: `build-rt-cts-evidence/rtp-expanded-b{1..6}-2026-11-15`,
  `rtp-heavy-diagnostic-2026-11-15`, `rtp-expanded-native-2026-11-15`.
- Layer: branch `codex/ray-tracing-pipeline-only@724918c`; each run summary
  records the layer DLL SHA.

## Limits

- Stratified sample, not the full 22,711-case namespace; 54 heavy cases remain
  unrun.
- One host/driver; masked discovery, not physical no-RT qualification.
- The failures above are measured results, not environment exceptions.
- `dEQP-VK.ray_query.*` is out of scope by contract.

## Next work

1. Fix the SBT miss-indexing class (largest real failure cluster) and the
   `pipeline_no_null_shaders_flag` handling.
2. Decide the indirect-trace contract: implement `vkCmdTraceRaysIndirectKHR` or
   keep rejecting it and record the CTS consequence.
3. Reproduce the depth-15 device lost with reduced dispatch size to separate a
   real fault from TDR.
4. Re-run this selection after those fixes; expand from the stratified sample
   toward the full namespace.
