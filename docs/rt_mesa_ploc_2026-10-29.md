<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Mesa PLOC production adapter, 2026-10-29

This change starts at `a844662` and adds an eligible PLOC path to the public
software AS builder. The output remains the same transient indexed Mesa IR and
the same production IR-to-BVH4 collapse. Runtime BVH4 ABI 2, original geometry
and primitive IDs, leaf flags, BLAS address resolution, and UPDATE refit are
unchanged. This document records the source/CPU/SPIR-V adaptation; the
[integration checkpoint](rt_pointer_interfaces_ploc_2026-10-29.md) records
the public GPU and external CTS observations separately.

## Fixed upstream contract

The reference is Mesa commit
`837ab0e30740abb9ebb60bcd40a5114c68c673d9`, not the moving Mesa main branch:

| File under `src/vulkan/runtime/` | SHA-256 of fetched reference |
| --- | --- |
| [`vk_acceleration_structure.c`](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/vulkan/runtime/vk_acceleration_structure.c) | `38879996a2722f8c8cd383dc22ff82cd579062f74c67da2a8c2072a5705970d7` |
| [`bvh/ploc_internal.comp`](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/vulkan/runtime/bvh/ploc_internal.comp) | `719ff93803b185ad9e4981e58b4a93287d42e369953025b65b8702d234c2a859` |
| [`bvh/hploc_internal.comp`](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/vulkan/runtime/bvh/hploc_internal.comp) | `b793f5afd05b3fb78fa5ade3027c240b27d61c5c3dc25782dcc74c9646798aa3` |
| [`bvh/vk_bvh_helpers.h`](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/vulkan/runtime/bvh/vk_bvh_helpers.h) | `a9c0081319d6179590777a718bf316a4a1582c6462d46331271ec33cc4889a66` |
| [`bvh/vk_bvh_defines.h`](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/vulkan/runtime/bvh/vk_bvh_defines.h) | `6da4a737b954eaf612fa7ff8e86c616d1f4e719605ec21a32d487980acdcf587` |

The reference host chooses LBVH at at most four requested leaves. It otherwise
chooses PLOC for TLAS, and for BLAS without PREFER_FAST_BUILD or ALLOW_UPDATE.
PLOC receives the final Morton-sort buffer and the opposite ping-pong buffer,
one eight-byte prefix partition per 1024 requested leaves, and an initially zero
internal-node counter. `task_counts[0]`, `task_counts[1]` and
`current_phase_end_counter` initially contain `0xffffffff`; all other task and
phase state initially contains zero. The kernel initializes live task counts
after the leaf stage has removed inactive inputs. It handles zero through two
active leaves independently of the general phase loop.

The initial production code compiled PLOC but created/dispatched only LBVH
pipelines. Its common INIT did not establish these three sentinels. A constant
64-lane specialization also could not be reused for the original PLOC subgroup
prefix arrays on a device whose native subgroup size is 32.

## Public Vulkan adaptation

`bvh_ir_record_build_backend` shares INIT, leaf generation, Morton sorting,
layout planning, and the indexed IR with the existing LBVH wrapper. It then
emits either LBVH TREE/BOUNDS or one PLOC stage. Production maps that stage to
the additional pipeline, then runs the existing BVH4 encoder/collapse. There
is no second runtime AS representation or CPU-generated construction topology.

PLOC is optional per device: 1024 workgroup invocations and X size, at least
40 KiB shared memory, the baseline compute BASIC/BALLOT subgroup support, and
a legal native power-of-two subgroup size from 1 through 128. Other devices
retain LBVH. The private feature/profile minimum remains 128 workgroup
invocations; PLOC does not raise it. The native subgroup size is specialized
separately from LBVH, although the adapted PLOC scan itself is subgroup
independent and emits no GroupNonUniform SPIR-V capability.

Two adaptations remove assumptions available to an internal driver but not
guaranteed by public Vulkan:

1. Exactly one persistent workgroup consumes all logical tasks through Mesa's
   phase/task scheduler. This makes progress independent of scheduling another
   workgroup. It is a correctness baseline, not a multi-compute-unit performance
   implementation. It can still process more than 1024 leaves and partitions;
   the dispatched workgroup count does not limit logical task count.
