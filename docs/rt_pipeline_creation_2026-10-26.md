<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RTP base creation flags and derivative pipelines

Canonical `vkCreateRayTracingPipelinesKHR` now accepts the legacy flags
`DISABLE_OPTIMIZATION`, `ALLOW_DERIVATIVES` and `DERIVATIVE`, including their
valid combinations. The previous implementation rejected every nonzero flag.
This is a scoped creation-contract improvement, not completion of M4.

## Normative contract

The derivative valid-usage rules were checked against the installed primary
Vulkan registry `validusage.json`, API **1.4.357**, Vulkan-Docs commit
`d184375dcc5da2b06ca375a8d7d1f9d21ca64a76` (registry date
`2026-07-17 00:55:04Z`). The file is at
`C:/VulkanSDK/1.4.357.0/share/vulkan/registry/validusage.json`.

- `VUID-VkRayTracingPipelineCreateInfoKHR-flags-07984/07985/07986` define the
  exclusive base-handle/index selection for a derivative.
- `VUID-vkCreateRayTracingPipelinesKHR-flags-03415` requires an in-call base
  index to reference an earlier element.
- `VUID-vkCreateRayTracingPipelinesKHR-flags-03416` requires the base to have
  `ALLOW_DERIVATIVES`.

These rules are also in the primary
[create-info reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkRayTracingPipelineCreateInfoKHR.html)
and [command reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateRayTracingPipelinesKHR.html).
The [pipeline derivatives section](https://docs.vulkan.org/spec/latest/chapters/pipelines.html#pipelines-derivatives)
describes derivation as an opportunity to accelerate creation and switching;
it does not require inheriting omitted child state. The child is fully specified.
Base fields are ignored when `DERIVATIVE` is absent.

The [flag definition](https://docs.vulkan.org/spec/latest/chapters/pipelines.html#VkPipelineCreateFlagBits)
requires an unoptimized pipeline for `DISABLE_OPTIMIZATION`. Consequently the
layer skips optional SPIR-V performance passes and forwards this flag to every
native compute variant. Required specialization, continuation lowering and
linking still run. Derivation hints are not forwarded as invalid native compute
base references; the canonical child is compiled independently.

## Implementation and lifetime

The pipeline retains its original creation flags. Derivative lookup uses the
same device's canonical pipeline map, either through a live public base handle
or the successfully created earlier output slot. The base must allow
derivatives. No parent reference is retained after creation, so destroying the
base cannot invalidate the child's native variants, layout or metadata.

A supplied application pipeline cache is forwarded to initial native creation
during the public call. It is never retained. Later lazily created stack
variants use `VK_NULL_HANDLE` for their cache. Existing per-pipeline native
variant caching and command-buffer ownership remain intact.

## Verification

`test_rtp_api --derivatives` creates a base with `ALLOW_DERIVATIVES` and
`DISABLE_OPTIMIZATION`, an in-call indexed child with `ALLOW_DERIVATIVES` and
`DERIVATIVE`, and a separate handle-based child with `DERIVATIVE` and
`DISABLE_OPTIMIZATION`. It destroys the original base and application cache
before recording any trace. Both children execute with their own SBT handles.
The existing independent output checks cover six traces, command-buffer reset,
different stack budgets, compute-state restoration, layout lifetime and two
queues using separate contexts.

All three final GPU runs passed with Khronos validation and synchronization
validation enabled, with no VUIDs, validation errors or synchronization hazards:

- `build-rq-once/rtp-derivatives-cache-above.log`
- `build-rq-once/rtp-derivatives-cache-below.log`
- `build-rq-once/rtp-derivatives-cache-native.log`

The native implementation reports zero stack use for this shader fixture. The
test now permits that valid result and allocates a conservative positive 16-byte
budget; all shader-result checks remain unchanged. The initial harness failure
is retained in `build-rq-once/rtp-derivatives-native.log`.

The native-spy host test passes, checking native flags/cache forwarding,
unchanged caller inputs, ignored non-derivative base fields, base permission,
invalid index rejection before native calls, independent child lifetime and
partial-array OOM results/cleanup. It also confirms that compile-control and
capture/replay flags are rejected before native compilation. Evidence:
`build-rq-once/rtp-derivative-host-cache.log`.

The default specialization-context runtime component still passes (17.66 s):
`build-rt-emitter/derivative-default-runtime.log`.

Actual external CTS normal-RT smoke passes **3/3**, with validation enabled:
`build-rt-cts-evidence/rtp-derivatives-cache-smoke/summary.json`. Cases are
LaunchID raygen, nested-helper ReportIntersection with module-Private state,
and callable image-handle suspension. CTS revision is
`d18e4c68543103371715845cd914c71e5859963e`; the runner snapshots and hashes the
isolated layer DLL. These cases exercise the unchanged ordinary creation route.
No matching derivative or disable-optimization construction case was found in
the inspected RTP mustpass list or ray-tracing CTS source, so the custom public
test is not described as CTS derivative coverage.

## Remaining boundaries

The accepted legacy flag mask contains exactly the three flags listed above.
Every other flag remains explicitly unsupported, including compile-required
and early-return controls, shader-group capture/replay, no-null shader promises,
primitive skip flags, pipeline libraries and other optional creation modes.
Creation `pNext` remains unsupported, including Flags2 and creation feedback;
shader stage flags and stage/group extension chains retain their previous
boundaries. There is no claimed derivative compilation speedup or pipeline-cache
hit rate. Complete M4 creation/optional-feature behavior remains open.
