<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Descriptor budget and public RTP boundary evidence, 2026-10-26

The descriptor profile planner is implemented and tested but is **not applied to
global physical-device discovery**. It is one prerequisite for the full resource
route, not a complete M0 qualification gate. Native allocation pooling for AS
address cells and radix-build resources remains open, as do complete capability,
property and device-create qualification on other devices.

## Representation and budget

An ordinary AS descriptor uses one native storage-buffer descriptor containing
the AS address cell. All six RT shader stages map to native compute. The global
descriptor-set caps therefore bound the union of all RT stages; dividing each
native limit by six is unnecessary. Native per-stage SSBO accounting includes
static SSBOs, dynamic SSBOs and translated AS descriptors. Native static-set SSBO
accounting excludes dynamic SSBOs.

For the tested device, native compute and static-set SSBO limits are both
1,048,576. Reserving the mandatory 16 AS descriptors and retaining 16 dynamic
SSBOs gives public per-stage SSBO 1,048,560 and static-set SSBO 1,048,544. The
static-set UBO cap is 1,048,561 while retaining 15 dynamic UBOs. The pure planner
also checks feature-dependent update-after-bind maxima, aggregate resource
limits, inline blocks, maintenance7 totals, mandatory Vulkan minima and checked
arithmetic. It leaves output untouched on failure.

The capture/apply helpers preserve `sType`, `pNext` and unrelated properties,
including Vulkan12/Vulkan13 promoted aliases. AS update-after-bind remains false.
The profile unit test independently checks generated maximal layouts, 1,000
asymmetric randomized native profiles, exact minimum boundaries and UINT32_MAX
arithmetic.

## Shared public descriptor handling

`vkrt_descriptor_layout.c` supplies the translation used by public layout
creation, core/KHR layout-support queries and descriptor-pool creation. It
preserves binding order, original chains and mutable-list indices. Descriptor
pool AS rows become SSBO rows independently, retaining legal duplicate rows
without a uint32 merged-count overflow.

Layout-support queries retain native creatability and native variable-count
results. They are not clamped to pipeline AS16 limits: Vulkan explicitly makes
this query independent of pipeline resource budgets. A requested 17-AS variable
layout can therefore be creatable even when a pipeline using all 17 descriptors
would exceed the advertised pipeline cap. Unsupported AS mutable/descriptor
buffer/push-descriptor encodings fail closed.

The NV per-stage feature structure is handled field by field. Virtual RT masks
`perStageDescriptorSet` and rejects enabling it, while preserving the native
`dynamicPipelineLayout` feature and ordinary-device passthrough. A downstream
query that ignores the structure cannot leak caller-initialized true bits.

## Native descriptor object proof

`test_resource_profile_gpu.c` enables no native AS/RQ/RTP extensions or features.
It translates and successfully queries, creates and allocates the complete
AS16 + staticSSBO1,048,544 + dynamicSSBO16 layout, then creates its native compute
pipeline layout. This fills the native per-stage SSBO limit exactly. Khronos
validation reports no diagnostics.

Evidence: `build-resource-profile/native-budget-validation.log`.

## Actual public RTP source execution

`test_resource_boundary.c` and `resource_boundary.{rgen,rmiss,rchit}` use the
public AS and RTP entrypoints. GPU builds produce a triangle BLAS, a hit TLAS and
an empty TLAS. The source raygen shader dynamically selects AS15/AS0, reads and
writes the last static and dynamic SSBO array elements, and reads the last word
of the complete application push-constant range.

On the tested device the pipeline occupies the last legal descriptor set
(31 of 32), all 256 application push bytes, all 16 AS descriptors and the full
native translated SSBO budget. All 1,048,544 static descriptors and 16 dynamic
descriptors are populated. Ordinary data buffers use STORAGE_BUFFER only; BDA
usage is explicitly requested only for AS/build/scratch/SBT buffers. The two
traces independently verify miss/hit results 89/99 and a static-buffer write of
123. An unchanged native-hardware control passes the same shader inputs and
checks.

The boundary exposed a real source-compiler bug: recursively referenced resource
array types were all classified as continuation values, causing a descriptor
array to hit the 4,096-element aggregate-codec restriction. Declaration reachability
and actual value-codec requirements are now separate. A resource-only array keeps
its native declaration without an element codec; concrete SSA aggregate values
and persistent pointees still receive their logical codecs. A focused 1,048,544
SSBO/AS16 source regression and existing aggregate `OpCopyLogical` tests pass.

The boundary also binds descriptors through a second pipeline layout created
with a distinct, identically defined descriptor-set layout. Its default mode
retains descriptor-set layouts until commands complete; the explicit
`--destroy-dsl-before-pipeline` mode destroys all DSL handles after pipeline-layout
creation and before RT-pipeline creation. This exposed handle equality in the RT layout
compatibility check. Descriptor definitions are now copied at creation and
retained by pipeline layouts. Compatibility compares original public binding
types, stages, counts, creation flags, binding flags, immutable sampler object
handles and mutable type lists, and includes the pipeline independent-sets flag.
Snapshots survive caller-memory changes, DSL destruction and handle reuse.
Focused tests distinguish different definitions and verify this lifetime.

The [Vulkan compatibility rules](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#descriptorsets-compatibility)
require identically defined layouts, rather than equal handles; the
[definition of object identity](https://docs.vulkan.org/spec/latest/appendices/glossary.html#glossary-identically-defined)
requires immutable sampler handles to refer to the same objects.

With the default retained-DSL mode, software, both validation-layer orders and
synchronization validation pass with no Vulkan warnings or errors. The native
RT control also passes with synchronization validation. Early destruction passes
execution with both backends and validation above the software layer, but the
SDK validation layer below it diagnoses a stale deleted-DSL child of the native
compute pipeline layout. Destroying the DSL after pipeline creation similarly
causes a stale-child pipeline-bind diagnostic.

This SDK issue is independently reproduced by `test_dsl_lifetime.c` with a native
public compute pipeline, no software layer and no native RT features. Destroying
the DSL before compute pipeline creation causes the same diagnostic, while
retaining it yields zero diagnostics. The native compute pipeline dispatches
successfully in both modes. `VkPipelineLayoutCreateInfo::pSetLayouts` requires
the implementation not to access DSL objects outside the command duration; the
software implementation retains copied definitions and already-created native
pipeline layouts, without reopening destroyed DSL handles.

Evidence:

- `build-resource-profile/public-boundary.log`
- `build-resource-profile/public-boundary-validation-above.log`
- `build-resource-profile/public-boundary-validation-below.log`
- `build-resource-profile/public-boundary-syncval.log`
- `build-resource-profile/public-boundary-native-control.log`
- `build-resource-profile/resource-codec-tests.log` (3/3 compiler/frame tests)
- `build-resource-profile/descriptor-identity-tests.log` (3/3 identity/boundary tests)
- `build-resource-profile/descriptor-identity-syncval.log` (destroyed distinct DSLs)
- `build-resource-profile/descriptor-identity-native-syncval.log` (default mode)
- `build-resource-profile/descriptor-identity-early-dsl-validation-below.log` (SDK diagnostic)
- `build-resource-profile/dsl-lifetime-native-compute.log` (native compute reproduction)
- `build-resource-profile/dsl-lifetime-retained-compute.log` (retained native compute control)

These are resource-boundary proofs, not substitutes for Vulkan CTS. CTS progress
and the remaining RT goal are tracked in `rt_cts_typed_runtime_2026-10-26.md`.
