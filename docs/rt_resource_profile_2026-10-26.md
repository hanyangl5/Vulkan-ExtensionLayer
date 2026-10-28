<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Resource profile, discovery and public RTP boundary evidence, 2026-10-26

The descriptor profile is now applied consistently to physical-device discovery
and virtual-RT device creation on qualifying native-RT devices. The registry
header, sparse radix pages and shared AS descriptor cells now use a device-owned
native allocation pool. These are concrete steps toward M0; they do not complete
M0 or the full AS/RQ/RTP goal. The complete private allocation-count budget and
qualification of physical devices without native RT remain open.

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
including Vulkan12/Vulkan13 promoted aliases. All four AS descriptor property
caps are 16: per-stage and per-set, both ordinary and update-after-bind-named.
`descriptorBindingAccelerationStructureUpdateAfterBind` remains false.
The profile unit test independently checks generated maximal layouts, 1,000
asymmetric randomized native profiles, exact minimum boundaries and UINT32_MAX
arithmetic.

## Discovery and device-create qualification

`vkrt_virtual_rt_resource_profile` combines the existing native backend component
prerequisites with an actual native limit/feature capture and the pure descriptor
planner. A failed query or insufficient budget leaves its output untouched. The
same qualification controls virtual-RT device creation, base AS/RQ/RTP feature
visibility and enumeration of their three extensions. An insufficient profile
therefore cannot advertise base RT support and then pass a different resource
check at device creation.

Legacy `vkGetPhysicalDeviceProperties`, core Properties2 and its KHR alias apply
the same profile. Properties2 first uses the shader-object property wrapper and
then applies the resource fields, preserving the wrapper's UUID and unrelated
caller output. The public output chain is queried once. Promoted property
aliases retain their original `sType` and `pNext` links.

The property clamp additionally requires native base AS, RQ or RTP feature truth.
Physical devices without native RT retain ordinary native properties, and this
change does not add base AS/RQ/RTP support to them. Ordinary devices that do not
request virtual RT retain their existing device-create path. The component-only
`vkrt_native_backend_eligible` predicate used by shader objects is unchanged;
descriptor-profile qualification does not change the shader-binary UUID domain.

Host coverage includes native 1.2/1.3/1.4 capture, mandatory-minimum failures,
legacy/core/KHR property agreement, feature/extension hiding on a failed budget,
ordinary no-RT passthrough and composition with a distinct UUID wrapper. Evidence:

- `build-source-layout/resource-profile-integration-run.log`
- `build-source-layout/device-features-resource-run.log`
- `build-source-layout/resource-profile-all-as-run.log`
- `build-source-layout/discovery-all-as-run.log`

### AS limits and the first validation failure

The first real pool-pressure validation run exposed a property bug: the ordinary
AS caps were 16, but both update-after-bind-named caps were zero. A pipeline
layout containing one ordinary AS descriptor then triggered
`VUID-VkPipelineLayoutCreateInfo-descriptorType-03572` and `03574`. The unedited
failure is retained in
`build-source-layout/resource-collapse-pool-validation-first-failure.log`.
That run also ended with a missing `rq_test.comp.spv` caused by its working
directory; it is not a successful execution or validation result.

This is a version-sensitive rule, not evidence that AS update-after-bind must be
enabled. The [Vulkan-Docs v1.4.357 descriptor-set rules](https://github.com/KhronosGroup/Vulkan-Docs/blob/v1.4.357/chapters/descriptorsets.adoc)
and SDK 1.4.357.0 validation limit all AS bindings directly against the two
update-after-bind-named properties. The [latest pipeline-layout reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineLayoutCreateInfo.html)
instead uses the maximum of the corresponding ordinary and update-after-bind
properties. The [required-limit table](https://docs.vulkan.org/refpages/latest/refpages/source/Required_Limits.html)
requires ordinary AS minima of 16 and permits zero update-after-bind limits when
that feature is unsupported; it does not require zero. Advertising all four
caps as 16 satisfies both counting rules without advertising the conditional
500,000-descriptor AS update-after-bind feature profile.

The existing union budget already reserves these same 16 ordinary AS
descriptors. They are counted once alongside the maximum permitted ordinary/UAB
SSBO counts; there is no second independent AS16 reservation. If native SSBO
update-after-bind is supported, its native limits are at least the ordinary
limits. Focused tests now check the direct native UAB per-stage and set bounds
as well as the existing max-pair proof. The corrected profile and discovery host
tests pass. The subsequent VVL/synchronization-validation reruns were still in
progress when this section was updated; no completed rerun result is asserted
here.

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

## Shared private storage

`vkrt_private_pool.c` now supplies storage for the registry header and sparse
radix pages, plus descriptor address cells shared by AS aliases at the same
buffer offset. The backing chunks are host-visible, host-coherent storage/BDA
buffers with a minimum chunk size of 64 KiB. Chunks and bookkeeping have device
lifetime and use the device allocator, falling back to the instance allocator
when appropriate. Freed slices can be reused, and an empty chunk is released.

CPU mapping alignment, absolute BDA alignment and native storage-buffer
descriptor offset alignment are handled separately. Sparse pages/header slices align their
actual device addresses without assuming that the native buffer's base BDA is
already aligned. Descriptor slices instead align their buffer-relative offset
to the queried native `minStorageBufferOffsetAlignment`. CPU access uses copies
that do not assume the mapped pointer has the BDA's alignment.

The root integration's three focused host checks pass:

- `build-rt-cts-evidence/cleanup-query/as-address-pool.txt`
- `build-rt-cts-evidence/cleanup-query/as-registry-pool.txt`
- `build-rt-cts-evidence/cleanup-query/host-translation-pool.txt`

The registry check includes 24 sparse views using three native allocations
against a four-allocation test budget, 26 fault positions and 8,000 concurrent
slice allocations. Expected negative-control errors in these host logs are
separate from VVL diagnostics. The full offline run is 25/25 in
`build-source-layout/resource-collapse-offline.log`. The first real GPU round
passes 6/6 in
`build-source-layout/resource-collapse-build-gpu-first.log`:

- `vkrt.gpu_build.software` and `vkrt.gpu_build.native`
- `vkrt.gpu_build.aligned_values` and `vkrt.gpu_build.aligned_values.native`
- `vkrt.gpu_build.pool_pressure` and `vkrt.gpu_build.pool_pressure.native`

These logs establish the recorded runs, without attributing them to an older
DLL hash. The first round is separate from the later validation rerun. Pooling
these AS resources removes their former per-object/per-page native allocation
pattern, but it does not yet prove the complete budget for every private build,
query and pipeline allocation or the full-device `maxMemoryAllocationCount`
contract. M0 remains open.

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
