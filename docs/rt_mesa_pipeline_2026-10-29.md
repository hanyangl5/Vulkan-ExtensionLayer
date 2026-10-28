<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Mesa pipeline comparison and mandatory boundary fixes

This checkpoint continues `fe6ea2ab88280b12c4f42329c1c497d83c1a7356` on
`codex/ray-tracing-fallback`. Work began on October 28 and completed on October
29, 2026, Hong Kong time. The complete fallback and M0-M5 remain unfinished.
The companion [evidence manifest](rt_mesa_pipeline_2026-10-29.json) binds source,
builds, binaries, commands, original observations and final results.

## Mesa comparison

The user requested Mesa RTP implementation references. The comparison pins
Mesa commit `837ab0e30740abb9ebb60bcd40a5114c68c673d9`, the existing goal's Mesa
baseline. The earlier builder IR-contract pin remains distinct. Seven reference
files, their exact URLs, byte sizes and SHA-256 values are recorded in the
manifest. The downloaded, unmodified MIT-licensed reference files retain their
notices under `build-mesa-rtp-2026-10-28/reference`; they are not build dependencies.

| Fixed upstream reference | Applicable design observation | Owned implementation |
| --- | --- | --- |
| [NIR shader-call lowering](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/compiler/nir/nir_lower_shader_calls.c) | `can_remat_instr` distinguishes immutable rematerializable values from live state that must survive a shader call; record/argument pointers require particular care | Typed CFG liveness, explicit frame codecs and definition-time address capture |
| [RADV CPS stage lowering](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/amd/vulkan/nir/radv_nir_rt_stage_cps.c) | `lower_rt_derefs_cps` and `radv_nir_lower_rt_io_cps` separate call-data addressing from shader-local storage | Layer-owned call frames, invocation data and scheduler ABI |
| [RADV common stage lowering](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/amd/vulkan/nir/radv_nir_rt_stage_common.c) and [stage functions](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/amd/vulkan/nir/radv_nir_rt_stage_functions.c) | RT storage and instruction lowering have explicit stage/call boundaries | Existing typed stage compiler and linked compute scheduler; no AMD hardware CPS ABI is imported |
| [RADV software intersection](https://github.com/chaotic-cx/mesa-mirror/blob/837ab0e30740abb9ebb60bcd40a5114c68c673d9/src/amd/vulkan/nir/radv_nir_rt_common.c) | Software box/triangle arithmetic has local floating-point invariants | Eight external AABB/procedural CTS diagnostics passed on the old checkpoint; these did not establish a new numerical defect |

The route remains a typed SPIR-V transformation and the shared Mesa-build/BVH4
backend. Referencing Mesa does not make NIR-to-SPIR-V a ready-made backend or
prove the complete continuation contract. This change adds owned code; it does
not copy Mesa's driver runtime or binary AS format.

## Source physical pointers

The old compiler rejected every `PhysicalStorageBuffer64` source even though
the execution frames already used BDA. It now accepts that addressing model
with GLSL450 and preserves physical pointers as actual 64-bit addresses.
Logical pointer identities retain their existing provenance representation.

Ordinary physical access-chain addresses are captured where defined. Phi/Select and live values
use numeric identities across suspension. Eight-byte codecs also cover pointers
inside Function/Private structs and arrays. Recursive typed bridges convert
resource pointer fields at native load/store boundaries, rather than attempting
to convert pointer fields using `OpCopyLogical`. Native pointee layouts,
alignment/memory operands and applicable alias decorations remain explicit;
recursive physical pointee types use forward declarations.

Independent review found that physical pointers to matrices and their restricted
access paths cannot be round-tripped through integers. These paths instead
capture their struct-root address and selectors at definition, then rebuild the
original access chain immediately before its memory use. Structural checks
cover this rule because the SDK validator accepted the earlier illegal form.
The review also examined alias annotations for direct pointer arrays. Current
unified SPIR-V revision history #827 removes the older extension document's
mandatory annotation requirement; no current validation failure or GPU value
error is claimed for missing annotations. The bridge now emits conservative
alias annotations and tests them explicitly.

The new public raygen/callable shaders perform two nested Callable activations.
The leaf swaps source resource pointers, then mutates their storage. The caller
must retain its old pointer, struct, array and Private identities while a fresh
load sees the swapped pointers. An independent CPU oracle checks every word of
both data buffers, records and output, including untouched words and tail
padding. Execution covers 27 launch IDs, two seeds, enabled/disabled pipeline
optimization, and the exact public raygen-plus-two-callable stack equation.
The same fixture passes natively. The frozen old DLL rejects its source.
The final fixture additionally loads/stores direct physical-pointer arrays and
checks their retained identities by modifying distinct data words after return.

VulkanMemoryModel, physical pointers in cross-stage payload/callable type
metadata, conflicting per-stage FP modes and remaining memory variants are
still open mandatory compiler work. This is not complete M3.

## Allocation-count admission

Every public and private native `VkDeviceMemory` allocation on a software RT
device now shares a live-plus-in-flight transaction ledger. Initialization
reads the real native `maxMemoryAllocationCount` before the first private pool.
The public limit and heap properties are unchanged; there is no fixed private
quota. Full native count returns `VK_ERROR_OUT_OF_DEVICE_MEMORY` before an
invalid native call. Existing pool slices can still reuse free ranges at that
boundary.

Host allocation callbacks and native Allocate/Free execute outside the ledger
lock. Metadata/native failures roll back reservations. Free retains its slot
until the native call returns and removes by record identity, preserving a new
object that reuses the numeric handle. Device and per-call allocator ownership
are preserved. Device cleanup drains private pools before dropping the ledger.

The explicit test boundary injects a native limit of 4096. The native control
successfully holds 4096 public allocations. The old software DLL attempts an
additional native allocation because private storage is missing from its count;
the guard stops it with exit 86. The new software DLL returns OOM without the
invalid call, successfully frees/reallocates a slot, and creates another AS
using existing pooled storage. This proves count admission, not success for
every legal public allocation combination or complete heap-budget delivery.

## Independent native query scopes

The blanket active-native-query rejection is replaced by retained native pool
type/statistic metadata. Known occlusion and graphics-only statistic queries
can surround AS query work. Compute-invocation statistics, unknown counters,
types and handles remain conservative because hidden compute work could
otherwise contaminate native results. Both ordinary and indexed Begin/End and
parent Execute qualification use the classification; reset clears both counts.

The fixed [Vulkan 1.4.357 query chapter](https://github.com/KhronosGroup/Vulkan-Docs/blob/v1.4.357/chapters/queries.adoc)
permits Begin/End outside a render pass. The AS pool is separate and its own
queries are not active. Four public cases isolate Reset, Property, Copy and
their combination inside an unrelated occlusion query, each executing twice.
No drawing means occlusion must be zero. AS copied value/availability/padding
is compared against an independent completed host result. Native controls pass;
the old software DLL rejects all four recordings. This does not implement
compute-statistic virtualization for all AS/RTP internal dispatches.

## Evidence and remaining work

Raw public observations are under `build-mesa-rtp-2026-10-28/gpu-before`,
`gpu-integrated` and `gpu-final`. Each observation retains executable/shader/DLL
hashes, exact environment, log and guard audit. Earlier observations remain
separate from the final binary. Independent CPU/SPIR-V records are retained in
the companion manifest. External CTS uses the user's `D:/Codes/VK-GL-CTS`
checkout at `d18e4c68543103371715845cd914c71e5859963e`, with masked native RT
discovery, downstream native-RT rejection and explicit timestamp-zero injection.
The CTS result is representative evidence, not formal conformance.

Final layer SHA-256:
`e00127c5efc03924497ea70c666a1dd01afbddd87fe5f08f08f4033828a6af78`.

| Final observation | Result and scope |
| --- | --- |
| Standalone and parent layer builds | Both pass; the parent check builds the canonical RT target |
| Offline checks | 49/49 pass, including generated replay, compiler, resource and guard checks |
| Public GPU checks | 9/9 pass: eight software observations with validation above/below the layer and one native control of the final pointer-array fixture; every software observation has a completed downstream guard audit |
| Frozen previous-DLL/native observations | 6/6 behave as expected: three native controls, old physical-source rejection, old native-count overflow stopped by the guard, and all four old occlusion-scope recordings rejected |
| External AS/RQ/RTP CTS | 24/24 pass on the final DLL; zero unsupported/crashed/timed-out/missing cases, zero validation error markers |
| CTS boundary | 32,875 checked calls, zero native-RT rejections or timestamp writes, 20 Event signals and 20 status observations; discovery mask and timestamp-zero profile enabled |

The final direct-array fixture and matrix recipe changes postdate the earlier
`gpu-final`/`mesa-rtp-final` observations. Their final records are specifically
`gpu-qualified` and `mesa-rtp-qualified-2026-10-29`. The matrix fix has source,
stage and final-runtime validation plus structural and safe negative controls;
no separate physical-matrix GPU value test is claimed. Independent component
records retain 6 ledger CPU positives/3 negative controls, 8 query-scope CPU
positives/5 negative controls, and 30 physical-source review commands with
their expected outcomes. None are substituted for the public GPU/CTS results.

Next work includes pointer-bearing cross-stage interfaces, remaining native
query and logical-stage synchronization behavior, protected/device-group
qualification, complete public/private resource delivery, builder/count and
numerical qualification, and physical no-RT/cross-vendor/POSIX execution.
Newer optional exclusions remain documented; they do not excuse these remaining
baseline requirements. No development-branch merge or push is performed.
