<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Portable BVH4 reference foundation

This directory defines an experimental layer-owned BVH4 wire ABI and a CPU
reference encoder/layout planner. It is not yet connected to either layer's GPU
builder, traversal, or shader compiler. It makes no extension-support claim.

Internal nodes contain four FP32 child bounds and tagged indices in 128 bytes.
The design is BOX32-inspired, but no AMD address packing or hardware node ABI is
used. Child indices have 30 bits; `0xffffffff` is invalid. The final index is
reserved uniformly for all kinds, leaving capacity above Vulkan's mandatory
`2^29-1` primitive minimum. Addresses are a 64-bit base plus a **64-bit** region
offset and index-times-stride calculation, not 32-bit byte offsets.

The C-compatible header describes packed scalar data. C and C++ tests check
sizes and key offsets. Shader consumers must use explicit word loads or scalar
layout; a default `std430` array of `vec3` would not match this ABI. AS base and
internal-node storage must be aligned to 128 bytes. The planner aligns regions
and rounds allocation size to 256 bytes.

The generic reference input is a compact binary tree with dense leaf indices for
each kind. It is independent of Mesa host/runtime types; a Mesa IR adapter is
still required. The encoder expands the largest-area internal frontier until
each output node has at most four children. It retains input leaf references and
conservative bounds, records internal/leaf parents, and rejects malformed graphs.
Empty and singleton roots need no internal nodes. Input bounds must be finite
and ordered; inactive/exceptional build inputs must be resolved by the eventual
builder adapter before this reference interface.

Parent storage is a prerequisite for a future traversal-overflow strategy, not an
implemented traverser. This reference does not populate geometry records or
serialize an AS image, implement refit/compaction, or access a GPU. Those remain
separate implementation work.

Build and run independently:

```sh
cmake -S layers/common/rt_fallback -B build-bvh4
cmake --build build-bvh4
ctest --test-dir build-bvh4 --output-on-failure
```

For GCC/Clang sanitizer coverage, configure with
`-DVEL_RT_BVH4_SANITIZERS=ON`. Tests independently check leaf coverage, subtree
containment and parent links over random and deeply unbalanced trees; test
rejection paths; and plan allocations larger than 4 GiB and at mandatory
primitive-count minima without allocating those AS buffers.
