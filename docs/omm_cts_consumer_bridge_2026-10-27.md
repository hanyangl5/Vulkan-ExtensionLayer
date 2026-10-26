<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# CTS OMM consumer bridge audit, 2026-10-27

This source audit identifies the next automatic consumer integration work for the
unchanged complete EXT goal in [the specification](opacity_micromap_layer.md) and
[implementation plan](opacity_micromap_plan.md). The proof matrix below is an
integration sequence, not a reduction of that goal. No CTS case was rerun for
this audit. The composer experiments at the end are separate local proofs.

## Pinned inputs and interpretation

| Input | Value |
| --- | --- |
| CTS checkout | `D:/Codes/VK-GL-CTS` |
| CTS revision | `d18e4c68543103371715845cd914c71e5859963e` |
| CTS executable | `build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe` |
| Executable SHA-256 | `8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380` |
| QPA release | `vulkan-cts-1.4.7.0-103-gd18e4c68543103371715845cd914c71e5859963e` |
| Probe list | [omm_cts_experimental_probe_cases.txt](omm_cts_experimental_probe_cases.txt) |
| Probe-list SHA-256 | `8f541207458632b3204036ad98eac88ede156f57c188171933264dcaa495a747` |
| Existing admission evidence | `build-omm-cts-evidence/layer-software-admission/summary.json` |

The six source files referenced below were compared with their pinned HEAD blobs,
normalizing CRLF/LF; all six match. No CTS source or executable was modified.
Source aliases used for precise line references are:

- **RQ**: `external/vulkancts/modules/vulkan/ray_query/vktRayQueryOpacityMicromapTests.cpp`.
- **RTP**: `external/vulkancts/modules/vulkan/ray_tracing/vktRayTracingOpacityMicromapTests.cpp`.
- **ASUtil**: `external/vulkancts/framework/vulkan/vkRayTracingUtil.cpp`.
- **Builder**: `external/vulkancts/framework/vulkan/vkBuilderUtil.cpp`.
- **Features**: `external/vulkancts/framework/vulkan/vkDeviceFeatures.cpp`.
- **Context**: `external/vulkancts/modules/vulkan/vktTestCase.cpp`.

The existing software-admission run selected exactly:

1. `dEQP-VK.ray_query.opacity_micromap.ext.copy.clone.2.level_1`
2. `dEQP-VK.ray_query.opacity_micromap.ext.render.compute_shader.no_flags.map_value.2.level_1`
3. `dEQP-VK.ray_tracing_pipeline.opacity_micromap.ext.query.compacted_size`

That historical run recorded three `Crash` outcomes with no QPA test result.
Stdout locates the failure at default device creation with
`VK_ERROR_FEATURE_NOT_PRESENT`, before shader or AS consumer execution. The
experimental RQ/RTP feature gate explains this admission failure; it is neither
an EXT `NotSupported` result nor a software rendering pass. CTS normally queries
and passes a broad supported-feature chain into default device creation
(Features:190; Context:393-398, 943-948). Consequently, even the property-query
probe can encounter that gate without using a ray query or tracing pipeline.
See also [the loader checkpoint](omm_layer_api_2026-10-27.md).

## The two RQ probes: exact resource and command chain

The clone case uses compute, flags zero, format 2-state, subdivision level 1,
UINT32 association indices, no nonzero base, no special index, no null micromap
and no mixed KHR OMM (RQ:1534-1566). The render case selects the same simple
consumer without a micromap copy (RQ:1447-1461).

