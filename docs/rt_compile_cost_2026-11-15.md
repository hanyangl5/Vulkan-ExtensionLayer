<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RTP compile-cost fix: November 15

Follow-up to the [RTP-only CTS run](rt_pipeline_only_cts_2026-11-15.md), which
recorded six 30 s-watchdog timeouts. This checkpoint explains the cost and
records the fix that removes them.

## Measurement

Opt-in instrumentation (`VKRT_PROFILE_COMPILE=1`) splits the first-execution
cost of a real CTS case
(`...acceleration_structures.operations.sparse_binding_structures.serialization.gpu_built.bottom_acceleration_structure.aabbs`):

| Phase | Before | After |
| --- | --- | --- |
| Per-stage typed compile (5 stages) | ~8 ms total | ~8 ms total |
| Scheduler GLSL compile (glslang) | 87.4 ms | 88.9 ms |
| Link stage libraries (`spvtools::Link`) | 57.2 ms | 55.2 ms |
| SPIR-V optimization | **4075 ms** | **59.7 ms** |
| Generated module | **118,195 words** | **40,697 words** |
| Runtime assembly total | 4307.6 ms | 293.1 ms |
| Native `CreateShaderModule` + `CreateComputePipelines` (warm driver cache) | 5.1 ms | 1.7 ms |
| Whole case, warm | 5.25 s | 1.13 s |

The dominant cost was `spvtools::Optimizer::RegisterPerformancePasses()` on the
~10^5-word generated module. It also *grew* the module to 118k words (the
targeted passes leave 40.7k). The measured native pipeline compile of the new
40.7k-word module was 1.85 s when its hash first missed the driver cache; the
baseline 30-47 s CTS timeouts are consistent with a cold compile of the
118k-word module plus the 4.3 s host assembly (the NVIDIA cache could not be
cleared safely to isolate that number, so the driver-side split is inferred,
not measured).

## Change

`BuildTypedRuntimeProgram` now runs a targeted pass list instead of the full
performance pipeline:

- `AggressiveDCE`, `Simplification`, `LocalSingleBlockLoadStoreElim`,
  `LocalSingleStoreElim`, `LocalAccessChainConvert`, `DeadBranchElim`,
  `AggressiveDCE`.

The intent is unchanged: remove aggregate parameter shuttles and dead codec
calls before native compilation. No fast-math or contraction passes are added;
source `NoContraction` and execution controls are untouched. Volatile sources
keep their existing no-optimization path.

Escapes and diagnostics:

- `VKRT_RUNTIME_OPT_FULL=1` restores the previous performance-pass pipeline for
  comparison, without changing code.
- `VKRT_PROFILE_COMPILE=1` prints per-stage and runtime phase timings to
  stderr. Both variables are inert by default.

## Verification

| Check | Result |
| --- | --- |
| Offline suite | **49/49 pass** |
| Software GPU matrix (`-L no_native_rt`) | **75/75 pass** |
| Typed RTP GPU matrix (`-L typed_rtp`) | **46/47**; only the documented native-control segfault (`physical_interfaces.native`, no layer) |
| External CTS, 60-case RTP selection, 30 s watchdog | **60/60 pass, 0 timeout, 0 fail** |
| Case latency in that run | average **1.17 s**, maximum **1.42 s** (baseline run: 6 timeouts at 30-47 s, max 47.4 s) |
| Downstream guard audit | **156,088 checks, 0 rejected**, 0 timestamp writes |
| Validation markers | 0 |

Evidence: `build-rt-cts-evidence/rtp-only-2026-11-15-opt-final`, per-case JSON
[`rt_cts_rtp_only_optimized_2026-11-15.json`](rt_cts_rtp_only_optimized_2026-11-15.json).
Layer DLL SHA-256:
`1bebc8c8a908654e1c10d283ad5ac99f4452a679eddc68fbc8383c56e65db8dd`.

## Limits

- The phase split is measured on one representative case; other cases differ
  in stage count and frame size.
- The optimized pass set is qualified by the local matrices and this 60-case
  slice, not by the full conformance suite.
- Large-launch frame memory, full resource budgets and physical no-RT
  qualification are unaffected by this change and remain open.
- The original 54 pass / 6 timeout run and its diagnostic rerun remain
  valid historical results; this checkpoint adds a later, different DLL.
