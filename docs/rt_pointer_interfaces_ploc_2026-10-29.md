<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Physical RT interfaces and Mesa PLOC, October 29

This checkpoint starts at `a844662` on `codex/ray-tracing-fallback`.
The complete mandatory AS/RQ/RTP goal and M0-M5 remain open. The changes extend
the production typed scheduler and shared GPU builder; they do not introduce
another runtime, AS representation or source directory hierarchy.

## Physical pointers across shader stages

The preceding [Mesa pipeline comparison](rt_mesa_pipeline_2026-10-29.md)
established source physical address capture and eight-byte continuation codecs.
The remaining interface gate rejected `OpTypePointer` inside payload, callable
data and procedural hit attributes before a public pipeline could be created.

`typed_runtime.cpp` now interns complete type graphs across source modules.
Pairwise structural equivalence closes recursive physical-pointer pointees
without depending on nominal IDs, declaration order, names or graph sharing.
A physical pointer remains distinct from a same-width integer and includes its
storage class and pointee type in matching. Recursive pointees do not expand
frame storage: a physical pointer carries the actual Vulkan uint64 address.
The scheduler consumes the same explicit `TypedRuntimeInterface` plan that
component tests inspect. Existing typed codecs and interface copies preserve
the address bytes; no address is converted to an application descriptor token.

