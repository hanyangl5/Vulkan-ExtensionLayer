<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# Software OMM pause checkpoint

Development was paused at the user's request on 2026-11-11. This checkpoint
preserves the existing implementation on `codex/software-opacity-micromap`.
It does not complete the full EXT contract or any full milestone in the plan.
Default native pass-through and the experimental opt-in remain unchanged.

## Final pending change

The AS backend accepts device UPDATE as well as BUILD. It forwards the native
operation, source, destination and scratch sizes without converting UPDATE into
BUILD, and retains the source object for the recording lifetime. Each target
receives metadata rebuilt from the full explicit update description. Missing
OMM associations do not implicitly inherit the source metadata.

The added component regression exercises actual native BLAS and TLAS UPDATE,
both in-place and out-of-place. It covers ordinary geometry, unchanged OMM,
OMM replacement, opacity data changes and special-only associations; source
BUILD replay, UPDATE replay, source/input destruction and allocation-failure
handling are also checked. This fixture calls the internal backend and is not
a public loader or external CTS qualification of UPDATE.

## Verification at pause

- Integration build of `VkLayer_VEL_opacity_micromap` and
  `test_omm_acceleration`: passed.
- `ctest --test-dir build-omm-integration -R '^omm_acceleration$' -V`:
  passed, 2,161 assertions on NVIDIA GeForce RTX 3080, including five UPDATE
  variants and twenty native UPDATE commands recorded without BUILD conversion.
  VVL 1.4.366 and synchronization validation were enabled; no validation errors
  or synchronization hazards were reported.
- Raw component output is retained locally in
  `build-omm-integration/checkpoint-2026-11-11-acceleration.log`.
- The previous graphics checkpoint at `66f7bfd` recorded 69/69 selected external
  CTS cases passing in each layer order. Its immutable evidence remains described
  in [that report](omm_graphics_consumers_2026-10-29.md).
- External CTS was not rerun for the final UPDATE change: the previously used
  `D:/Codes/VK-GL-CTS` checkout and executable were absent at this checkpoint.
  Earlier CTS results do not qualify this new change.

## Remaining work

Production Ray Tracing Pipeline integration, integer-derived AS runtime lookup,
full AS copy/serialization, public UPDATE qualification and the remaining
lifecycle, synchronization and feature-interaction matrix are unfinished.
The full requirements remain in [the plan](opacity_micromap_plan.md) and
[status](opacity_micromap_status.md); no further implementation is scheduled by
this checkpoint.
