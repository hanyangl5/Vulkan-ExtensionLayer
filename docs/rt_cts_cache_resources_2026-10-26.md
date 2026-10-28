<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Cached RTP, resource boundaries and expanded external CTS

The full [implementation goal](ray_tracing_fallback_goal.md) remains active.
Source base is `762ebd4` plus this checkpoint's changes. This work does not
complete M0–M5 or qualify a physical GPU without native RT.

## External CTS

The external checkout is `D:/Codes/VK-GL-CTS`, revision
`d18e4c68543103371715845cd914c71e5859963e`. The executed binary is
`build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe`, SHA-256
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
Runs use RTX 3080, driver 596.49, Vulkan 1.4.329 and SDK/validation 1.4.357.
Each case has validation, a separate hidden process, a 60-second timeout and
disabled CTS shader cache. The runner snapshots and hashes the layer DLL and
manifest, retaining commands, stdout, session identity and QPA results.

| Selection | Software pass | Software fail | Native pass | Other results |
| --- | ---: | ---: | ---: | ---: |
| Initial expanded state-spill selection | 39 | 13 | 52 | 0 |
| Final complete state-spill rerun | 52 | 0 | 52 | 0 |
| Initial execution selection | 32 | 14 | 46 | 2 matching NotSupported |
| Final complete execution rerun | 46 | 0 | 46 | 2 matching NotSupported |

The exact [52-case selection](rt_cts_state_spill_cases.txt) includes the previous
20 cases plus 16/64-bit aggregates, null/undefined values, pipeline interfaces
and opaque resource values across TraceRay, ExecuteCallable and
ReportIntersection. All 13 initial failures reject opaque image/sampler/combined
values or image texel pointers crossing continuation boundaries. These are
implementation failures, not skips or native-driver failures. The original
results remain in `build-rt-cts-evidence/rtp-state-spill-initial/summary.json`;
the matching native control is `rtp-state-spill-native/summary.json`.

The independent [48-case execution selection](rt_cts_execution_cases.txt) covers
traversal dispositions, specialization, nested callables, memory guarantees,
shader records, recursion and SBT alignment. Both complete final runs use the
same layer snapshot, SHA-256
`77946ee3dda62ea3f16365a6b779f7ce3f98c3859099b965f0a389ede05d80f5`.
Together they measure **98 pass, 0 fail, 2 NotSupported**, matching the native
controls. The two unsupported cases require SBT handle alignment 1 and the
optional longVector feature. No validation warnings/errors, crashes, timeouts
or missing results were found in the final run outputs. NotSupported never
counts as a pass. Neither selected matrix establishes conformance.

Final artifacts are `build-rt-cts-evidence/rtp-state-spill-checkpoint` and
`rtp-execution-checkpoint`. Initial and intermediate failures remain beside them;
their commands, layer hashes and QPA/stdout hashes are retained in the
[machine-readable evidence](rt_cts_cache_resources_2026-10-26.json).

The existing 56-case RQ/AS selection was also rerun with this layer: **54 pass,
2 fail**, with **56/56** native controls. Both failures still stop at the
explicitly unimplemented AS property-query pool, before compaction or
serialization CTS execution. They are retained in
`build-rt-cts-evidence/rq-cache-opaque-checkpoint`; native controls are in
`rq-cache-opaque-native`. This checkpoint does not claim to resolve them.

## Native pipeline caching and application state

Each RTP stack-budget variant owns one cached native compute pipeline. The
context address travels through a private layout with an independent 8-byte
compute push range; it no longer specializes a new native pipeline for every
Trace command. Read-only dispatch packets retain pipeline ownership, while
continuation frames remain invocation-private.

Six Trace calls across two stack budgets create exactly two native pipelines.
The public GPU proof fills all 32 descriptor-set slots and all 256 application
compute push bytes and executes two queues with separate contexts. Legacy and
maintenance6 paths pass both validation orders and synchronization validation.
Default specialization-based component runtime behavior is separately retained.
Evidence: `build-rq-once/rtp-cache-reuse-proof.json`,
`build-rq-once/rtp-cache-final-*.log` and
`build-rt-emitter/rtp-cache-default-context-test.log`.

