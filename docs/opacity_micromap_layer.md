<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Software Opacity Micromap Layer: Goals and Specification

**Status:** design proposal; documentation only. No OMM implementation or
conformance claim is made by this branch. Reviewed against the Vulkan
documentation available on 2026-10-25.

## Purpose

Provide opacity micromap functionality to unmodified Vulkan applications on
devices that already support native ray tracing but lack native OMM support.
The driver continues to build acceleration structures, traverse its native BVH,
and intersect geometry. The layer implements micromap storage and API behavior,
then instruments shaders to apply OMM decisions to native intersection candidates.

The goal is complete mandatory device functionality for the advertised extension,
including Ray Query and Ray Tracing Pipeline consumers when those native features
are available. Intermediate milestones do not redefine completion as a limited
scene, shader pattern, or executable subset.

This provides compatibility. It does not promise the performance of fixed-function
OMM filtering: software evaluation still executes shader instructions.

## Scope and extension baseline

The first target is `VK_EXT_opacity_micromap`, with its dependency requirements
satisfied. Native `accelerationStructure` is required. Native `rayQuery` and
`rayTracingPipeline` remain independently queried capabilities; the layer does not
invent either feature on a device that lacks it.

| Feature or API | Initial target |
| --- | --- |
| EXT mandatory device operations and OMM consumption | Complete implementation goal |
| `micromapCaptureReplay` | Report false until implemented and tested |
| `micromapHostCommands` | Report false until implemented and tested |
| Native OMM already available | Prefer native pass-through |
| `VK_KHR_opacity_micromap` | Separate follow-up scope |

The KHR extension is not just an additional extension name. It requires
`VK_KHR_device_address_commands`, represents micromaps through
`VkAccelerationStructureKHR`, and reuses AS operations. Its Ray Query declaration
rules also differ. Supporting it requires an explicit API/resource-model bridge
and a dependency audit. Do not advertise KHR support merely because EXT works.

Applications provide the micromap data. Automatically baking alpha textures into
OMMs, UE integration, custom BVH construction, and software geometric traversal are
outside this workstream.

## Relationship to ray tracing fallback

| Available device capabilities | Backend |
| --- | --- |
| Native RT and native OMM | Native driver path |
| Native RT without OMM | Native RT plus this software OMM adapter |
| No native RT | Separate RT fallback; shared OMM core can be integrated later |

