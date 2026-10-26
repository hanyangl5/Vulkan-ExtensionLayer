<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# GPU metadata producer and packed-data query classifier

This checkpoint follows `4377d0f` toward the unchanged complete EXT goal. It
connects the actual GPU micromap builder to a reusable metadata producer and the
automatic native RQ transform. The classifier now reads real packed OMM storage,
not encoded CPU-oracle decisions. Public AS association, descriptor/integer AS
provenance and binding-context delivery are still required. No full milestone is
complete, and the experimental loader's consumer/AS-build gates remain in place.

## Runtime metadata contract

`omm_metadata.h` defines a versioned, BVH-independent wire layout:

| Structure | Bytes | Meaning |
| --- | ---: | --- |
| RootCell | 64 | Stable owned identity, body address/span and generation |
| BodyHeader | 64 | Kind, active/capacity bounds, record count and generation |
| TLAS instance | 32 | BLAS RootCell identity, opaque native BLAS reference, original flags and active state |
| BLAS geometry | 64 | Original type/flags, primitive count, normalized association and inline OMM snapshot spans |

Instance records retain the original native instance ID, including inactive
positions; custom indices are not metadata indices. A native AS address is an
opaque identity and is never dereferenced or given an invented alignment rule.
All dereferenced BDAs refer to explicitly owned/readable allocations. Bounds
checks cannot authenticate an arbitrary nonzero BDA.

The normalized association is one uint64 per primitive. The top four values
encode the four special states; ordinary index-plus-base arithmetic retains its
64-bit carry. The independent OMM stream starts at primitive zero; it is not
offset by geometry vertex/index `primitiveOffset` or `firstVertex`. This follows
the separate definitions of the [build range](https://docs.vulkan.org/refpages/latest/refpages/source/VkAccelerationStructureBuildRangeInfoKHR.html)
and [OMM association](https://docs.vulkan.org/refpages/latest/refpages/source/VkAccelerationStructureTrianglesOpacityMicromapEXT.html);
a nonzero-offset native differential case remains required for public integration.

The BLAS body owns both normalized indices and a snapshot of the canonical OMM
bytes. It does not retain application micromap/index buffers. Snapshot capacity
can exceed its actual GPU header size. Body active bytes are bounded by body
capacity, which is bounded by the RootCell's known backing span. This runtime
pointer-bearing layout is not public Vulkan serialization.

The checked host planner/reference supports ordinary triangles/AABBs, both EXT
index widths, implicit association, special-only null micromaps, validation and
relocatable body cloning. A two-phase compute kernel produces the same body from
GPU index and canonical OMM inputs, separated by a compute dependency. Phase-1
status accesses use atomics; invalid status never authorizes publication. Public
AS-owned allocation, root publication, update/clone/serialization propagation and
safe retirement still need implementation. Resetting a CB must not release a
body that an AS continues to reference.

## Classifier composition and execution

`omm_classifier.glsl` resolves RootCell → TLAS instance → BLAS geometry → normalized
association → inline OMM snapshot. It reuses `omm_lookup.glsl` and
`omm_opacity.glsl`. The scalar callback receives explicit scene metadata and
native instance/geometry/primitive IDs plus native barycentrics. Invalid metadata
is a diagnostic failure, not permission to treat an unknown AS as ordinary.

`omm_shader_compose` links the classifier closure into an application module. A
scalar thunk creates seven local memory objects before calling glslang's
pointer-parameter helper, preserving Vulkan's function-call pointer restrictions.
It adds no descriptor, push-constant range or shader-global resource. The source
entry points and GLSL450/Vulkan memory model survive, with physical-storage
addressing and Int64 required by the internal code. Both inputs and the final
module are validated for Vulkan 1.2; temporary Linkage declarations are removed.
AS provenance must run after linking, because IDs and instruction offsets change.

Three independently reproduced composition defects were fixed before this
checkpoint: the helper's BuiltIn WorkgroupSize could override application local
size, a persistent OOM could escape while constructing a diagnostic, and helper
specialization IDs could collide with application IDs. The helper's workgroup
decoration is removed, diagnostics are best-effort without throwing, and any
helper SpecId is rejected. Application workgroup and specialization declarations
remain intact. The review retains an actual old-output GPU failure (one invocation)
and corrected output (eight invocations).

## Semantic evidence and limits

- Composition: 194 checks, including source memory/addressing models, compute and
  fragment entry points, SPIR-V version upgrade, resource rejection, workgroup
  isolation, specialization isolation and seven sustained OOM budgets.
- Actual GPU workgroup check: production library injection retains the original
  4×2×1 group; eight invocations produce a local-index sum of 28. VVL/sync is clean.
- Metadata host reference: 1,643 checks, including malformed bounds, 64-bit
  arithmetic, clone/input lifetime and opaque native identities.
- Metadata GPU producer: 35 assertions over seven geometries/9,264 bytes and
  eight invalid-input cases pass. Replaying the same producer CB after source
  active size changes from 2608 to 1104 bytes matches the independent reference.
  Error cases cover 64-bit base carry, null-micromap ordinary indices, invalid
  width/bounds/offsets and status aliasing with jobs, indices and snapshots.
- Packed classifier: 4,506 actual native traces pass with VVL and sync enabled.
  GPU-built 2/4-state canonical data feeds the GPU metadata producer; all source
  canonical/index/job buffers are destroyed before traversal. The application
  shader contains ordinary native Proceed operations, Private query arrays and
  dynamic indexing; composition and automatic filtering supply the behavior.

The native matrix covers all 16 level-2 centroids, four special-only null states,
ordinary triangles/AABBs, logical ray/instance flags, original getters and
first-hit behavior. The CPU oracle compares results; it does not feed decisions
to the shader. Other levels retain the separate exhaustive/sampled shared lookup
evidence, rather than being claimed as this native matrix's coverage.

The fixture explicitly constructs the relationship between the native AS and its
RootCells. It does not establish automatic AS operand provenance, descriptor
transport at full application resource budgets, GPU TLAS normalization, public
AS interception or full lifecycle/synchronization. These are the next integration
steps. The default native pass-through path and public device operations remain
as described in [the preceding loader report](omm_layer_api_2026-10-27.md).

Real CTS results must be distinguished from these component tests. The pinned
external CTS currently has eight native control passes and two native query-pool
failures. Software probes are still rejected at the incomplete consumer device
gate, so there are zero software CTS passes. The [consumer bridge audit](omm_cts_consumer_bridge_2026-10-27.md)
pins exact source paths and the remaining end-to-end chain. Its RTP-namespaced
query probe does not exercise RTP, and its RQ `copy.compact` group actually calls
CLONE; neither case name substitutes for command/consumer coverage.

The integrated standalone run passes all 20 non-loader CTests (10 offline and
10 GPU), with no failures or skips. Public API software tests pass all 54 checks
with VVL both above and below the layer. Parent VEL builds the new targets and
passes ten offline tests; a Vulkan-free host-only build passes all three tests.

Raw component evidence is under `build-omm-compose`, `build-omm-classifier` and
`build-omm-metadata`. The [evidence manifest](omm_metadata_classifier_2026-10-27.json)
pins source, binaries and raw verification results. This checkpoint does not
change discovery or the final acceptance contract.
