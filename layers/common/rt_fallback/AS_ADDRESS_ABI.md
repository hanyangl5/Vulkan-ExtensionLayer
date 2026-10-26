<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Public AS values and physical BVH4 roots

The RQ layer returns opaque 256-byte-aligned AS values. These values are not
ordinary buffer device addresses and must be resolved before a shader reads
BVH4 bytes. The physical BVH4 header requires 4-byte alignment. Node-region
offsets retain the planner's preferred 128-byte relative alignment.

The Vulkan-Docs revision matching SDK 1.4.357 is
[`d184375dcc5da2b06ca375a8d7d1f9d21ca64a76`](https://github.com/KhronosGroup/Vulkan-Docs/blob/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/resources.adoc#L9740).
Its address contract requires 256-byte alignment and preserves creation-offset
differences for GENERIC AS objects in the same buffer. The implementation uses
one affine public range per AS-storage buffer, so same-offset aliases share a
value and different offsets preserve their byte difference. The normative text
does not require the absolute GENERIC value to equal buffer BDA plus offset.
The note permitting typed addresses to differ is informative; it cannot establish
the converse equality for GENERIC objects.

`vkrt_as_registry.c` assigns these ranges in the complete nonzero 56-bit key
space. Range size uses the original buffer size, checked without a 32-bit
ceiling. Only live AS offsets allocate sparse GPU pages; a giant buffer does
not allocate a dense shadow. Buffer ranges are released after buffer destruction
and its last AS reference. Same-offset aliases retain the leaf until their last
reference disappears. Empty paths and ranges are reclaimed and reused.

The 64-byte immutable resolver header has this little-endian scalar layout:

| Offset | Field | Required value |
| ---: | --- | --- |
| 0 | u32 magic | `0x52534156` |
| 4 | u32 version | 1 |
| 8 | u32 entry stride log2 | 3 |
| 12 | u32 radix bits | 8 |
| 16 | u32 levels | 7 |
| 20 | u32 public address shift | 8 |
| 24 | 2 reserved u32 | 0 |
| 32 | u64 root page BDA | Nonzero, 8-byte aligned |
| 40 | 3 reserved u64 | 0 |

Each page has 256 u64 slots. Public value `v` selects key `v >> 8`; seven bytes
select slots from most to least significant byte. The first six slots point to
pages; the final slot holds the physical BVH4 header BDA. Null, misaligned public
values, absent paths and unknown header metadata return zero. The resolver uses
explicit 64-bit byte arithmetic and 4-byte word loads. There is no physical-BDA
fallback or old runtime-format detection.

Private pages/header use persistently mapped HOST_VISIBLE | HOST_COHERENT
storage/device-address buffers, shared concurrently across all enabled queue
families when needed. Buffers include seven spare bytes so their logical data
can be aligned to eight inside the actual buffer, independently of buffer BDA
alignment. This compatible coherent memory type is a private prerequisite.
There is no application descriptor or push-constant slot for the resolver root:
RQ compilation embeds the device's full 64-bit root as an ordinary constant,
after converting a dedicated library specialization constant. Application
specialization IDs remain intact. The TLAS builder uses private specialization.

CreateAS allocates its complete path and its descriptor value cell while failure
can be returned. The cell is initialized once to the public value; noncoherent
cell memory is flushed at creation. After valid backing binding, Build/GetASAddress
publishes a physical address into the preallocated coherent leaf. A stable or
aliased GetASAddress performs no allocation, flush or mapped-memory write.
Existing live paths are immutable; different AS entries update disjoint slots.
First shader use follows the host writes and queue submission. Destruction must
follow completed AS uses; pruning never changes a path used by another live AS.

TLAS construction resolves each public BLAS reference and stores a physical
BLAS root in the existing BVH4 instance record. Traversal therefore does not
depend on retaining the BLAS handle or resolver entry after TLAS construction.
The [construction definition](https://github.com/KhronosGroup/Vulkan-Docs/blob/d184375dcc5da2b06ca375a8d7d1f9d21ca64a76/chapters/accelstructures.adoc#L21-L25)
requires Build/Copy with the given AS as destination; recreating a view over old
bytes does not construct it. Build, copy and deserialization destinations publish
their mapping before GPU use. BVH4 ABI 2
also retains the original public BLAS value at instance byte 120 for serialized
reference lists; the physical traversal root remains at byte 0.

This closes the observed legal 16-byte buffer-suballocation address gap, not
the complete resource ABI proof. Per-AS descriptor SSBOs and native allocation
budgets remain unresolved for full-limit applications. GPU copy/compaction and
serialization and semantic UPDATE are implemented; full property-query publication and
complete RTP qualification remain unfinished. Optional
capture/replay and device-address commands are not claimed by this path.
