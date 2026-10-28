<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Downstream native-RT rejection boundary

`VK_LAYER_VKRT_NO_NATIVE_RT_GUARD` is a test layer placed below the software RT
layer. It verifies the observed downstream calls of a particular test process.
It does not emulate a physical no-RT device or establish complete extension
support. The Vulkan loader and the actual device driver still execute ordinary
Vulkan operations.

Execution-only mode preserves native discovery so unmodified CTS can select RT
cases. The guard rejects native RT extension/feature requests, native RT entry
points, known RT SPIR-V capabilities/extensions/instructions/execution models,
and RT buffer/descriptor/query/stage/access use at the intercepted boundary.
Shader-module creation, shader-object SPIR-V and maintenance5 inline pipeline
modules are inspected. Opaque shader binaries, shader identifiers and pipeline
binaries fail closed because their instructions cannot be inspected here.

Set `VKRT_NO_RT_GUARD_LOG` to a unique writable JSONL path for each process. The
guard refuses instance creation without that evidence destination. A forbidden
result-returning API records a rejection and returns an error; forbidden void
commands record a rejection and terminate with exit code 86. Evidence-write
failure terminates with exit code 87. Neither rejection is a passing test.

Set `VKRT_NO_RT_GUARD_MASK_DISCOVERY=1` for the separate discovery-mask mode.
It removes native RT extensions and clears their feature/property outputs.
The software layer currently does not advertise complete emulation on such a
masked device, so unmodified CTS may report NotSupported. That result proves
the remaining discovery gap and does not count as an execution pass.

A successful execution-only proof needs the ordinary test result **and** a
complete guard log: zero rejection records/counters, completed instance
summaries, at least one created device and inspected shader definition. Each
record identifies the process and the current DLL load using a monotonic
timestamp plus a module-static address. CTS may unload the layer after discovery
and reload it before execution; the auditor accepts those distinct load segments
only within the same PID, with a separate start and balanced instances/summaries
in every segment. Counters must remain monotonic within each load. Repeated load
identities, mixed process IDs and truncated segments are rejected. At least one
complete segment must include the device and inspected shader work. Use
`no_native_rt_guard_audit.py`; process exit success alone is insufficient.
`scripts/run_rt_cts.py --no-native-rt-guard-dir ...` additionally freezes the
guard DLL/manifest and checks the per-case evidence. Layer and validation
placement remain part of each run's configuration.

## Artificial dispatch-limit test mode

Set `VKRT_NO_RT_GUARD_DISPATCH_LIMITS=2,2,2` to force small workgroup-count
limits for construction dispatch tests. The syntax is exactly three positive
decimal uint32 values separated by commas. Zero, signs, whitespace, overflow
and extra fields are rejected before instance creation. This mode cannot be
combined with discovery masking.

Legacy Properties, core Properties2 and KHR Properties2 clamp each reported
`maxComputeWorkGroupCount` axis to the smaller of its native value and the
configured value. Each created device independently retains those effective
bounds. Direct `vkCmdDispatch` and core/KHR `vkCmdDispatchBase` check all three
axes, including checked base-plus-count bounds, before forwarding. An oversized
dispatch records a rejection and terminates with exit 86. Indirect dispatch is
rejected in this mode because the guard cannot inspect GPU-supplied counts.
The ordinary mode keeps its original forwarding and native-RT rejection policy.

Forced runs use the distinct `forced-dispatch-limits` log mode, with immutable
`dispatch_limits`, monotonic `dispatches` and `multidimensional_dispatches`, and
per-axis `dispatch_max` group counts. The default auditor and CTS runner reject
this mode. Use an explicitly expected profile for the dedicated test:

```sh
python layers/raytracing/tests/no_native_rt_guard_audit.py /absolute/test.guard.jsonl \
  --expected-dispatch-limits 2,2,2
```

The dedicated audit requires matching limits in every load segment, valid
counters/maxima, zero rejections and at least one checked dispatch in an
executing segment. Its result reports observed dimensions so a GPU fixture can
also require that multidimensional dispatch actually occurred. A zero-dispatch
process, a different requested profile or forced fields relabeled as ordinary
execution evidence is rejected. Existing schema-2 execution-only logs remain
valid without any artificial-profile fields.

These intentionally tiny limits are below Vulkan's mandatory minima. They
exercise chunking and shader bounds using small allocations; they do not
establish an ordinary Vulkan device profile, mandatory-limit support, physical
no-RT qualification or normal CTS conformance. The GPU test's independent output
oracle and ordinary-limit regression must be checked separately.

The C host control loads the same negotiation/GIPA/GDPA interface against a
mock downstream driver. It checks positive forwarding, 13 blocked attacks,
feature/property/extension masking, and a fatal native-command control. The
Python controls also verify a discovery/reload sequence and reject thirteen
malformed/incomplete evidence variants. Additional controls cover checked
configuration parsing, consistent property clamping, direct/base dispatch,
native-limit intersection, fatal oversized/indirect dispatch, empty-dispatch
evidence and dedicated-profile audit mutations:

```sh
python layers/raytracing/tests/no_native_rt_guard_checks.py \
  --executable /absolute/build/test_no_native_rt_guard \
  --output /absolute/build/no-native-rt-guard-controls
```

`no_native_rt_guard_generate.py` pins the native-entrypoint and feature tables
to the Vulkan registry, and opcode/capability/model tables to the SPIR-V core
grammar. Their source SHA-256 values are in the generated header. Regeneration
is an explicit SDK update, followed by repository clang-format on the generated
header/include. This boundary's coverage is the pinned registry and named
intercepts; future/unknown extension interfaces and physical no-RT qualification
require their own audit and execution evidence.
