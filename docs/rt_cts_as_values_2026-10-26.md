<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# External Vulkan RT CTS: AS values and workgroup metadata

Checkpoint: 2026-10-26, after base commit `4494570`. The production RQ fallback
and native controls both pass **21/21 selected real CTS cases with Khronos
validation enabled**. The rebuilt RTP prototype passes 3/6; the other three
still fail its legacy payload restriction. This does not complete the
[AS/RQ/RTP goal](ray_tracing_fallback_goal.md).

The [exact RQ selection](rt_cts_as_value_cases.txt) and
[machine-readable evidence](rt_cts_as_values_2026-10-26.json) record commands,
selected cases, exit codes, reasons, binary/manifest hashes and SHA-256 hashes
of local QPA/stdout logs. The [earlier report](rt_cts_results_2026-10-26.md)
retains the initial 20-case matrix and unexplained startup crash.

## Configuration and results

The unchanged external checkout is `D:\Codes\VK-GL-CTS`, declared revision
`d18e4c68543103371715845cd914c71e5859963e`. Its existing Release `deqp-vk.exe`
identifies the same revision and hashes to
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
The executable and tested CTS sources were not modified. Windows x64, RTX 3080,
driver 596.49, reported Vulkan 1.4.329; a physical no-RT GPU was not available.

| Frozen run | Validation | Selected | Pass | Fail | Skip/crash/timeout/no result |
| --- | --- | ---: | ---: | ---: | ---: |
| `native-as-values-final-validation-01` | Khronos enabled | 21 | 21 | 0 | 0 |
| `rq-as-values-final-validation-02` | Khronos enabled | 21 | 21 | 0 | 0 |
| `rtp-typed-emitter-01` | Disabled | 6 | 3 | 3 | 0 |

The final RQ run reports no API validation errors. The SDK validation manifest
and DLL hashes are included; validation uses the original SDK installation,
while each fallback DLL/manifest is copied into a frozen run directory.
RTP native controls for these same six cases passed in the earlier report;
this checkpoint reruns all six against the newly built RTP DLL.

RQ covers the earlier 14 cases plus AS function arguments, multiple queries in
compute and fragment stages, three dynamically indexed query-array variants,
and shared-memory consistency. The AS dynamic-indexing CTS uses both a
NonUniform descriptor-array access and integer AS-address conversion. These
runs execute the current Mesa binary-IR production RQ prototype. Shared BVH4
GPU encoding remains a separate component and is not its traversal format.

RTP passes raygen LaunchId, device AS vertex update and empty TLAS. Miss/chit
LaunchId and procedural Any-Hit hit-kind still fail:
`VEL RT pipeline: Only location-zero vec4 ray payload is implemented`.
The exact failed pipeline checks and stdout are retained. They are neither
skipped nor counted as supported passes.

## Fixes driven by CTS

1. **Actual AS values.** Each AS owns an eight-byte descriptor address cell.
   Lowering preserves scalar/array descriptor selection, uint64/uvec2
   conversion and helper/SSA passage, then stores the actual address in each
   query. Every TLAS access uses that query's root. AS creation allocates the
   cell without reading an unbound backing buffer; valid address queries or
   builds publish the immutable address once under a mutex. Descriptor updates
   allocate nothing and only bind the existing cell. Allocator, noncoherent
   flush/failure cleanup and enabled-queue-family sharing are covered by spies.
   Private downstream SSBO dynamic/nonuniform indexing and
   variablePointersStorageBuffer prerequisites are checked and enabled.
2. **Remove native RT declarations.** Enabling CTS validation exposed
   `VUID-VkShaderModuleCreateInfo-pCode-08742`: lowered modules still declared
   `SPV_KHR_ray_query` after the native device extension was consumed. The
   lowerer now removes obsolete RT extension declarations as well as types,
   operations and capabilities. Direct/grouped NonUniform decorations remain
   on the new descriptor member pointers.
3. **Preserve application workgroups.** The first expanded 21-case batch passed
   17 and failed four. Extracted real CTS SPIR-V showed both the application's
   WorkgroupSize (4, 48 or 64 invocations) and the traversal dummy entry's size
   1. The dummy metadata caused outputs beyond invocation zero to remain
   unwritten. Removing its WorkgroupSize and keep-alive SpecId decorations
   fixes all four cases. A regression preserves user LocalSize64,
   WorkgroupSize64 and user SpecId0; another rejects surviving RT declarations.

Intermediate failures, including the 17/21 batch and three validation failures,
are recorded separately in the JSON with raw-log hashes. The successful final
batch does not erase them. The older STATUS_INVALID_HANDLE startup crash did
not recur, but its root cause remains unknown. The independently confirmed
registry first-use race was fixed with shared platform once initialization;
there is no evidence that it caused that historical crash.

## Component checks and remaining work

RQ offline checks pass 9/9, and production traversal-helper, CPU AS fixture and
software GPU BUILD checks pass. Registry stress covers 32 threads and two
translation units in 20 fresh Release and 20 ASan processes; the original
unsafe header fails its delayed negative control. POSIX/TSAN was not executed.
Hidden cells share all enabled queue families, and obsolete unlocked AS build
recording metadata was removed. Dedicated physical multi-family execution is
still unqualified.

The full RTP repository regression passes 12/12. Its new typed SPIR-V frame
component generates typed codecs and per-suspension SSA spill/restore directly
from continuation plans. SPIRV-Tools and validation-enabled GPU execution cover
64 independent frames, recursively restored aggregate components, 257 runtime
Phi iterations, persistent local aliases and a late supplied suspension result.
This component does not yet lower the source CFG or replace the production
legacy compiler, so the three RTP CTS failures remain expected open defects.

AS address cells still consume an SSBO per AS descriptor and an allocation per
AS. Full application descriptor/allocation budgets, complete no-RT discovery,
shared BVH4 production consumers, refit, mandatory APIs and physical no-RT
qualification remain unfinished. These 27 unique selected RQ/RTP cases are a
small development slice of CTS, not a conformance submission.

## Reproduction

Build the fallback first; use the exact same case list for the native control
and RQ run. Output directories must be new or empty. On this machine Python is
provided by the bundled runtime rather than the Windows Store alias.

```powershell
python scripts/run_rt_cts.py `
  --cts D:/Codes/VK-GL-CTS/build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe `
  --archive-dir D:/Codes/VK-GL-CTS/external/vulkancts/data `
  --cases docs/rt_cts_as_value_cases.txt --mode ray-query `
  --layer-dir build-rq-goal/layers --output-dir build-rt-cts-evidence/rq-next `
  --validation-layer-dir C:/VulkanSDK/1.4.357.0/Bin --timeout 60 `
  --cts-revision d18e4c68543103371715845cd914c71e5859963e
```

Use `--mode native` and omit `--layer-dir` for the control. The runner disables
implicit layers, runs every exact case in a hidden separate process and records
all results, including nonzero failure exits. Validation is optional and
explicit; without `--validation-layer-dir`, CTS validation stays at its default.