Core/KHR descriptor and push command aliases capture the affected logical bind
points and immutable application bytes. Inline dynamic layouts retain private
owners after their public temporary is destroyed. Nonzero NV push banks retain
their independent native state; explicit RT bank use fails closed. Unknown
ordinary-device chains pass through unchanged. Native dynamicPipelineLayout is
unavailable on this GPU, so the NULL-layout path has host-spy evidence only.
Ordinary compute shader-object binding is now also captured and restored,
including explicit unbinding and switching back to a pipeline. The GPU fixture
uses independent, compatible compute and RT push layouts. Both validation orders
with synchronization validation pass without diagnostics; logs are
`build-rt-cts-evidence/shader-object-final-sync-{above,below}.txt`.

Separately created, identically defined descriptor-set layouts are compared
using copied public definitions, including immutable sampler identity and
binding flags. Copies survive caller-memory mutation, destruction and handle
reuse. The resource report retains an independently reproduced SDK validation
issue for early DSL destruction, including a native compute control. Its default
boundary fixture retains DSL objects through execution for zero-diagnostic
validation.

Linker duplicate-type elimination now normalizes aggregate copies to
`OpCopyObject` when their types become identical. ShaderCallKHR scope operands
become Device scope, which conservatively contains the invocation's logical
shader-call tree and preserves the original acquire/release semantics. Ordinary
constants sharing the scope value 6 are untouched. Invocation scope was rejected
because Vulkan requires Relaxed semantics for Invocation-scoped memory barriers;
the intermediate failure remains in `rtp-execution-link-scope`. Scalar-layout
shader records use matching validator options throughout compilation and runtime
optimization, consistent with the required native scalar-layout feature.

The [shader-call scope definition](https://docs.vulkan.org/spec/latest/chapters/shaders.html#shaders-scope-shadercall)
and [shader call execution model](https://docs.vulkan.org/spec/latest/chapters/raytracing.html#shader-call-instructions)
underpin this lowering. The linked GPU regression combines identity aggregate
copies, barriers around suspension and ordinary numeric-6 output indexing.

## Resource and memory-query integration

The [resource report](rt_resource_profile_2026-10-26.md) records the checked
descriptor planner and actual public RTP execution at the native SSBO boundary:
16 AS descriptors, 1,048,544 static SSBOs, 16 dynamic SSBOs, descriptor set 31,
and all 256 application RT push bytes. It checks the final array elements and
both hit/miss results. The profile remains inactive in global discovery pending
complete capability and private-allocation qualification.

Descriptor layouts, layout-support queries and pools share AS-to-SSBO/stage
translation. Pool rows remain separate, avoiding overflow from merging legal
duplicate rows. Resource-only descriptor array types no longer acquire enormous
continuation codecs; actual aggregate values retain checked codecs.

Ordinary buffer usages now retain their original native requirements. Only
explicit AS storage/build-input/SBT usage receives storage-buffer/BDA translation.
Core/KHR device buffer requirement queries use the same translation, handle
requirement queries use actual native handles, and external-buffer queries work
before logical-device creation. Registry-derived stack storage preserves every
known BufferCreateInfo prefix, including Flags2, without heap allocation in valid
void queries. Unknown future prefixes remain an explicit boundary.

Parent integration checks pass 20/20 offline. The broader GPU run passes 21/22;
the one failure used a stale ABI1 traversal-test shader with ABI2 host fixtures.
CMake now depends on both shared ABI/address headers. A new shader-reported
version/size tuple rejects this stale artifact explicitly, while all original
opacity/SBT/depth140/resume expectations remain unchanged. The rebuilt failed
test passes 1/1; all 22 selected GPU tests therefore have current passing evidence.
Production traversal dependencies already included the shared ABI headers.

The standalone RT suite passes 17/17. These checks supplement external CTS.
Logs are `build-rt-cts-evidence/cache-opaque-checkpoint-{offline,gpu}.txt`,
`traversal-abi-recheck.txt` and `cache-opaque-components.txt`. The original broad
failure remains in its log. Public memory-query validation in both layer orders
is retained in `build-rq-memory/validation-{above,below}.txt`.

## Remaining work

Pointer Phi/parameter provenance, mutable resource encodings, complete
descriptor disturbance/interoperability and source coverage remain open. The
[opaque provenance report](rt_opaque_provenance_2026-10-26.md) defines the
implemented traditional-descriptor route. Public AS property-query pools account
for the two retained RQ/AS CTS failures. Their host publication needs explicit
per-execution synchronization; coherent polling or reused completion markers
do not establish that contract. No such shortcut has been installed.

Native allocation pooling, truthful complete no-RT discovery, physical no-RT and
POSIX qualification, builder numerical/large-count coverage and the full M0–M5
exit criteria remain open. Historical CTS failures are retained.