The native adapter must be implementable without Mesa BVH builders, a portable
software BVH format, or a complete software RT shader continuation compiler.
The existing [`codex/ray-tracing-fallback` workstream](https://github.com/hanyangl5/Vulkan-ExtensionLayer/tree/codex/ray-tracing-fallback)
is related work, not a build or completion dependency.

Use a capability-aware integration boundary in VEL. Do not blindly stack two
independent layers that both virtualize AS objects and rewrite the same shaders.

## Architecture

The module names below describe responsibilities, not existing implementation.

| Module | Responsibility |
| --- | --- |
| OMM API and storage | Object tracking, build, copy, queries, serialization, properties |
| OMM evaluator | Barycentric indexing, state decoding, special indices, opacity resolution |
| AS metadata | Geometry association, instance information, address/descriptor lookup, lifetime tracking |
| Native RT adapter | Shader instrumentation, candidate handling, SBT/context translation |
| Future software RT adapter | Call the same evaluator from software traversal |

Share format definitions, the evaluator, applicable object bookkeeping, SPIR-V
utilities, and test vectors. Keep candidate delivery and traversal control in the
backend adapters. The evaluator must not depend on a particular BVH node layout.

Native AS storage is opaque. Metadata must come from intercepted application
operations and layer-owned data, never from interpreting a driver's BVH bytes.

## Functional requirements

### Discovery and API translation

Extension enumeration, feature/property queries, device creation, and entry-point
lookup must agree. Report actual supported subdivision limits and optional
features. Remove emulated extension names, unsupported `pNext` structures, flags,
and SPIR-V declarations before forwarding to the driver, while preserving unrelated
application state and the original logical values for shader semantics.

Layer resources and internal shader capabilities must fit the device's limits.
Metadata access cannot assume an unused descriptor binding, descriptor set, or
push-constant range in the application. Any required resource reservation or
layout virtualization is a design proof obligation.

### Micromap operations

| Operations | Required behavior |
| --- | --- |
| `vkCreateMicromapEXT`, `vkDestroyMicromapEXT` | Correct handles, backing ranges, allocation behavior, and lifetimes |
| `vkGetMicromapBuildSizesEXT` | Valid bounds for the actual internal representation and scratch requirements |
| `vkCmdBuildMicromapsEXT` | GPU-resident inputs; both formats and all reported subdivision levels |
| `vkCmdCopyMicromapEXT` | Clone and compact modes with equivalent observable contents |
| `vkCmdWriteMicromapsPropertiesEXT` | Correct compacted/serialization sizes and query availability |
| `vkCmdCopyMicromapToMemoryEXT`, `vkCmdCopyMemoryToMicromapEXT` | Serialization round trip with the layer's compatibility identity |
| `vkGetDeviceMicromapCompatibilityEXT` | Truthful compatibility decisions, including foreign blobs |

The built representation must be self-contained after command completion. It must
not retain a dependency on application build-input or scratch memory beyond the
lifetimes allowed by Vulkan. Serialization must not contain unrelocated transient
process pointers. A compact operation may retain the same size if no smaller
representation is available, provided all reported sizes and semantics remain valid.

The optional host-command and capture/replay paths need separate implementation
and tests before their feature bits can become true.

### OMM lookup and opacity

Support 2-state and 4-state encodings, all special indices, indexed and implicit
triangle association, index strides, `baseTriangle`, and both usage-count forms.
Recognize special indices at their stored index width before adding `baseTriangle`;
support a null micromap handle when the association legally uses only special indices.
Implement the specified barycentric-to-microtriangle mapping, including boundary
behavior, at every reported subdivision level.

Keep these two steps distinct: first obtain the OMM result; then apply the remaining
logical opacity and culling rules. The evaluator's result is `ignore`, `opaque`,
or `non-opaque`. Unknown states remain non-opaque normally; force-2-state resolves
unknown-transparent to ignore and unknown-opaque to opaque. An ignored OMM result
must never be revived by a force-opaque ray or instance flag.

Preserve instance OMM disable/force-2-state controls, ray flags, geometry opacity,
and relevant AS update permissions. Geometry without an active OMM retains its
original behavior, including when it shares a TLAS with OMM geometry.

### AS association and lifetime

Attach sidecar metadata to native BLAS/TLAS objects while forwarding ordinary
geometry to native AS construction. Track geometry/primitive numbering and original
instance flags without relying on mutable application custom indices as unique IDs.

Handle multi-geometry BLASes, BLAS reuse, GPU-generated instance inputs, valid AS
updates, clone/compaction, destruction, and address relocation. Materialize any
metadata needed after build-input buffers can legally be reused. AS serialization
requires a versioned envelope carrying the native blob and required sidecars, with
matching compatibility and size-query handling. Preserve Vulkan-defined public
serialization headers and address-relocation conventions; layer metadata is not
permission to replace externally specified fields with an arbitrary format.

Descriptor-sourced AS operands require preserved descriptor provenance and a
parallel metadata lookup, including dynamic indexing and helper-function passage.
An opaque SPIR-V AS value cannot simply be cast to an integer address. Integer
provenance from `OpConvertUToAccelerationStructureKHR` must also be preserved.

### Native Ray Query adapter

Instrument initialization and `Proceed` to expose native candidates for software
opacity evaluation. A candidate-enabling strategy is to save the original flags,
request `NoOpaqueKHR`, and remove incompatible opacity/OMM bits from the native
operation. The exact rewrite must be validated against every supported flag path.

The wrapper must then implement the original logical behavior:

- Ignore transparent or logically culled candidates and continue traversal.
- Auto-confirm logically opaque triangle candidates without returning them as
  application-visible non-opaque candidates.
- Return logically non-opaque triangle candidates to application code normally.
- Preserve AABB candidate processing and its original logical opacity.

Preserve candidate versus committed state, all query getters, original ray flags,
`ConfirmIntersection`, `GenerateIntersection`, `Terminate`, and first-hit behavior.
Handle multiple live queries, arrays, reinitialization, and query objects passed
through functions in every supported native shader stage.

EXT-enabled Ray Query consumes attached OMM data without the KHR execution mode.
For a future KHR-only path, the required `OpacityMicromapIdKHR` declaration must be
handled separately; its absence is not an OMM-disable mechanism.

### Native Ray Tracing Pipeline adapter

Retain native shader-call execution and geometric traversal. Wrap/inject Any-Hit
handling so ignored candidates skip application code, logically opaque candidates
bypass application Any-Hit, and non-opaque candidates execute the original handler.
Preserve `IgnoreIntersection` and `TerminateRay`, hit attributes, payloads, original
shader-visible flags, and application-visible side effects.

Global flag rewriting also affects non-OMM and procedural geometry. Intersection
shaders need guarding/wrapping where necessary: rejecting a reported intersection
later cannot undo an application Intersection shader that should never have run.
Procedural reports must retain their original logical opacity and Any-Hit behavior.

Handle absent Any-Hit shaders, legal zero/null shader groups, pipeline libraries,
group-handle queries, and application SBT indexing. Any shadow SBT must preserve
the original shader-record data, obey alignment/stride limits, and support
GPU-generated records and indirect trace commands when those features are enabled.
Injected shaders must be included in stack-size accounting without consuming an
extra application ray recursion level.

Pipeline caches and any exposed shader/pipeline binary paths must identify the
instrumentation ABI and preserve the information needed to recreate the logical
application shaders. Native opaque binaries cannot be assumed to contain editable
SPIR-V.

Transporting the original flags and AS metadata into the candidate wrappers is an
open design gate. A candidate approach is a dispatch context referenced by a shadow
SBT, with a launch-indexed trace-depth stack and redirected shader-record accesses.
It must be proven for nested traces, callable shaders, and shader-call memory
ordering. Intersection shaders cannot read ray payloads; a hidden payload alone
is not a complete context transport design.

### Synchronization and state preservation

Translate micromap stages/access masks into the actual compute, transfer, native AS,
and instrumented-shader accesses. Cover barriers, events, queue ownership, semaphore
dependencies, query reset/availability, and query-result copies. Cross-command and
cross-queue metadata publication must follow application synchronization.

Internal work must preserve application command state and support command-buffer
reuse, secondary command buffers, and concurrent submissions where valid. Avoid
CPU reads of GPU-generated inputs or hidden queue-idle waits as a correctness
shortcut. Define how injected accesses participate in buffer-scoped dependencies.

## Decisions and unresolved proof gates

The native-RT/shared-core split and EXT-first scope are decisions. The internal
storage ABI, metadata resource delivery, shader context ABI, and exact SPIR-V
transforms remain proposals until tested.

Before declaring support, prove AS operand provenance, transparent resource
delivery at application limits, full Ray Query state semantics, procedural shader
side-effect preservation, zero/null hit-group handling, and GPU-generated SBT and
instance paths. Also audit feature interactions such as descriptor buffers,
pipeline libraries, shader objects, and indirect tracing wherever exposed.

Do not silently accept an unsupported path with changed semantics. During
development, incomplete prototypes must remain explicitly experimental and must
not be presented as a complete advertised extension implementation.

## Completion criteria

The implementation must satisfy the mandatory EXT device API and all native
consumer paths it exposes, across the limits and optional-feature combinations it
reports. Test both application-visible results and shader side effects; an image
match alone is insufficient. Use CTS coverage where available plus dedicated
regressions. Passing local tests alone is not a formal conformance claim.

See the [implementation plan and acceptance matrix](opacity_micromap_plan.md).

## References

- [VK_EXT_opacity_micromap](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html)
- [VK_KHR_opacity_micromap](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_opacity_micromap.html)
- [VK_KHR_device_address_commands](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_device_address_commands.html)
- [Ray traversal and opacity rules](https://docs.vulkan.org/spec/latest/chapters/raytraversal.html)
- [Micromap build and input lifetime](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBuildMicromapsEXT.html)
- [Triangle-to-micromap association](https://docs.vulkan.org/refpages/latest/refpages/source/VkAccelerationStructureTrianglesOpacityMicromapEXT.html)
- [EXT feature reporting](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceOpacityMicromapFeaturesEXT.html)
- [Ray tracing pipelines and shader binding tables](https://docs.vulkan.org/spec/latest/chapters/raytracing.html)
- [Shader execution environments](https://docs.vulkan.org/spec/latest/chapters/shaders.html)
- [SPV_KHR_ray_query](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_ray_query.html)
- [SPV_EXT_opacity_micromap](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_opacity_micromap.html)