| Step | Original source | Actual input or behavior |
| --- | --- | --- |
| Input allocation | RQ:573-611 | One triangle and four microtriangles; one packed data byte. Host-visible BDA input has size 1024 plus that byte, triangle records at 0, index at 256, packed data at 512. Usage includes MICROMAP_BUILD_INPUT_READ_ONLY and SHADER_DEVICE_ADDRESS. |
| Triangle/index records | RQ:627-655 | Micromap triangle dataOffset 0, subdivisionLevel 1, format 2-state; UINT32 association index value 0. |
| Size and build | RQ:666-737 | DEVICE build-size query, flags 0, BUILD mode, one direct pUsageCounts, triangle stride 8; backing and scratch are allocated, then CmdBuildMicromaps receives the three BDA inputs. |
| Build publication | RQ:740-757 | Sync2 MM_BUILD/MM_WRITE to MM_BUILD/MM_READ for the clone path, or AS_BUILD/MM_READ for the direct path. |
| Clone | RQ:760-801 | A second backing buffer and micromap are created; CmdCopyMicromap uses source original and destination second handle. A second MM_BUILD/MM_WRITE to AS_BUILD/MM_READ barrier follows. |
| Association | RQ:807-819 | VkAccelerationStructureTrianglesOpacityMicromapEXT is attached to triangles.pNext: UINT32 index buffer at BDA+256, indexStride **0**, baseTriangle 0, one direct usage count, original or cloned micromap as appropriate. |
| Geometry | RQ:821-832; ASUtil:178-183 | One nonindexed R32G32B32 triangle at (0,0,0), (1,0,0), (0,1,0); geometric indexType NONE; geometry flags 0. |
| BLAS | RQ:862-869 | Native BLAS createAndBuild with build flags 0 for these cases. |
| TLAS | RQ:1000-1012 | One identity instance, flags 0, custom index 0, mask 0xff and SBT offset 0. |
| Descriptor | RQ:1138-1174 | Set 0 binding 0 AS, binding 1 origins SSBO, binding 2 output SSBO, all stage flags ALL. An AS descriptor write contains the native TLAS handle. |
| Dispatch | RQ:1258-1285 | Shader module and compute pipeline creation, compute pipeline and descriptor set binds, then Dispatch. |
| Completion/lifetime | RQ:1289-1301 | Shader-write to host-read barrier, one submit and wait, then micromap destruction. Input, scratch, backing and AS local owners outlive traversal. |

Index stride zero is an actual input, not permission for the layer to substitute
sizeof(uint32_t). It addresses the same association element for every primitive;
this one-triangle case alone cannot distinguish those behaviors.

There is also a pinned-source coverage trap: the branch at RQ:772-777 always
passes `VK_COPY_MICROMAP_MODE_CLONE_EXT`, including when its outer parameter is
CT_COMPACT. A `copy.compact` case name in this revision therefore does not itself
prove that a COMPACT command executed. This does not change historical outcomes.
The separate public API test explicitly issues COMPACT and provides different
coverage.

### AS plumbing that consumer interception must reach

ASUtil defaults to DEVICE build, flags 0 and pGeometries rather than an array of
geometry pointers (ASUtil:1418-1427). Its BLAS size-query preparation forwards the
logical geometry chain to GetAccelerationStructureBuildSizesKHR
(ASUtil:1691-1727). The recorded build prepares the same logical geometry and
calls CmdBuildAccelerationStructuresKHR (ASUtil:2010-2059). Range offsets are
primitiveOffset 0, firstVertex 0, transformOffset 0
(ASUtil:2339-2351); the OMM association is installed at
geometry.triangles.pNext (ASUtil:2372-2380), and original geometry flags are
retained (ASUtil:2453-2478). createAndBuild performs both operations
(ASUtil:2487-2495).

The TLAS instance records contain the native BLAS device address obtained using
GetAccelerationStructureDeviceAddressKHR (ASUtil:3592-3627). They are CPU-produced
in these probes, then uploaded before the native TLAS build
(ASUtil:4203-4264). Instance custom index 0 is application data; it is not a unique
sidecar identity. The native AS address must remain valid for traversal.

### Shader semantics and metadata provenance

RQ:286-296 declares the AS as a descriptor-backed `accelerationStructureEXT` at
set 0 binding 0. It is not obtained by converting a device address into an AS.
The descriptor builder ultimately calls UpdateDescriptorSets
(Builder:253-277). Automatic metadata transport must therefore follow descriptor
provenance through an opaque AS load; an AS-to-integer cast is not an available
shortcut.