2. Local compaction follows `gl_LocalInvocationIndex`, using a shared
   Hillis-Steele prefix scan. It reuses the completed merge phase's
   `nearest_neighbour_indices` array. The merge aggregate counts surviving
   logical nodes directly. Mesa's nearest-neighbour search/merges, global
   partition aggregation, ping-pong layout and phase/task state machine remain.
   No relationship between subgroup lane ordering and local invocation IDs is
   assumed, and no full-subgroup feature is required. This follows the public
   [subgroup ID contract](https://docs.vulkan.org/spec/latest/chapters/interfaces.html#interfaces-builtin-variables-sg-local-invocation-id).

There are two 1088-element shared arrays (AABBs and uint neighbours) and four
shared uints. Even allowing 32 bytes for each AABB because of vec3 padding gives
`1088*(32+4)+4*4 = 39184` bytes, below the 40960-byte qualification bound.
This bound is specific to PLOC and is not imposed on LBVH devices.

PLOC buffer-reference accesses are `coherent` in this shader only. The generated
VulkanMemoryModel module has NonPrivatePointer and availability/visibility
operands on those accesses; device-scoped barriers can therefore publish IR,
partition and scheduler state between invocations. The existing host dispatch
helper supplies a compute-to-compute barrier after every stage. Its checked XYZ
packing remains in use for shared leaf/sort stages; PLOC's single native group
needs no unbounded grid or unchecked dimension.

The original uint32 task counters remain bounded explicitly. In the pessimistic
case where each merge iteration removes only one leaf, a conservative bound for
both phases, including per-iteration padding, is
`N*(N+1)/1024 + 2*N`. Counts whose bound reaches `0xffffffff` select LBVH.
An explicitly requested PLOC backend with such a count fails before emitting
any command. All products and scratch offsets are calculated in uint64.

## Scratch and producer ID

PLOC prefix partitions alias `bvh_ir_build_layout.node_info` after sorting. The
existing LBVH region allocates 16 bytes per requested leaf, covering eight bytes
per 1024 PLOC leaves, including the empty planner's allocation. Sort counts are
in a different region. Thus size query and recording use the same full layout
without growing or overlapping scratch. Both ping-pong buffers already have
eight bytes per leaf; PLOC uses key/id pairs on its first read and uint IDs in
subsequent iterations, as in the pinned contract.

The adapter names Mesa's `driver_internal[5]` at header byte 104 `producer_id`:
common INIT writes zero, and an actual PLOC shader invocation writes
`BVH_IR_PRODUCER_PLOC = 0x504c4f43`. This is diagnostic provenance in a
driver-owned part of the transient Mesa header. It is not a build input flag or
a field in runtime BVH4 or serialization. Existing CPU IR adaptation and GPU
BVH4 validation do not require this word to be zero; envelope build flags at
byte 124 remain zero and retain their existing validation.

For a public GPU test, copy `scratch + layout.source + 104` after the build and
require the PLOC producer ID. For a scene with more than two active leaves,
`sync_data.phase_index` at byte 68 ends at `0xffffffff`; task-started/done counts
at bytes 52/56 and phase-end at byte 64 also expose completed logical work.
These observations distinguish actual PLOC execution from a successful LBVH
fallback; original geometry, hit IDs/bounds and guards still need an independent
oracle.

## Verification and remaining scope

Executed without Ninja, Vulkan device execution, or CTS:

- Extended `tests/test_ir_builder_contract.c`: eligibility and rejected limits,
  flags/size policy, empty/small/1025/65537/1048576 command streams, sorted-buffer
  parity, shared scratch alias, PLOC stage without LBVH stages, and rejection of
  excessive task counts before the first emit. Compiled with Clang C11 and ran:
  PASS.
- Modified `src/vkrt_build.c`: strict Clang C11 syntax check against generated
  shader headers: PASS.
- Modified INIT and PLOC: glslc Vulkan 1.1/SPIR-V 1.4 compilation and spirv-val
  with scalar layout: PASS. Disassembly confirms VulkanMemoryModel,
  DeviceScope, non-private coherent accesses, and no subgroup capability in the
  final PLOC module.

Scratch sizing and CPU command capture alone do not prove GPU geometry
correctness. The integration checkpoint separately executes an eligible GPU
with more than 1024 actual leaves, poisoned scratch, the original-geometry
oracle and producer ID readback. It includes capacity9/active0,1,2 shortcuts,
257/1025-leaf and TLAS builds, CPU ineligible/policy checks, and actual CTS
FAST_BUILD/UPDATE paths. The exact final binaries and results are bound there;
the standalone source/CPU checks above do not acquire that broader scope.

HPLOC is still unintegrated. The pinned host requires a separate uint range
buffer initialized to `0xffffffff`, one subgroup per workgroup, a subgroup size
at most 64, and its own pipeline/dispatch contract. The imported local shader
also retains an unresolved `vk_debug.h` include and a 64-bit findMSB adaptation
gap. Compiling or dispatching it without resolving those contracts is not part
of this change. M1 and the full software RT goal remain incomplete.
