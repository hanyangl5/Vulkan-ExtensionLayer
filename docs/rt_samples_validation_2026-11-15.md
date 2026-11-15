<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Vulkan-Samples RTP validation: November 15

Validation of the software RT layer against the ray tracing pipeline samples in
`D:\Codes\vksamples1`. The samples application was rebuilt locally in Release
(`build\app\bin\release\AMD64\vulkan_samples.exe`) so the sample registry and
every sample plugin match the checkout.

Host: NVIDIA GeForce RTX 3080 on Windows x64. The samples run windowed (the
installed loader/driver does not expose `VK_EXT_headless_surface`), with
`--stop-after-frame` and `--screenshot`, so each run terminates on its own.

Layer under test: `build-rtp-only`,
`VK_LAYER_VKRT_SOFTWARE_RAY_QUERY` plus `VK_LAYER_VKRT_NO_NATIVE_RT_GUARD`
(`--mask-discovery`), branch `codex/ray-tracing-pipeline-only`.

## ray_tracing_basic: PASS

| Run | Result | Screenshot |
| --- | --- | --- |
| Native | exit 0, 120 frames, no leaked device memory | `output/images/native-basic.png`, 78,208 B |
| Software layer | exit 0, 120 frames, no leaked device memory | `output/images/software-basic.png`, 78,396 B |

Image comparison (1280x720): mean per-channel difference **0.39**; only
**2,639 pixels (0.286%)** differ by more than 8 and all lie in the UI text box
`(23,122)-(266,144)`, where the two frame-time readouts differ. The ray-traced
image itself matches.

Downstream guard proof: 13,403 checked calls, **0 rejected**, 26 shader
definitions, one device created without native RT.

## ray_tracing_reflection: PASS (after a layer fix)

First run failed with `CreateRayTracingPipelinesKHR` =
`VK_ERROR_FEATURE_NOT_PRESENT`. A trace (`VKRT_TRACE_FILE`) showed a
four-stage, four-group pipeline whose continuation validation failed:

```
Invalid continuation input: ... member 0 contains an array with stride 28 not
satisfying alignment to 16 ... may be allowed if you enable the
scalarBlockLayout feature
```

`AnalyzeContinuations` validated application shaders without
`ValidatorOptions::SetScalarBlockLayout`, while every other validator in the
layer enables it. The virtual device always enables `scalarBlockLayout` (the
software runtime depends on it), so the input was valid. The fix enables
scalar and workgroup-scalar block layout in that validator
(`continuations.cpp`).

After the fix:

| Run | Result | Screenshot |
| --- | --- | --- |
| Native (5 frames) | exit 0 | `output/images/native-reflection5.png`, 111,389 B |
| Software layer (5 frames) | exit 0, no leaked memory | `output/images/software-reflection.png`, 112,450 B |

Image comparison: mean per-channel difference **0.335**, 0.26% of pixels
differ by more than 8. The sample animates its camera on a wall clock, so a
small drift is expected; the image is visually identical.

Guard proof: 1,607 checked calls, **0 rejected**, 26 shader definitions, one
device created without native RT.

## ray_tracing_extended: FAIL (device lost)

| Run | Result |
| --- | --- |
| Native | exit 0, BLAS/TLAS and frames complete |
| Software layer | **DEVICE_LOST** after TLAS build; app fail-fasts (`0xC0000409`) |

Software log: `BLAS Build duration 25.7 ms`, `TLAS Build duration 1.2 ms`, then
`Detected Vulkan error: ERROR_DEVICE` ~7 s later, on the first frames of the
Sponza scene. The guard recorded a clean device creation (13 checks, 0
rejected); no layer rejection appears before the fault, so this is a GPU-side
fault inside the fallback dispatch, not a boundary rejection. Native control on
the same scene passes.

This remains open: it needs a GPU debugger run (Nsight/Aftermath) or a reduced
Sponza-size reproduction to identify the faulting dispatch and the generated
module.

## Limits

- One host/driver; windowed runs; not physical no-RT qualification.
- `ray_tracing_position_fetch` and `ray_queries` samples were not run: the layer
  does not advertise those capabilities (ray query is out of scope by contract).
- Frame counts differ between some captures; where the camera animates on wall
  time, the image comparison includes that drift.

## Next work

1. Diagnose `ray_tracing_extended` device lost with a GPU debugger or a reduced
   reproduction (instance/geometry/group counts).
2. Re-run the three samples after each layer change; keep native screenshots as
   controls.
