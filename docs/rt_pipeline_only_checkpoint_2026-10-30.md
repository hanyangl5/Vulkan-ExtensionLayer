<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RTP-only local qualification: October 30

Checkpoint for branch `codex/ray-tracing-pipeline-only` after the scope pivot
(`2fc4de8`). This record covers local build/GPU qualification done while the
external CTS corpus was unavailable on this host. It does not claim milestone
completion.

## What was verified

| Check | Command | Result |
| --- | --- | --- |
| Standalone build | `cmake --build build-rtp-only --parallel 8` | Builds (Release, SDK toolchain) |
| Offline suite | `ctest --test-dir build-rtp-only -L offline` | **49/49 pass** |
| Software GPU matrix | `ctest --test-dir build-rtp-only -L no_native_rt` | **75/75 pass** |
| Typed RTP GPU matrix | `ctest --test-dir build-rtp-only -L typed_rtp` | **46/47**, one native-control segfault |
| Parent layer target | `cmake --build build-source-layout/top-level --target VkLayer_VKRT_SOFTWARE_RAY_QUERY` | Builds |

The single `typed_rtp` failure is `vkrt.rtp_api.physical_interfaces.native`,
which runs with `VK_INSTANCE_LAYERS=` (no fallback layer). It segfaults inside
physical-pointer source pipeline creation on this driver. The pre-pivot status
already recorded this unresolved native observation; it is not a fallback
result and is not counted as a pass.

## Compile cost measured

End-to-end wall time of the public fixtures on the RTX 3080 host, including AS
build, pipeline compilation, dispatch and result checks:

| Fixture | Wall time |
| --- | --- |
| `test_rtp_api.exe` (raygen/miss/closest-hit + compute, one trace) | 5.3 s |
| `test_rtp_arenas.exe` (131 KB array live across two nested callables, 27 launches, three contexts) | 5.4 s |

The Ray Query era `26 s` typed compile and the callable watchdog timeouts
belonged to the removed `rtp_query_suspend` fixture and `dEQP-VK.ray_query.*`
cases. They are no longer representative; RTP fixtures are nowhere near the
30 s CTS watchdog on this host. Large-launch memory cost is still unqualified.

## Fixture and compiler-test repairs

These were required to get the pivot tree's GPU labels green; they are test
oracles, not product behavior changes:

- `state_compiler_tests.cpp`: identify the suspension boolean by the first
  `OpReportIntersectionKHR` result instead of the source OpName, because
  source cleanup may delete the named copy. Frame checks for `shadow` and
  `untouched` now detect that cleanup removed those variables and report the
  skip instead of throwing `std::map` key errors. Private-variable counts for
  the module-Private variants are one (the initialized payload); cleanup
  removes the load-only uninitialized word.
- `test_dsl_lifetime.c` and `test_shader_inline.c`: enumerate physical devices
  with the two-call query. The previous `count = 1` request returned
  `VK_INCOMPLETE` on a host with more than one ICD device.
- `test_shader_inline.c`, `test_software_discovery_gpu.c`, `shader_binary*`:
  removed Ray Query feature/extension requests and RQ shader sources.
- `test_shader_binary_immutable` and `test_shader_binary_profiles` were
  removed with their shaders. Their subject was Ray Query source/binary
  envelopes, and both fail identically **without the layer** on this host, so
  they cannot provide fallback evidence here. Shader-object transport remains
  covered by `vkrt.shader_objects*`, `vkrt.rtp_api.shader_object_mixed` and the
  RTP API matrix.

## External CTS: blocked on missing corpus

**Superseded on 2026-11-15:** the corpus was rebuilt locally and the run is
recorded in the [RTP-only CTS checkpoint](rt_pipeline_only_cts_2026-11-15.md).
The selection was narrowed from 74 to 60 cases, all
`dEQP-VK.ray_tracing_pipeline.*`; generic `dEQP-VK.pipeline.shader_object_*`
cases were dropped after the guard boundary rejected binary shader creation.

The required CTS checkout is not present on this host. `D:\Codes\VK-GL-CTS`,
`C:\VK-GL-CTS` and the usual SDK-registry locations do not exist; the only
`deqp-vk.exe` on disk is a 27-byte audit fixture. No CTS case can be executed
until the corpus is restored.

Prepared for the run:

- Case selection: [`rt_cts_rtp_only_cases.txt`](rt_cts_rtp_only_cases.txt),
  **74 exact cases**, all `dEQP-VK.ray_tracing_pipeline.*` (AS operations,
  pipeline libraries, refit/update, shader-object binary creation, data spill).
  No `dEQP-VK.ray_query.*` entry remains.
- Runner: `scripts/run_rt_cts.py` (mode `software-rt`, `--mask-native-rt-discovery`,
  `--zero-timestamps` optional depending on the run).

Exact command once the corpus exists:

```powershell
python scripts/run_rt_cts.py --mode software-rt `
  --cts D:\Codes\VK-GL-CTS\build\external\vulkancts\modules\vulkan\Release\deqp-vk.exe `
  --archive-dir D:\Codes\VK-GL-CTS\external\vulkancts\data `
  --cases docs\rt_cts_rtp_only_cases.txt `
  --layer-dir build-rtp-only\layers `
  --no-native-rt-guard-dir build-rtp-only\no-native-rt-guard `
  --validation-layer-dir build-shader-ownership-2026-10-29\validation-new\windows-x64-427606911 `
  --mask-native-rt-discovery --zero-timestamps `
  --output-dir build-rt-cts-evidence\rtp-only-2026-10-30 `
  --cts-revision vk-gl-cts@<rev> --layer-revision codex/ray-tracing-pipeline-only@<bin>
```

Native controls must be produced by the same runner with `--mode native`
against the same case list and recorded separately.

## Remaining work

1. Restore the CTS corpus and run the selection above; retain every result.
2. Re-run the same matrix on a physical device without native RT.
3. Qualify mandatory resource budgets, pipeline options and the remaining
   command schemas carried over from the pre-pivot status.
