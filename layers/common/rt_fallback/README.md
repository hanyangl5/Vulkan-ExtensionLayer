<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Portable BVH4 construction components

This directory defines an experimental layer-owned BVH4 wire ABI, CPU reference
encoder/layout planner, Mesa scalar-IR adapter, and component GPU hierarchy
encoder. Neither layer's production builder/traversal/compiler consumes the
complete shared representation yet. These components make no extension-support
claim.

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
implemented traverser. Serialization, refit/compaction, shared traversal, and
production builder/consumer integration remain separate work.

## Mesa construction IR adapter

`mesa_ir_adapter.h` accepts separate header/node byte spans or the existing
[VKRT v2 envelope](../../../vkrt_layer/IR_CONTRACT_V2.md). Its scalar record
contract is the locally imported Mesa `d1a2854aec69179f2b80d1c7368771041181cda5`
schema; it does not yet qualify every builder variant in the newer Mesa reference
selected by the implementation goal. It reads bytes without alignment-dependent
casts, validates the graph iteratively, and removes empty/singleton wrapper boxes.
Unreferenced inactive slots are never read. Output contains a compact `BuildNode`
tree plus triangle/AABB/instance records retaining original IDs and flags.

The full per-geometry flags table is required because Mesa's packed leaf word
contains opacity but omits NO_DUPLICATE_ANY_HIT. Active instances require an
explicit BLAS address resolver; old IR addresses must never accidentally become
shared-runtime addresses. Affine transforms and their checked inverses are
preserved. Unsupported BUILD variants, invalid ranges/graphs/counts/bounds and
unrepresentable transforms fail explicitly. The default input requires finite
active bounds; this is not complete Vulkan exceptional-input coverage.

Mesa construction IDs still have a 32-bit byte-offset ceiling. That is a remaining
builder limitation, not a restriction of the 64-bit runtime layout planner.

## GPU hierarchy encoder

`bvh4_encode.comp` consumes a validated compact `BuildNode` array through BDA.
Phase zero assigns dense internal IDs with 32-bit atomics and initializes parent
links. A compute write-to-read/write barrier precedes phase one, which writes
child bounds/IDs, root bounds and unique parent links. The host contract and push
constant layout are in `bvh4_gpu.h`. All region offsets and index/stride arithmetic
are 64-bit, with no storage-buffer descriptor range requirement or internal set.
AS addresses are aligned to 128 bytes; scalar word/input accesses promise only
4-byte alignment. This follows the
[BDA alignment contract](https://docs.vulkan.org/guide/latest/buffer_device_address_alignment.html).
There is no inter-workgroup polling or assumed invocation ordering.

This first GPU component retains binary arity in the BVH4 node format; it does
not yet perform the reference encoder's four-child collapse. It populates
hierarchy/header/parents only, leaving leaf records for a separate producer.
CPU conversion of Mesa IR followed by GPU encoding is a component test, not the
final GPU-only construction path. Direct GPU Mesa adaptation, record emission,
encode/refit integration and both consumers remain open.

Build and run independently:

```sh
cmake -S layers/common/rt_fallback -B build-bvh4
cmake --build build-bvh4
ctest --test-dir build-bvh4 --output-on-failure

# Optional Vulkan/glslc/spirv-val component test. The device enables only
# Vulkan 1.2 compute, BDA, scalar layout and shaderInt64; no RT extensions.
cmake -S layers/common/rt_fallback -B build-bvh4 -DVEL_RT_BVH4_GPU_TESTS=ON
cmake --build build-bvh4
ctest --test-dir build-bvh4 -L no_native_rt --output-on-failure
```

For GCC/Clang sanitizer coverage, configure with
`-DVEL_RT_BVH4_SANITIZERS=ON`. Tests independently check leaf coverage, subtree
containment and parent links over random and deeply unbalanced trees; test
rejection paths; and plan allocations larger than 4 GiB and at mandatory
primitive-count minima without allocating those AS buffers.

Adapter tests add sparse leaf slots, multiple geometries/full flags, original
IDs, TLAS relocation/inverse transforms, a 4096-leaf deep tree, and malformed
record/graph/count checks. When Vulkan headers are found, they also check the
actual imported Mesa C declarations against the parser's byte offsets. CPU tests
remain buildable without Vulkan headers.

GPU tests independently verify coverage, conservative finite bounds, parent
links, header offsets/counts and allocation guards over empty/singleton inputs,
28 shuffled random trees, an 8192-leaf deep tree, forced dispatch chunks and an
adapted sparse Mesa fixture. Set `VEL_RT_BVH4_VALIDATION=1` to request the Khronos
validation layer. A device without the required compute features returns CTest
skip code 77; qualification must distinguish a skip from an executed pass.
