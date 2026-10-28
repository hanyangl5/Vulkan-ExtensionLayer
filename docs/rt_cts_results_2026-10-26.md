<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# External Vulkan RT CTS checkpoint — 2026-10-26

This delivery includes actual VK-GL-CTS execution, not only repository-authored
tests. It is a development smoke matrix, not a conformance submission or proof
of the complete AS/RQ/RTP goal. The [20-case inventory](rt_cts_smoke_cases.txt)
and [per-case machine-readable evidence](rt_cts_results_2026-10-26.json) retain
exact selections, commands, exit codes, reasons, executable/library hashes and
local raw QPA/stdout paths. No CTS source or expected result was changed.

## Configuration

- Source: `D:\Codes\VK-GL-CTS`, revision
  `d18e4c68543103371715845cd914c71e5859963e`.
- Executable: `build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe`.
  Its startup release string matches the source revision:
  `vulkan-cts-1.4.7.0-103-gd18e4c68543103371715845cd914c71e5859963e`.
  Binary SHA-256:
  `8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
- Existing build uses the Visual Studio 18 2026 generator, default target,
  Release executable. Unrelated checkout changes exist in `.gitignore` and
  untracked Python/build helper files; tested CTS module sources are unchanged.
- Windows x64, NVIDIA GeForce RTX 3080, driver 596.49, reported Vulkan 1.4.329.
  This hardware has native RT; a physical no-RT GPU was not tested.
- Native controls run without fallback or implicit layers. Fallback runs use
  one frozen manifest/DLL snapshot per run, with absolute library paths.
  Each exact case gets its own process, QPA and stdout file. Shader cache is
  disabled; watchdog and outer per-case timeout are 60 seconds. CTS Vulkan
  validation is disabled in this matrix; separate component validation does
  not substitute for a future CTS validation-layer run.

## Results

| Run | Selected | Pass | Fail | NotSupported | Crash/early abort | Timeout/no result |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Native controls, 18 + 2 cases | 20 | 20 | 0 | 0 | 0 | 0 |
| Latest RQ batch, frozen `rq-20261026-final-02` | 14 | 12 | 1 | 0 | 1 | 0 |
| RTP, `rtp-20261026-dynamicfix` | 6 | 3 | 3 | 0 | 0 | 0 |
| Targeted RQ empty-TLAS rechecks, identical DLL | 3 attempts | 3 | 0 | 0 | 0 | 0 |

Do not replace the latest RQ batch's crash with the successful rechecks. It
occurred before any stdout or QPA record, exit `3221225480` (`0xc0000008`), on
`ray_query.acceleration_structures.empty.traditional_structures.compute_shader.gpu_built.index_none.no_primitives_top`.
The same frozen DLL passes three isolated rechecks; the cause remains unknown.
The preceding full RQ batch also passed this case and reported 13/14 pass.
This is an unresolved reproducibility issue, not a supported skip.
Targeted Windows Application/WER checks found no matching crash report, so no
faulting module or stack was available. Empty logs do not prove that layer
entry points were never entered.

The latest RQ batch passes compute triangle/AABB flow, fragment and vertex flow,
intersection distance, custom instance index, committed SBT offset, candidate
AABB opacity, two opacity/cull cases, inactive-triangle emptiness and query
wrapper-function passage. Dynamic AS descriptor-array indexing fails during
lowering with an undefined pointer type. Its native control passes. This is
direct evidence for the remaining AS value/provenance work, not a waived case.

RTP passes raygen launch ID, device AS vertex update and empty TLAS. Miss/chit
launch ID and procedural Any-Hit hit-kind cases fail. Their stdout identifies
the legacy compiler's `Only location-zero vec4 ray payload is implemented`
restriction before CTS reports the failed pipeline object. Removing that first
restriction does not by itself prove these complete shader paths.

These CTS results exercise the current production prototypes: RQ's old Mesa
binary-IR traversal and RTP's legacy compiler/backend. The new shared BVH4 GPU
bridge has separate component execution evidence and is not yet the format
consumed by these CTS runs.

The source inventories contain 49,359 RQ and 22,677 RTP cases, including optional
features and configurations. The remaining inventory was not selected in this
first smoke matrix. It has not been declared unsupported or passed; later
iterations must expand applicable coverage and group it by implemented feature.

## Defects exposed and fixed by CTS

1. RQ declaration reordering placed `OpTypeImage` after pointers/functions.
   Image/sampler/opaque declarations now remain in the type section.
2. Function-storage queries passed through helper functions were addressed
   through Private pointers. Lowering now retains their real storage class.
   A compile regression additionally caught duplicate function types after
   pointer merging; deduplication now reaches a fixed point while preserving
   distinct aggregate layouts.
3. CTS enables advertised optional RT features on its default context. RQ now
   masks incompatible native RT options/dependencies and consumes emulated
   AS/RQ names/feature structures before creating the downstream compute device.
   It does not advertise new base support on a no-RT ICD yet.
4. RTP rejected the CTS Vulkan 1.4 feature chain. It now uses registry-matched
   pNext sizes and preserves unrelated fields, without duplicate storage16
   aggregate/individual prerequisite structures.
5. CTS supplies a nonnull but empty dynamic-state list. RTP now accepts the
   semantically empty form and still rejects unimplemented nonempty state.
6. Relative DLL paths caused Windows loader error 87 outside the library
   directory. Both runner snapshots and production RTP manifests now resolve
   absolute target paths; a repository-root-CWD regression covers this.

Earlier snapshot runs `rq-20261026-01` and `rtp-20261026-01` failed to load the
library because of the runner's relative-path bug. They are retained locally as
harness-error evidence and excluded from the final fallback result table. Other
intermediate failure logs are also retained; they must not be mistaken for runs
of the final DLL snapshot.

## Reproduction

Build the fallback first. Use the configured native toolchain/Python; on this
machine the Windows Store `python` alias is not a runnable interpreter, so the
bundled Python executable was used. Output directories must be new or empty.

```powershell
New-Item -ItemType Directory -Force build-rt-cts-evidence | Out-Null
Get-Content docs/rt_cts_smoke_cases.txt |
  Where-Object { $_ -like 'dEQP-VK.ray_query.*' } |
  Set-Content build-rt-cts-evidence/rq-cases.txt