This follows the source-value/frame separation in Mesa's shader-call lowering
and RT CPS implementation, using the same pinned Mesa revision
`837ab0e30740abb9ebb60bcd40a5114c68c673d9` as the preceding comparison. This layer
continues to use typed SPIR-V continuations and its own public Vulkan scheduler;
it does not depend on NIR or a driver-private shader-call ABI. Vulkan's
[RT interface matching rules](https://docs.vulkan.org/spec/latest/chapters/interfaces.html#interfaces-raypipeline)
are applied to valid shader interfaces. Native pointee layout decorations remain
with each source memory access; this change adds no separate blanket layout
decoration equality requirement.

The public `test_rtp_api --physical-interfaces` fixture uses genuine Vulkan 1.1 /
SPIR-V 1.4 application shaders in all six RT stages, an ordinary AS descriptor,
one nonopaque procedural BLAS/TLAS, four SBT groups and public dynamic stack
sizing. A 72-byte payload/callable aggregate and 24-byte hit attribute contain
nested physical-pointer arrays. Two reports exercise accepted and ignored
any-hit results, committed-attribute snapshots, and subsequent attribute
mutation. Miss and closest-hit shaders execute two nested callable activations.
The caller retains a saved pointer while the child changes pointer fields.

Both optimized and DISABLE_OPTIMIZATION pipelines execute 27 rays for three
seeds; the last phase aliases both input references to the same address. The
independent host oracle compares every A/B data word, address word, input record,
unused output slot and trailing sentinel. Ordinary BDA writes occupy distinct
stage/report slots; the two aliased read/modify/writes in the inner callable
belong to the same invocation. Public RT-to-host dependencies and fences precede
readback. IS explicitly rejects the designated miss rays, because a conservative
[AABB candidate](https://docs.vulkan.org/spec/latest/chapters/raytraversal.html#ray-intersection-candidate-determination)
need not intersect the application's AABB. Exact report invocation multiplicity
is qualified on the tested software traversal; this is not a general promise
that every native implementation invokes an intersection shader once.

CPU tests cover recursive Node graphs, nominal sharing differences, all RT
interface storage classes, different pointees and pointer-versus-uint64 identity.
Two safe CPU mutants fail the new checks. SDK357 validates all six source modules
and the generated optimized/unoptimized runtime modules. The earlier 42-command
component freeze predates the final IS miss guard and is retained with its
original hashes; final source validation and execution are recorded separately.

## Production Mesa PLOC

The [PLOC adaptation record](rt_mesa_ploc_2026-10-29.md) pins the host and shader
references, scratch contract, local scan and uint32 task-bound derivation.
The public builder now creates a separate PLOC pipeline on eligible devices,
selects it using Mesa's leaf-count/build-flag policy, and feeds its actual indexed
IR into the existing GPU BVH4 encoder. Requested counts at most four, FAST_BUILD
or ALLOW_UPDATE BLAS, ineligible devices and excessive task bounds retain LBVH.
TLAS follows the pinned policy; UPDATE remains the existing refit operation.

The correctness baseline dispatches one persistent workgroup, which consumes
multiple logical tasks and phases. It requires neither cross-workgroup forward
progress nor a relationship between local invocation and subgroup lane order.
Local compaction uses a workgroup shared-memory scan. Device eligibility requires
1024 invocations, 40 KiB shared memory and the existing baseline subgroup support;
the overall fallback's 128-invocation minimum is unchanged.

The GPU test reads a shader-written producer ID from transient scratch in
addition to checking full BVH4 structure, parent links, bounds and flags,
plus independent sampled geometry/ID/ray oracles. It covers capacity 9 with
0/1/2 active leaves,
257 requested/220 active leaves, 1025 leaves across logical tasks, and a five-slot
TLAS with four active instances. Legacy and synchronization2 submissions, repeated
scratch reinitialization and procedural instance pointers are exercised.
CPU stream tests cover backend policy, eligibility, sort parity, scratch aliases,
larger counts and pre-command rejection at the task-counter boundary. Giant
allocations are not claimed from those CPU checks.

## Validation and retained failures

Final evidence is bound in
[`rt_pointer_interfaces_ploc_2026-10-29.json`](rt_pointer_interfaces_ploc_2026-10-29.json).
Raw logs remain in `build-pointer-interfaces-2026-10-29` and the two
`build-rt-cts-evidence/physical-ploc-{native,qualified}-2026-10-29` directories.

| Check | Result and scope |
| --- | --- |
| Standalone and parent RT target builds | PASS, Windows x64 / Clang / SDK 1.4.357.0 |
| Final offline checks | 49/49 PASS |
| Final public GPU runs | 6/6 PASS: five guarded software observations and one native small-tree control |
| Separate controls | Two previous-DLL expected negatives and one successful native full geometry-builder control |
| External software CTS | 16/16 PASS, no skips/crashes/timeouts or validation markers |
| Matching native CTS | 16/16 PASS, no skips/crashes/timeouts or validation markers |

The public software runs use masked native RT discovery, the downstream
no-native-RT guard, synchronization validation, and both validation-layer orders.
The final CTS additionally injects zero timestamp support: 16,116 checked calls,
zero rejected native-RT boundaries or timestamp writes, and four Event signals
and status observations. The frozen library SHA-256 is
`102f68324b94df3e7a788a8610df6c96cfdeaa6c1706830fe9e65f87e5aaa34f`.
The physical device is an RTX 3080, driver 596.49, Vulkan 1.4.329. This is
injected-profile qualification, not a physical no-RT-device result.

The CTS executable comes from `D:/Codes/VK-GL-CTS`, declared revision
`d18e4c68543103371715845cd914c71e5859963e`, SHA-256
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
Its exact selection is stored in both CTS summaries: eight interface/spill/callable
regressions, three complex-geometry cases (1280/2048 triangles, including the
FAST_BUILD LBVH policy), two 32-instance TLAS flag/AABB cases, UPDATE, compaction
and serialization. Source leaf counts and host selection policy establish which
backend these CTS inputs select; the separate GPU fixture directly reads the
producer marker. CTS does not directly cover pointer-bearing RT interface types;
the six-stage public fixture supplies that evidence. These results establish
their stated slice, not complete mandatory coverage or conformance.

The previous `a844662` library rejects the new interface fixture with
`unsupported invocation interface type`; its otherwise working LBVH builder
fails the explicit PLOC producer/phase assertions. Neither expected negative
is counted as a successful new implementation run.

The native six-stage physical-interface control remains unqualified. Unoptimized
and source-equivalent spirv-opt optimized SPIR-V both pass SDK validation, but
the native RTX 3080 / 596.49 driver raises `0xc0000005` inside
`vkCreateRayTracingPipelinesKHR`, before SBT or stack execution. It reproduces
without the software layer or validation. CDB records a null-address read at
`nvoglv64+0x5ccda` and the creation stack, with no software DLL loaded. Independent
review found no concrete host/API/type defect explaining the exception; its
root cause is unresolved. Debugger exit zero after `q` is not a test pass.
Keep all crash logs distinct from successful software and native builder runs.

SDK357's direct glslc `-O` invocation for Vulkan 1.1 / SPIR-V 1.4 also rejects its
own binary using a SPIR-V 1.3 optimizer environment. The diagnostic alternative
compiles unchanged GLSL first and invokes `spirv-opt -O` with the correct
`vulkan1.1spv1.4` target; all retained physical interface types survive. This
toolchain failure is not counted as application or fallback validation failure.
The optimized diagnostic freeze predates the final IS miss guard; the final
unoptimized CDB run uses the final six source binaries and records their hashes.

## Remaining work

HPLOC is still unwired; PLOC performance and cross-vendor qualification remain
open. The single-workgroup path establishes correctness, not throughput parity
with Mesa. Source VulkanMemoryModel/ShaderCall semantics, remaining memory and
execution modes, full public/private resource delivery, native logical-stage and
query-statistic semantics, group/protected execution and physical no-RT/POSIX
qualification remain required by the complete goal. The newer optional extension
exclusions and representative-CTS policy are unchanged.

The next builder step is the pinned HPLOC host contract: initialize its per-leaf
range buffer to `0xffffffff`, supply the 40-byte push ABI and checked XYZ
dispatch, then retain its range-arrival hierarchy and local clustering. Remove
the unused debug include and fix the validator-rejected uint64 `FindUMsb` through
32-bit halves. A public Vulkan adaptation must also guard negative shared-array
indices, define shared-memory phase barriers, and prove release/acquire range
publication across workgroups. A fixed 32-invocation workgroup implementation of
the collectives can avoid new general subgroup-size-control prerequisites;
uniform barrier participation is required. This is an implementation plan,
not HPLOC code or execution evidence. End-to-end 64-bit Morton-key layouts and
selection also remain open separately from the current 32-bit-key builder.