The SPIR-V target is 1.4 (RQ:260). At RQ:300-318 the shader initializes the query
with flags 0, origin from the SSBO, direction (0,0,-1), tMin 0 and tMax 2. It writes
1 for an exposed triangle candidate and 2 for a committed triangle. It never
calls ConfirmIntersection. Centroid rays originate at (u,v,1)
(RQ:1111-1127). Local size 128 and eight workgroups supply 1024 invocations, but
only the first four rays are active at level 1 (RQ:116-120, 300).

For these 2-state/no-flags probes, transparent intersections must disappear and
opaque intersections must auto-commit: expected outputs are 0 or 2. Returning an
opaque intersection as an application-visible candidate produces 1 and violates
the test. Preparing shadow query flags alone does not implement that behavior.
The Proceed loop needs a classifier-backed filter and auto-confirm path that
also preserves native candidate/committed state and termination semantics.

These cases keep all input resources alive until traversal finishes. They do not
prove GPU-generated index/instance capture, legal post-build input reuse,
early micromap destruction after the relevant dependency, update/copy provenance
or execution-time command-buffer replay.

## The property query probe does not execute an RTP consumer

`ray_tracing_pipeline.opacity_micromap.ext.query.compacted_size` resides under an
RTP-named group but its implementation requires AS and EXT micromap support only
(RTP:1610-1623). It has no BLAS, TLAS, descriptor, shader module, tracing pipeline,
SBT or TraceRays operation.

RTP:1640-1671 creates a single 2-state level-0 micromap from a triangle record at
0 and a zero data byte at 512 in a 1025-byte input. It uses ALLOW_COMPACTION
(RTP:1673-1675, 1945), queries build sizes, creates and builds the micromap
(RTP:1693-1720), then issues an MM_BUILD/MM_WRITE to MM_BUILD/MM_READ barrier
(RTP:1722-1734). It creates a MICROMAP_COMPACTED_SIZE query pool, resets it, records
CmdWriteMicromapsPropertiesEXT (RTP:1737-1746), submits and waits, and obtains a
64_BIT|WAIT_BIT result, checking only that it is nonzero (RTP:1748-1757).

Removing the feature-admission gate is insufficient for this probe. The
micromap query-pool, property-write and result path also needs implementation,
including completion and availability behavior. Historical native controls fail
at CreateQueryPool with VK_ERROR_UNKNOWN; the cause remains unresolved. Those
controls are not expected-pass evidence or a NotSupported outcome. See
[the native inventory](omm_cts_inventory_2026-10-26.md).

A distinct existing shader consumer is
`dEQP-VK.ray_tracing_pipeline.opacity_micromap.ext.no_flags.map_value.4.level_1`.
Its raygen uses the AS descriptor and TraceRay; its application any-hit writes
payload 1 and terminates, closest-hit writes 2 unless payload is already 1, and
miss writes 0 (RTP:182-277, especially 244-245, 257-259 and 270). Native SBT groups
are raygen 0, miss 1 and hit 2 with AH plus CH (RTP:918-940), followed by TraceRays
at RTP:947. This is useful future RTP integration evidence; it was not run in
this audit. The separate manual 37-case RTP wrapper proof is not automatic
instrumentation of this CTS shader.

## Required automatic bridge and resource transport

| Boundary | Required behavior |
| --- | --- |
| AS objects and device addresses | Track create/destroy and handle/native-address identity without parsing opaque BVH storage or changing the native address used by instances. |
| AS size and recorded build | Sanitize copied native geometry chains/OMM flags for both size and build; preserve logical association for the sidecar. Never forward a layer-owned micromap handle into native AS processing. |
| GPU association capture | Capture indices, base triangle and instance records in the GPU build dependency. Preserve logical primitive/instance/geometry IDs; do not assume custom index uniqueness or CPU-readable input. |
| AS copy/update/replay | Carry compatible sidecar state at execution time; a CPU snapshot taken when a copy CB is recorded can be stale when it executes or replays. Explicitly guard unsupported host/indirect entry points. |
| Descriptor lifecycle | Track set layouts, pools, allocation/free/reset/destruction, AS descriptor writes/copies and command-buffer descriptor binds. Pair the shader's AS value with the correct live metadata version. |
| Shader/pipeline lifecycle | Retain original modules and instrument actual pipeline creation and execution. Query object preparation plus shader-library composition does not by itself replace Proceed/classification. |
| Broader AS sources | Provide explicit transport for helper arguments, aliases, dynamic descriptor indices, templates, push descriptors and descriptor buffers before admitting those supported application paths. |
| RTP consumers | Preserve pipeline groups, native SBT records, payload/hit attributes, TraceRay context, unknown application-AH behavior, opaque AH bypass and transparent rejection. Procedural opacity decisions must precede application intersection side effects. |