Get-Content docs/rt_cts_smoke_cases.txt |
  Where-Object { $_ -like 'dEQP-VK.ray_tracing_pipeline.*' } |
  Set-Content build-rt-cts-evidence/rtp-cases.txt

python scripts/run_rt_cts.py `
  --cts D:/Codes/VK-GL-CTS/build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe `
  --archive-dir D:/Codes/VK-GL-CTS/external/vulkancts/data `
  --cases docs/rt_cts_smoke_cases.txt --mode native `
  --output-dir build-rt-cts-evidence/repro-native --timeout 60 `
  --cts-revision d18e4c68543103371715845cd914c71e5859963e

python scripts/run_rt_cts.py `
  --cts D:/Codes/VK-GL-CTS/build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe `
  --archive-dir D:/Codes/VK-GL-CTS/external/vulkancts/data `
  --cases build-rt-cts-evidence/rq-cases.txt --mode ray-query `
  --layer-dir build-rq-goal/layers `
  --output-dir build-rt-cts-evidence/repro-rq --timeout 60 `
  --cts-revision d18e4c68543103371715845cd914c71e5859963e

python scripts/run_rt_cts.py `
  --cts D:/Codes/VK-GL-CTS/build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe `
  --archive-dir D:/Codes/VK-GL-CTS/external/vulkancts/data `
  --cases build-rt-cts-evidence/rtp-cases.txt --mode ray-tracing `
  --layer-dir build-rt-frames `
  --output-dir build-rt-cts-evidence/repro-rtp --timeout 60 `
  --cts-revision d18e4c68543103371715845cd914c71e5859963e
```

Runner exit 1 means at least one failure/crash/timeout/missing result and does not
discard the logs. NotSupported is recorded separately and never counts as pass.
Future deliveries must retain failures and new selections under new run names,
then compare the same selections against native controls.
