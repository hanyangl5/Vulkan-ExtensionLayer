<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# No-native-RT advertisement and qualification gates, 2026-10-28

This is a source audit and an implementation proposal. It does not enable
virtual AS/RQ/RTP discovery, close a milestone, or establish physical no-RT
support. The audit used source at `f963e7cd3eb400d846a94c554a2dcaf35866394b`
plus the working tree on 2026-10-28. Function names identify the relevant code
if subsequent changes move the listed lines. The separate frozen-DLL 50-case
CTS run was in progress when this document was prepared; no outcome from that
run is asserted here. No build, GPU test, or CTS run was performed for this audit.

## Fixed specification and SDK baseline

The local baseline is Vulkan SDK **1.4.357.0**, with `VK_HEADER_VERSION=357` in
`C:/VulkanSDK/1.4.357.0/Include/vulkan/vulkan_core.h`. Its registry is
`C:/VulkanSDK/1.4.357.0/share/vulkan/registry/vk.xml`, SHA-256
`5ce67444b5233c113b492bd35c1912cd7711cf557a4daf691c7941630ad3ec02`.
That hash matches the registry recorded in the generated
[guard table](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/tests/no_native_rt_guard_registry.h:4).

Normative comparisons below use the official
[Vulkan-Docs v1.4.357 Limits source](https://github.com/KhronosGroup/Vulkan-Docs/blob/v1.4.357/chapters/limits.adoc#L6356),
including its RT required-limit rows and footnotes. The rendered
[Required Limits reference](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html)
is convenient for reading, but `latest` is not the pinned version. SDK updates
require a new comparison of the source rules, registry, VVL and guard tables.

Optional features are not automatically part of the mandatory base profile.
For example, AS host commands, capture/replay, indirect build/trace and AS
descriptor update-after-bind are separate feature promises. The production
backend's private requirements are also distinct from application-visible
mandatory features: requiring a native feature internally does not mean the
application must explicitly request it. A complete advertised profile must
support all legal mandatory behavior within its reported limits.

## Current predicate chain and the actual discovery breaks

The existing candidate resource profile does **not** depend on native
`accelerationStructure`, `rayQuery` or `rayTracingPipeline` being true. It combines
the compute backend checks with the descriptor-budget proof. Masking native RT
truth can leave this candidate predicate true while public discovery remains
false through the following independent paths.

| Source location | Current behavior | Consequence with all native RT base features false |
| --- | --- | --- |
| [vkrt_features.c:138](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_features.c:138), `native_backend_capabilities` | Checks native non-RT compute features/properties and returns query capability separately | The compute candidate can remain eligible |
| [vkrt_features.c:224](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_features.c:224), `virtual_rt_resource_profile` | Requires backend and descriptor proof; copies the query-capability output without requiring it | Candidate eligibility is not complete public RT qualification |
| [vkrt_discovery.c:25](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_discovery.c:25), `apply_properties` | Additionally requires `native_rt_features`, which ORs the three native base bits | No software descriptor/property profile is applied |
| [vkrt_discovery.c:67](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_discovery.c:67), `GetPhysicalDeviceFeatures2` | Clears base bits when ineligible; never sets them from software support when eligible | Native false remains public false |
| [vkrt_discovery.c:128](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_discovery.c:128), extension enumeration | Filters the native list and appends only deferred host operations | Missing AS/RQ/RTP extension names remain absent |
| [vkrt_features.c:285](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_features.c:285), device creation | Uses the backend/resource candidate, strips consumed RT requests and adds private native features | Device-create admission can differ from discovery and complete mandatory capability |

The current behavior is deliberate prototype gating, not a hidden dependency
of software traversal on native RT hardware. Removing only the native-RT
properties gate, or setting the three feature bits true, would leave the other
API outputs and execution contracts incomplete.

The non-RT prerequisites that must remain real include BDA; scalar layout;
integer 8/16/64 and storage-buffer 8/16 access; Vulkan device-scope memory-model
support; SSBO dynamic/nonuniform indexing; variable storage-buffer pointers;
native Vulkan 1.2 for the active SPIR-V 1.5 modules; compute subgroup
basic/ballot/shuffle; float32 signed-zero/Inf/NaN preservation; and the active
builder's workgroup/shared-memory/push-constant limits. The descriptor proof
must continue preserving the applicable core and enabled-feature minima.
Native RT truth is not a substitute for any of these checks.

These are necessary candidate checks. They do not prove private allocation
budgets, compatible coherent BDA buffer memory, all queue/lifecycle behavior,
shader coverage, or construction capacity. Actual private buffer requirements
and device initialization remain relevant after physical-device queries.

## Mandatory query capability is not in the public predicate

At [vkrt_features.c:165](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_features.c:165),
`hostQueryReset && shaderBufferInt64Atomics` becomes `query_backend`; absence of
either does not fail the current public candidate. Device initialization
[recomputes this flag](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_layer.c:266)
and disables it for a multi-physical-device group. The query initializer can
return success without creating `dev->queries` when its flag or required native
entry points are missing. Subsequently the virtual AS
[CreateQueryPool path](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_query_pool.c:351)
returns `VK_ERROR_FEATURE_NOT_PRESENT` when `dev->queries` is null.

That is not a complete base AS contract. Device AS property queries and their
result/completion behavior cannot become unavailable merely because AS host
commands or other optional RT features are false. The public profile must
require the currently selected implementation's genuine query prerequisites,
or use a complete alternative query implementation. Native host-query-reset
and integer-atomic requirements are backend choices, not newly mandatory
application feature requests.

The completion path also checks the command pool's queue family for compute
support and nonzero timestamp-valid bits in
[record admission](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_query_pool.c:256).
Those conditions, protected-command behavior, device-group handling, submission
occurrences, completion retirement and logical-stage waits need an explicit
contract. A successful initializer with query functionality silently omitted is
not an acceptable replacement. The separate execution/replay work must still
prove those semantics; this audit does not claim it finished.

## Generate complete software properties

The existing
[resource profile](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_resource_profile.c:360)
updates the four AS descriptor-count properties. It does not synthesize the
remaining AS properties or any RTP properties. The guard's masked-discovery
mode clears those native property tails. A real ICD may ignore an unsupported
structure, making caller-initialized stale output another concern.

The software contract must fill every relevant AS/RTP property from its own
implementation and preserve caller `sType`, `pNext`, unrelated outputs and the
shader-object UUID composition. It should use the same software values on an
eligible native-RT GPU and an eligible no-RT GPU. For example,
[group-handle export](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_rtp.cpp:738)
uses 32-byte handles; passing through unrelated native RTP properties does not
define the software SBT ABI.

Selected base-profile comparisons from the pinned required-limit table are:

| Property | Required comparison |
| --- | --- |
| `maxGeometryCount`, `maxInstanceCount` | At least `2^24 - 1` |
| `maxPrimitiveCount` | At least `2^29 - 1` |
| Ordinary AS per-stage/set descriptor limits | At least 16 |
| `minAccelerationStructureScratchOffsetAlignment` | Power of two, no greater than 256 |
| `shaderGroupHandleSize` | Exactly 32 |
| `maxRayRecursionDepth` | At least 1 |
| `maxShaderGroupStride` | At least 4096 |
| `shaderGroupBaseAlignment` | No greater than 64 |
| `maxRayDispatchInvocationCount` | At least `2^30` |
| `shaderGroupHandleAlignment` | Power of two, no greater than 32 |
| `maxRayHitAttributeSize` | At least 32 |

These are qualification comparisons, not proposed values already proven by this
audit. See the fixed
[v1.4.357 RT table](https://github.com/KhronosGroup/Vulkan-Docs/blob/v1.4.357/chapters/limits.adoc#L6746)
and property definitions in the same chapter. Optional capture/replay and other
feature-dependent limits require their own applicable conditions.

The AS update-after-bind-named properties need careful version-specific
treatment. Preserve the established
[SDK357 ALL-layout analysis and retained VVL failure](D:/Codes/Vulkan-ExtensionLayer/docs/rt_resource_profile_2026-10-26.md:70):
VUIDs `03572`/`03574` counted ordinary AS bindings against those properties, so
zero was incorrect for the existing AS16 layout path. Current code sets all
four AS counts to 16, counting the same reserved ordinary-AS budget once; the
optional AS UAB binding feature stays false.

Read those rows using the complete feature-gated table, not its supported
number in isolation. The
[pinned Limits introduction](https://github.com/KhronosGroup/Vulkan-Docs/blob/v1.4.357/chapters/limits.adoc#L5803)
and table header distinguish Unsupported Limit from Supported Limit.
The controlling
[AS UAB feature definition](https://github.com/KhronosGroup/Vulkan-Docs/blob/v1.4.357/chapters/features.adoc#L4002)
governs updating AS descriptors after binding. With that feature false, the
unsupported `-` column does not impose the supported 500000 minimum.
[Footnote 9](https://github.com/KhronosGroup/Vulkan-Docs/blob/v1.4.357/chapters/limits.adoc#L7116)
still requires the UAB-named limits to cover their ordinary counterparts.
Together with SDK357's ALL-layout counting, retain the existing false feature
and four limits of 16. This audit identifies no new conflict in that decision;
it does not add optional UAB functionality to the mandatory no-RT work.

## Production construction limits remain narrower than the desired contract

The production build admission at
[vkrt_build.c:148](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_build.c:148)
rejects `l.ir.as_size >= UINT32_MAX`, `leaf_count >= 2^29`, and
`ceil(leaf_count / 64) > max_build_dispatch_x`. The last value comes directly
from native `maxComputeWorkGroupCount[0]`.

For example, X=65535 admits at most 4,194,240 leaves through that dispatch
condition, well below the required `maxPrimitiveCount` comparison above.
The source IR has another independent boundary: its
[layout](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/bvh_ir_build_layout.h:25)
uses 68-byte triangle leaves, 32-byte AABB leaves or 96-byte instance leaves,
plus 40-byte internal nodes. Triangle IR therefore grows approximately as
`108 * leaves` before alignment/header costs and hits the 4GiB admission check
well before the desired primitive-count limit. Host `VkDeviceSize` arithmetic
alone does not remove the source GPU format's offset/ID constraints.

Some encoder and refit passes already have explicit `first`/chunk loops at
[vkrt_build.c:220](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_build.c:220)
and [vkrt_build.c:244](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/src/vkrt_build.c:244).
The earlier global admission check still applies; these loops do not qualify
large complete construction. Likewise, production RTP trace chunking does not
prove AS builder capacity.

Real allocation exhaustion may legitimately fail. A fixed narrow-ID or
one-dimensional dispatch rejection reported as out-of-device-memory is not
evidence that the larger advertised construction contract has been implemented.
Do not lower mandatory public minima to the present algorithmic thresholds.

One candidate is **segmented Mesa construction, conversion into a global BVH4,
and chunked or two-dimensional construction dispatch**. This is a proposal,
not implemented or validated by this audit. It must prove global geometry and
primitive IDs, sorted-key handling, segment-root stitching, bounds and parent
links, instance references, and consistent BUILD/UPDATE/COPY/serialization/query
sizes. All affected shaders must use consistent global invocation numbering;
changing only the host dispatch dimensions is insufficient. An alternative is
a complete wider-offset source IR and corresponding shader/host ABI update.
Both routes require explicit overflow, count and memory-budget proofs.

## Suggested production landing sequence

1. Define one public virtual-RT qualification/profile query containing the real
   backend prerequisites, descriptor proof, mandatory query contract and complete
   software property plan. Keep ordinary shader-object backend eligibility
   separate. Return an explicit failed gate, rather than treating descriptor
   qualification alone as full public support.
2. Close mandatory execution gaps, including construction capacity and the
   query/replay/logical-stage conditions. Validate property values against actual
   code paths and the applicable feature-gated specification. Legal required
   shader/API inputs that are still unsupported remain goal work.
3. Route extension enumeration, base Features2, legacy/core/KHR Properties and
   `CreateDevice` through that same public profile. Remove native RT truth as
   a prerequisite only when the corresponding software contract is justified.
   Fill complete known RT outputs, not merely base booleans. Preserve optional
   false bits, unknown chain suffixes and ordinary-device behavior.
4. Add missing AS/RQ/RTP extension names exactly once for a qualified physical
   device, including capacity/count/fill handling and the layer-name path.
   Resolve dependency closure from the pinned registry. In SDK357, AS requires
   deferred host operations plus the Vulkan 1.1 extension path or Vulkan 1.2;
   RQ/RTP require AS and SPIR-V 1.4 or Vulkan 1.2. The private alias list must also
   be valid for the effective application/native API versions. Native physical
   Vulkan 1.2 alone is not proof of the older application API alias/module path.
5. At device creation, consume RT names/feature nodes and enable only genuinely
   supported private requirements. Reuse the same captured profile decisions;
   do not silently disable mandatory query functionality after admission.
   Preserve the existing independent CPU deferred and ordinary-device paths.
6. Complete host, masked-discovery GPU, external CTS and physical-device
   qualification below. A discovery-only patch or a feature-bit switch does
   not meet this sequence's completion criterion.

## Required validation matrix

| Axis | Cases and evidence |
| --- | --- |
| Native RT truth | All false; each partial AS/RQ/RTP combination; all true; ICD ignores queried RT structures; stale true caller bytes |
| Private backend | Fully qualified candidate; each required scalar/storage/BDA/memory-model/indexing/pointer/builder condition missing individually |
| Resources | Exactly admitted descriptor budget and one below each boundary; applicable enabled optional-feature budgets; private memory types and allocation failures |
| Query/completion | Missing host reset or buffer int64 atomics; absent dispatch functions; compute queue with/without timestamps; requested queue families; device groups; repeated and overlapping submissions; failure retirement |
| Discovery APIs | Enumeration count-only, zero/short/excess capacity, retry/error paths, duplicate names, named layer and unrelated layer; core/KHR Features2; legacy/core/KHR Properties2 with sentinels and shader UUID composition |
| Device creation | Minimal unmodified application request; core versus extension feature aliases; extension-only requests; input chains byte-preserved; below-layer RT names/bits absent; normal non-RT/deferred-only requests unchanged |
| Software properties | Every AS/RTP field defined; minimum comparisons and power-of-two/alignment checks; same software ABI with native RT masked or visible; reconciled feature-gated UAB counting rules |
| Construction | Boundary layout/ID arithmetic, 4GiB crossings, dispatch row/chunk boundaries, original IDs across segments, BUILD/UPDATE/empty/copy/query/serialization agreement; real GPU oracle when resources permit |
| Functional masked discovery | Normal application enumeration/feature selection/device creation followed by AS BUILD/UPDATE/COPY/properties, RQ and RTP with SBT/stack/libraries; no forced CTS feature selection |
| Qualification environments | Native execution-only guard and native controls retained separately; masked guard with VVL; physical no-RT devices, additional vendors and POSIX |

Extend the existing discovery host tests: they currently encode no-RT native
passthrough rather than complete emulation, including the native-false feature
expectations and property modes in
[test_discovery.c](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/tests/test_discovery.c:378).
Update them to assert the unified justified profile and negative gates, not to
expect true unconditionally. The guard must continue rejecting native RT below
the software layer; it must not be relaxed to make a run pass.

## Evidence boundaries

The test
[guard policy and modes](D:/Codes/Vulkan-ExtensionLayer/layers/raytracing/tests/no_native_rt_guard_README.md:4)
distinguish three different claims:

- **Execution-only guard:** native discovery remains visible so CTS selects RT
  cases. A complete zero-rejection log plus functional pass demonstrates the
  observed test did not forward forbidden native RT at the intercepted boundary.
- **Masked discovery:** `VKRT_NO_RT_GUARD_MASK_DISCOVERY=1` removes native RT
  extensions and clears feature/property tails. Successful unmodified
  application admission and execution would additionally test the layer's
  independent advertisement contract. NotSupported or a skip is not that proof.
- **Physical no-RT qualification:** ordinary Vulkan runs on an actual no-RT
  device with its real capabilities, memory types, limits and driver compiler.
  Neither guard mode emulates those hardware/driver differences.

Retain hashes of the software DLL, guard, manifests, CTS executable/revision,
configuration and per-process complete guard logs. Report failures, crashes,
NotSupported and skips separately. A new passing slice does not rewrite older
failures or establish the untested profile/milestones. This audit adds no new
execution evidence to the existing checkpoints.