The BDA classifier needs a root metadata address and provenance for the loaded
AS. The sparse CTS layout does not establish an application-at-limits resource
contract. A general implementation cannot assume a spare binding, spare set,
spare push-constant bytes, unused specialization IDs, or a chosen instance custom
index. Any private descriptor/layout transport must preserve original layout
compatibility, limits, descriptor contents and shader semantics. A temporary
restricted admission mode must identify and reject unsupported layouts rather
than silently overwrite an application resource. Such restrictions remain
unfinished work against the full goal.

## Private GPU sidecars add synchronization scopes

Existing micromap translation maps MM_BUILD to COMPUTE and MM_READ/WRITE to
shader accesses, and includes compute when MM_READ is paired with AS_BUILD.
The AS consumer bridge introduces separate private GPU buffers beyond those
micromap operations. The original BLAS helper publishes AS_READ|AS_WRITE through
a legacy barrier (ASUtil:2093-2101), and TLAS publishes those accesses to
ALL_COMMANDS (ASUtil:4298-4306). These ordinary AS barriers need not contain any
micromap bit.

The implementation must bridge logical AS build publication and logical AS
shader reads to actual sidecar compute writes and instrumented shader reads.
Replacing only micromap mask bits cannot establish this. A buffer-scoped barrier
on the public native-AS buffer does not automatically cover a different private
sidecar allocation. Resource ranges, events, queue-family transfer, semaphore
stage boundaries and the private compute operation all need a defined mapping.
In particular, an AS_BUILD-only semaphore scope must include whichever actual
operations implement the logical AS work. Transitive visibility must be proved
from real dependencies; it cannot be assumed from command order. Hidden queue
idle or CPU readback would not establish the intended GPU implementation.

The selected probes use one queue and global barriers, so passing them would not
qualify the full scope, lifetime or cross-queue matrix. Keep independent tests
for buffer-scoped dependencies and GPU-generated/reused inputs.

## Minimum next end-to-end proof matrix

| Proof | Required observable evidence |
| --- | --- |
| Unmodified descriptor-source compute RQ | Known mixed 2-state data gives transparent 0 and opaque 2, never candidate 1; original shader/workgroup behavior survives. |
| Exact clone probe | Traversal consumes the copied micromap's content and identity, not a stale original or native micromap. |
| Descriptor write/copy/rebind | Multiple AS objects and descriptor versions select the matching sidecar through the actual public descriptor path. |
| GPU-generated indices and instances | GPU-written data drives the correct classification and logical IDs without CPU readback. |
| Mixed ordinary triangles/AABBs | Candidate/committed getters, Confirm, Generate and logical opacity flags remain correct for non-OMM geometry. |
| AS clone/update and CB replay | Metadata follows actual execution; build inputs can be reused after completion without corrupting traversal. |
| Public micromap properties | Reset/write/get/copy/availability use real completion semantics and do not forward layer-owned handles. |
| Actual four-state RTP rendering | An unmodified RTP shader observes miss 0, application AH 1 or opaque CH 2 with correct side effects and SBT/context behavior. |

Retain VVL above and below the layer, synchronization validation, immutable
binary/manifest snapshots, exact case names, and result categories. An admission
failure, a manually composed shader and a native control must remain distinct
from an external CTS software pass.

## Composer audit: three findings fixed and independently rechecked

The reviewed component is `layers/raytracing/omm/omm_shader_compose.cpp`.
Experiments used SDK 1.4.357.0 glslang/SPIRV-Tools, validated Vulkan 1.2 inputs and
outputs, and independent binaries under `build-omm-cts-audit/composer`. They did
not edit production sources or load the software OMM layer. Current production
fixes and parent-owned regressions cover all three findings below.

