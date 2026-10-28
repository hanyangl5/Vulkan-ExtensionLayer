<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Indexed construction IR v3

This is an internal build intermediate, consumed by the portable BVH4 encoder
and refit path. It is not the public AS, runtime BVH4 or serialization ABI.
The historical [v2 contract](IR_CONTRACT_V2.md) remains a supported decoder input.

The scalar Mesa header occupies bytes `[0,108)`. The metadata at offsets 108,
112, 116, 120 and 124 contains magic `0x58424856`, version `3`, leaf capacity,
geometry type and build flags respectively. Records begin at byte 128. Header
and record sizes remain unchanged: internal 40, triangle 68, AABB 32 and
instance 96 bytes. Construction metadata and child IDs have changed meaning;
unchanged record sizes do not make v2 and v3 interchangeable.

A valid child ID is `(record_index << 2) | kind`, with kinds triangle 0,
internal 1, instance 2 and AABB 3. `0xffffffff` remains invalid. Leaf indices
are relative to the leaf region; internal indices are relative to the internal
region. With `C` the input capacity, `S` the geometry's leaf stride and `i`
the decoded index, the node-relative byte offset is:

```
leaf:      uint64(i) * S
internal:  uint64(C) * S + uint64(i) * 40
address:   container_address + 128 + byte_offset
```

The capacity includes inactive holes. Active leaf count must not replace `C`
when locating internal records. v2 instead decodes its tagged byte offset with
`id & ~3`; encoder and CPU adapter select the decoder using explicit version
metadata. Unknown versions are rejected by their input validation.

Production admits at most `0x1fffffff` leaves. The checked size planners use
64-bit products, additions and alignment; native allocation failure remains a
real resource outcome. Removing the former 4-GiB construction-container guard
does not imply that every qualifying device can physically allocate the
largest possible AS at once.

The leaf push-constant ABI is 96 bytes, including its 64-bit internal-region
offset. Leaf, tree and bounds shaders flatten native XYZ workgroup coordinates
with 64-bit arithmetic before their logical range checks. Morton sorting claims
only its logical job count, independent of padded dispatch planes. The encoder
and refit passes retain their checked `first`-based dispatch chunks.

Validation includes host maximum-count/layout checks, v2/v3 adapter cases,
SPIR-V validation and GPU address-codec reads with synthetic offsets above
4 GiB. Artificially small dispatch limits exercise real production XYZ grids.
These are distinct from a physically allocated giant-AS or complete numerical
qualification; see the repository implementation status for current evidence.
