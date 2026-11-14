<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RTP-only external CTS: November 15

**Follow-up:** the [compile-cost fix](rt_compile_cost_2026-11-15.md) removes the
six timeouts below. With the optimized runtime the same 60 cases pass 60/60 in
an average of 1.17 s (maximum 1.42 s). This page keeps the original run and its
diagnostic rerun as history; the two runs use different DLLs.

First external Vulkan CTS run of branch `codex/ray-tracing-pipeline-only`
(`f6ed969`) after Ray Query was removed from the delivered contract. It
supersedes the "CTS corpus missing" blocker recorded in the
[October 30 checkpoint](rt_pipeline_only_checkpoint_2026-10-30.md).

## Corpus and selection

- CTS source: `VK-GL-CTS` `main` tarball fetched from codeload on 2026-11-15.
  The build has no git metadata, so QPA reports `releaseName unknown`
  (`releaseId 0xcafebabe`); provenance is the tarball plus the recorded binary
  SHA-256.
- Built locally: Visual Studio 18 Community, Ninja, Release, `deqp-vk` only,
  `-DDEQP_SKIP_VVL=ON -DDEQP_DISABLE_VK_VIDEO_TESTS=ON`.
  deqp-vk SHA-256: `ce2dbe54557472a3c0025ba2947f5c3488b23ac4271a982282ec72e939678254`.
- Selection: [`rt_cts_rtp_only_cases.txt`](rt_cts_rtp_only_cases.txt),
  **60 exact cases**, all `dEQP-VK.ray_tracing_pipeline.*`: AS operations
  (compaction/copy/serialization), UPDATE/refit, pipeline libraries, data
  spill, mem guarantee and launch builtins.
- `dEQP-VK.ray_query.*` is excluded by scope. Generic
  `dEQP-VK.pipeline.shader_object_*` cases were removed from the selection
  because the no-native-RT guard boundary rejects binary `vkCreateShadersEXT`
  as uninspectable; they are not RT-pipeline delivery cases.
- Layer: snapshot of `build-rtp-only`, DLL SHA-256
  `136523c2150ba8640032ca22e72c60c48d725753b8d3ebb51d8622d54b54ce74`
  (branch `codex/ray-tracing-pipeline-only@f6ed969`).
- Device: NVIDIA GeForce RTX 3080 (`0x10de:0x2216`), Windows x64.

## Software fallback result

Run: `build-rt-cts-evidence/rtp-only-2026-11-15`
(masked native RT discovery, zero timestamps, Khronos validation enabled,
30 s watchdog interval / 90 s total). Per-case JSON:
[`rt_cts_rtp_only_2026-11-15.json`](rt_cts_rtp_only_2026-11-15.json).

| pass | fail | NotSupported | crash | timeout | no result |
| --- | --- | --- | --- | --- | --- |
| **54** | 0 | 0 | 0 | 6 | 0 |

- Downstream guard audit: **148,881 checked calls, 0 rejected**, 0 timestamp
  writes, masked discovery active.
- No validation markers in any case log.
- The six timeouts are slow cases, not wrong results: serialization
  (bottom/top AS, AABBs), three UPDATE cases and `builtin.launchidext`.
  A separate diagnostic rerun with a 120 s watchdog interval completed
  **6/6 pass** in 4.77-7.00 s each
  (`build-rt-cts-evidence/rtp-timeout-diagnostic-2026-11-15`,
  [JSON](rt_cts_rtp_only_timeout_diagnostic_2026-11-15.json)). The original
  30 s-interval results are retained; the diagnostic run does not replace them.
  The gap between first execution (30-47 s) and rerun (~5 s) points at
  cold pipeline/driver-cache compilation cost in the layer, not at a semantic
  failure. It still needs a dedicated compile-cost investigation.

## Native control

Run: `build-rt-cts-evidence/rtp-only-native-2026-11-15` ([JSON](rt_cts_rtp_only_native_2026-11-15.json)), same 60 cases,
`--mode native` (no fallback layer, no guard).

| pass | fail | NotSupported | crash | timeout |
| --- | --- | --- | --- | --- |
| **60** | 0 | 0 | 0 | 0 |

## Exact commands

```powershell
python scripts/run_rt_cts.py --mode software-rt `
  --cts D:\Codes\VK-GL-CTS\build\external\vulkancts\modules\vulkan\deqp-vk.exe `
  --archive-dir D:\Codes\VK-GL-CTS\external\vulkancts\data `
  --cases docs\rt_cts_rtp_only_cases.txt `
  --layer-dir build-rtp-only\layers `
  --no-native-rt-guard-dir build-rtp-only\no-native-rt-guard `
  --validation-layer-dir build-shader-ownership-2026-10-29\validation-new\windows-x64-427606911 `
  --mask-native-rt-discovery --zero-timestamps `
  --output-dir build-rt-cts-evidence\rtp-only-2026-11-15 `
  --cts-revision "VK-GL-CTS main tarball 2026-11-15" `
  --layer-revision "codex/ray-tracing-pipeline-only@f6ed969"

python scripts/run_rt_cts.py --mode native `
  --cts D:\Codes\VK-GL-CTS\build\external\vulkancts\modules\vulkan\deqp-vk.exe `
  --archive-dir D:\Codes\VK-GL-CTS\external\vulkancts\data `
  --cases docs\rt_cts_rtp_only_cases.txt `
  --output-dir build-rt-cts-evidence\rtp-only-native-2026-11-15 `
  --cts-revision "VK-GL-CTS main tarball 2026-11-15"
```

## Limits

- Masked discovery is not physical no-RT qualification; this GPU has native RT.
- One host, one driver. No cross-vendor, POSIX or multi-device run.
- Six original timeouts are not passes; the diagnostic rerun is separate
  evidence.
- Passing this 60-case slice is not a conformance claim.

## Next work

1. Investigate the 30-47 s cold cost of the timed-out cases; per-pipeline
   compile caching is the likely target.
2. Expand the selection toward the full `dEQP-VK.ray_tracing_pipeline` suite
   and rerun after the cost fix.
3. Physical no-RT device and cross-vendor qualification.