| Finding | Original reproduction | Fixed independent result |
| --- | --- | --- |
| Helper WorkgroupSize leaked into the application | Actual glslang BDA compute library emitted BuiltIn WorkgroupSize 1x1x1. Composition and validation succeeded for an application LocalSize 4x2x1, but the native RTX 3080 executed only one invocation. WorkgroupSize has precedence over LocalSize. | Helper BuiltIn WorkgroupSize decoration is stripped while source decoration is preserved. The same native GPU probe now executes eight invocations. |
| Persistent OOM escaped the status API | The bad_alloc path constructed another allocating diagnostic string. Sustained allocation failure at budgets 0,1,2,4,8 threw despite leaving output unchanged. | Failure reporting uses string_view/noexcept and catches diagnostic assignment allocation failure. All five independent budgets return OutOfMemory without an escaping exception and leave output unchanged. |
| Helper SpecId collided with application specialization | Valid source and helper both used SpecId 7, with different defaults. Old composition/validation succeeded with both decorations, allowing application specialization to change helper semantics. | Helper SpecId decorations are rejected with InvalidLibrary. Source specialization is preserved. The independent collision probe now rejects the helper. |

The WorkgroupSize precedence rule is in the local pinned specification at
`D:/Codes/khr/vulkan/chapters/interfaces.adoc:5935-5938` and the
[Vulkan Guide](https://docs.vulkan.org/guide/latest/compute_shaders.html#workgroup-size).
The real library has BDA loads, physical-storage addressing and Function locals,
without helper global variables. The scalar thunk preserves its seven-argument
classifier ABI. Composition promotes the addressing model while preserving the
source memory model; that is not a proof for arbitrary future helper atomics,
barriers, globals or specialization. Those changes need a renewed audit.

An additional valid SPIR-V 1.3 source omitted an SSBO from its entry-point
interface, then linked with the newer helper successfully and validated. The
linker rebuilt the required final interface; this was not a reproducible current
defect. No other reproducible current composer defect was found in this review.

Retained artifacts, relative to the worktree:

| Artifact | Evidence or SHA-256 |
| --- | --- |
| `build-omm-cts-audit/composer/library.comp`, `library.spv` | Actual glslang-generated BDA helper, not only hand-authored IR. |
| `build-omm-cts-audit/composer/gpu_probe.cpp`, `gpu-old.exe`, `gpu-fixed.exe` | One-workgroup atomic invocation counter; old exit 2, fixed exit 0. |
| `build-omm-cts-audit/composer/gpu-old.log` | Observed 1; `e2760b7daa67efddd2b4f743744bb5e8cc31063dd8809df958da4853392ffa1f`. |
| `build-omm-cts-audit/composer/gpu-fixed.log` | Observed 8; `8f364044979728d6ef3438bcf5bb4f8d721c89696ca008aefc68a202bcb7ebff`. |
| `build-omm-cts-audit/composer/fixed-results.log` | Persistent OOM returned status 5, no escape, unchanged output; `5ccb4cd16d071183057be62715e5d1ca9dba0d4b761a56a70a77ae99bf5324bc`. |
| `build-omm-cts-audit/composer/version-specid-fixed.log` | SpecId conflict status 2 and valid SPIR-V 1.3 interface success; `50812cc877a4566c502ad9a39d4918d9339e9a3808bce3370b140be22c190e15`. |
| `build-omm-cts-audit/composer/omm_shader_compose_snapshot.cpp` | Old reproduced source snapshot; `4ad2e1ddc47a11e19338b6941982f4d5e367b590d0faf2de3f7207f56f7e3187`. |
| `build-omm-cts-audit/composer/omm_shader_compose_specid_fixed.cpp` | Rechecked fixed source snapshot; `a200349c25f01f57afe3c7fc043e72b884d638f0ca75a306e04693cf87b247d5`. |
| `build-omm-compose/workgroup-oom-specid-fixed.log` | Parent-owned fixed host suite: 194 checks, 0 failures, including more sustained OOM budgets. |

These logs are local execution artifacts, not tracked external CTS pass reports.
