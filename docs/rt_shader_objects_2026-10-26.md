<!-- Copyright (c) 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: MIT -->
# Shader-object source and binary transport, 2026-10-26

## Build grammar and optional headers

The source-envelope validator requires SPIRV-Tools with Vulkan 1.4 target
environment support. The supported grammar baseline is SPIRV-Tools
`v2025.1.rc1` or newer; its
[primary C header](https://github.com/KhronosGroup/SPIRV-Tools/blob/v2025.1.rc1/include/spirv-tools/libspirv.h)
defines `SPV_ENV_VULKAN_1_4`. Current
[Ubuntu noble security metadata](https://packages.ubuntu.com/source/noble/spirv-tools)
lists `2025.1~rc1-1~ubuntu0.24.04.2`, so a current apt update can supply that
enum despite the distribution's original 2024 release date. This is package
evidence, not a completed Linux build of this checkout. Older package snapshots
need an updated matching SPIRV-Tools header/library pair. Silently selecting
Vulkan 1.3 can reject otherwise compatible source that depends on newer grammar;
Universal 1.6 omits Vulkan-specific validation and is not an equivalent fallback.

Ubuntu noble's
[Vulkan development package](https://packages.ubuntu.com/libvulkan-dev)
remains `1.3.275.0-1build1`. The corresponding
[primary Vulkan header](https://github.com/KhronosGroup/Vulkan-Headers/blob/v1.3.275/include/vulkan/vulkan_core.h)
has ShaderObject types/PFNs but lacks `VkCustomResolveCreateInfoEXT`.
Custom-resolve declarations and encode/decode paths are therefore guarded by
`VK_EXT_custom_resolve`; an unsupported custom-resolve RQ reconstruction chain
fails before native creation and its envelope is incompatible. Proven NoRT groups
use the authoritative native-binary route, which does not reconstruct that chain.
The new-SDK path is unchanged. The focused host test compiles and passes both
with the current SDK and with this optional macro disabled; logs are
`build-resource-profile/shader-binary-optional-host.log` and
`shader-binary-no-custom-host.log`. This check concerns the shader-object module,
not all newer optional interfaces elsewhere in the unified layer. The same
v1.3.275 header also names result 1000482000 only
`VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT`; the module and its host/GPU fixtures
therefore use this original spelling universally. It is a same-valued alias of
the newer canonical `VK_INCOMPATIBLE_SHADER_BINARY_EXT` on current headers.
This changes no current-SDK runtime behavior and needs no guessed header-version
threshold. A symbol audit of the new module found no other unconditional Vulkan
1.4 API dependencies; whole-layer Linux qualification remains separate.

The implementation is in `layers/raytracing/src/vkrt_shader_objects.c` and
`vkrt_shader_interface.c`. Shader handles remain native. This is a component
qualification, not complete M0/M2/M4 support or Vulkan conformance; the resource
budget/property planner remains inactive.

## Current creation and transport paths

SOURCE-only calls retain native batching and the application's shader allocator.
Ordinary SOURCE-only calls on ordinary devices forward the original create-info
array. Virtual RT SOURCE calls use the common RQ lowerer and strip explicit RT
push-stage bits, omit RT-only ranges, and preserve `VK_SHADER_STAGE_ALL`. Borrowed
source, specialization data, arrays and chains are never modified.

Every exported stage has a device-owned envelope. An originally linked stage
stores the complete original linked SOURCE group, not merely its own module:
TESE, for example, can depend on execution modes supplied by TESC. Each group
member records source, entry name, specialization, flags and next-stage. Each
exported envelope identifies which member's shader it represents.

There are two deliberate import routes:

- When the complete original group has no AS/RQ/RT capability, extension,
  execution model or instruction dependency, its source provenance is accompanied
  by the target's authoritative native binary. Interface/pNext reconstruction
  is unnecessary. This preserves native behavior for unused optional layouts,
  samplers and creation metadata across logical feature profiles.
- When any original peer depends on virtual RT, every peer carries a full
  compilation-interface recipe. Imports rebuild destination-owned private
  layouts/samplers/conversions, recompile the complete original SOURCE group,
  rewrite RQ against the destination resolver, fetch the selected temporary
  shader's native binary, and retire every returned temporary shader reference.
  Temporary layouts/samplers are destroyed only afterward.

Both routes finish with native `codeType=BINARY`. The final create-info preserves
its caller's flags, next-stage, pNext, specialization pointer and descriptor/push
interface, apart from the existing virtual-profile RT push-stage translation.
The temporary SOURCE compilation instead restores the original entry,
specialization, flags, next-stage and supported semantic pNext. A call containing
both SOURCE and BINARY inputs converts all prepared final elements to BINARY,
so a final linked native batch cannot mix code types.

The final interface may legally be absent or different from the original SOURCE
interface. Converting such input directly to native SPIR-V induced VUIDs 10383
and 10384 in the previous implementation. Private original-interface compilation
followed by native BINARY creation removes that new SOURCE-only requirement.
See [`VkShaderCreateInfoEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/VkShaderCreateInfoEXT.html).

A bad binary at index k preserves a valid independent prefix before k. If
truncation breaks a linked group's logical next-stage topology (08409), the
prefix retreats to that group's first member; the failed group and tail remain
null. For example, `[COMP, VERT->GEOM, FRAG, badGEOM]` retains COMP without sending
the illegal `[VERT->GEOM, FRAG]` chain downstream. A lone VERT prefix remains
legal and is retained. Caller flags/nextStage are never rewritten to disguise
the failure. The first null identifies the failed element or linked group,
unless an earlier native error occurred; native partial results are retained.
See [`vkCreateShadersEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateShadersEXT.html).
If post-creation binary caching fails, its failed
slot and tail are destroyed with the same shader allocator; earlier complete
objects remain available. The outputs always contain null or valid handles.

## Envelope and interface identities

The final envelope format is **3**; its SHA-256-derived UUID domain changed from
intermediate FORMAT2 snapshots when the interface environment header changed.
Native `shaderBinaryVersion` ordering is retained. The UUID depends on native
UUID, envelope format and physical backend eligibility, not logical RT enablement.
Unknown raw native binaries are incompatible on every logical profile.

The envelope uses explicit little-endian fields: a 128-byte header with total
length, group count, selected target, optional native-binary length, identity and
checksum; then 16-byte-aligned stage records with 96-byte metadata followed by
SPIR-V, entry name, specialization map/data and any interface recipe. A proven
NoRT group's native payload follows the group. RQ groups store no native payload
containing the injected resolver address. Container fields and interface recipes
contain no Vulkan handles, host pointers or allocation callbacks.

The interface recipe is version **2**. Its 32-byte header stores source robustness
mask at byte24 and source push translation policy at byte28. Binding definitions,
immutable sampler creation data and YCbCr conversion definitions are copied while
original objects are alive. AS descriptor types and RT descriptor stages follow
the source layout's translation policy even on an ordinary destination device.
Push translation strips RT bits instead of mapping them to compute.

The importer checks arithmetic, lengths, padding, checksum, identities, entry
name/model, original source fields, unique linked stages and next-stage topology.
The real SPIRV-Tools validator checks source modules. RQ reconstruction also has
an explicit specialized-source validation API, operating on a temporary copy.
Its 44 real SDK checks include legal scalar/vector floating conversions and
specialized invalid workgroup sizes; the base and getter lowerer regressions also
pass. Logs are `build-source-layout/shader-format3-specialization-run.log` and
`shader-format3-lowerer-{base,getter}.log`. Residual undefined integer expressions
remain subject to the SDK validator's limits; this is not exhaustive malformed
specialization certification. Original stored source and specialization are
preserved for native compilation. Unused specialization IDs can have size zero;
each map offset remains strictly below dataSize and its extent stays in bounds.

Interface validation precedes materialization and rejects duplicate ordinary
bindings, overlapping push stage masks and invalid sampler semantic combinations.
YCbCr enum and format-component constraints use metadata generated from the
matching `vk.xml`, alongside the existing pNext metadata. Enabled construction
features and tracked scalar property limits are checked before
private native creation. There is no silent removal of interface flags. Source
stage/next-stage/creation-feature checks also run for every temporary peer.
Complete per-format physical feature eligibility is still a qualification gap;
this preflight is not exhaustive malformed-recipe certification.

Repeated public Get calls return invariant bytes for a live handle. Short output
buffers receive no bytes, size zero and `VK_INCOMPLETE`. The relevant guarantee is
[Binary Shader Compatibility](https://docs.vulkan.org/spec/latest/chapters/shaders.html#shaders-objects-binary-compatibility).

## Lifetime and profile boundaries

Canonical cache state, records and bytes use DEVICE scope with the original
DeviceCreate allocator, falling back to the original InstanceCreate allocator.
Zero-queue maintenance9 devices still retain that owner. The layer never retains
a shader's allocation callbacks in a shared canonical record. Native handles may
alias: the first canonical bytes remain stable until the last reference retires.
Native Create/publication and Destroy/retirement are serialized to avoid a reused
handle merging with an old cache record. Temporary native aliases are destroyed
directly once per returned reference and do not alter public cache refcounts.
See [Host Memory](https://docs.vulkan.org/spec/latest/chapters/memory.html#memory-host).

The RTP descriptor-layout tracker also counts repeated native handle references.
An intermediate Destroy no longer removes the definition needed by another live
reference; existing pipeline layouts retain immutable snapshots through handle
reuse. Public pipeline-layout aliases retain a stable canonical definition and
one DEVICE-owned token/private owner per native creation. Native public
Create/Destroy still receive the corresponding caller allocator; ordinary
retirement does not dereference an earlier alias's expired callback context.
Native creation/publication and destruction/retirement use the same mutex, and
private owners retire after unlocking. The host regression covers callback
context expiry, Device/Instance ownership, allocation failures and gated native
handle reuse with zero outstanding references.

Different public definitions collapsing to one translated native handle
remain an unresolved public-identity problem. They do not retroactively change
existing snapshots, and the tracker does not claim definition compatibility for
an ambiguous descriptor-layout snapshot. Full public proxy identity remains
future work for descriptor and pipeline layouts.

Eligible ShaderObject-enabled devices receive the common private lowering
prerequisites even without public RT features. Imported RQ creates that device's
resolver lazily under the state lock. The immutable public `virtual_rt` flag still
controls public buffer/descriptor/memory translation; a lazy registry does not
change the public device profile. Ordinary devices without ShaderObject retain
their original setup path.

RQ reconstruction remains bounded by destination construction and compilation
profiles. Source-required robustness defaults are recorded and fail before native
creation when the destination lacks those semantics. Optional full-subgroup and
workgroup required-size reconstruction remains explicitly unsupported until its
specialized LocalSize proof is complete. Unknown RQ creation chains and some
optional interface constructions fail closed. Enabling every feature privately
is not a general solution: robustness and update-after-bind constraints can
conflict. Registry-mapped core and selected promoted peer capabilities are
checked, as is actual-enabled maintenance4 for LocalSizeId. Other extension
capabilities, per-operation/storage-class requirements and cross-build
compiler-grammar identity remain qualification work. Source code feature
restrictions, including shaderFloat64, also continue
to apply as described in [Features](https://docs.vulkan.org/spec/latest/chapters/features.html).

## External CTS evidence

All runs use VK-GL-CTS revision `d18e4c68543103371715845cd914c71e5859963e`,
the pinned local deqp-vk executable, SDK1.4.357 validation and isolated runner
snapshots. Full QPA, stdout, environment, DLL hashes and statuses are retained.
No failed/skipped case was filtered out.

The initial four-case native control passed4/4. The old source-only reconstruction
passed2/4 and failed null/different layout with ten errors of VUID10383/10384 each;
these are genuine internal SOURCE violations on valid outer BINARY calls.
Evidence: `shader-binary-interface-{native,software}` under
`build-rt-cts-evidence/`. Delivery snapshot `560d4f98...` retained those failures.

The expanded list `docs/rt_cts_shader_binary_extended_cases.txt` contains77 binary
query cases,30 incompatible-binary cases and22 creation cases. Baseline commit
`0902dfb` passed125/129 and failed4: the two layout cases plus
`create.all.fail` and `create.all_with_mesh.fail`. Native passed129/129.

The first full-group/native-binary bridge snapshot passed **129/129** with zero
validation-error/warning cases, no skips, crashes, timeouts or missing results.

| Snapshot | Pass | Fail | NS/crash/timeout/noresult |
| --- | ---: | ---: | ---: |
| Native control | 129 | 0 | 0 |
| `0902dfb` old importer | 125 | 4 | 0 |
| FORMAT2 bridge first snapshot | 129 | 0 | 0 |
| FORMAT3 / IFv2 / final helper snapshot | 129 | 0 | 0 |

Evidence directories are `shader-binary-extended-native`,
`shader-binary-extended-0902dfb`, `shader-binary-extended-v2-first` and
`shader-binary-extended-format3-final` under `build-rt-cts-evidence/`.
The first bridge DLL SHA-256 is
`beb24834b4e02f183b7de3ebbcc627e2a6e6843a2274973fbf89c051de5d14e7`.
Its `diagnostics-summary.json` records zero error/warning cases. This immutable
snapshot predates the final FORMAT3/IFv2 environment header and specialization
helper refinement. The final FORMAT3 DLL SHA-256 is
`2247327289901ea3dfa8b40c52dfb0cb79fed640836f44e1e7db44094ada94bb`.
Its fresh 129-case regression also has zero validation/performance warning or
VUID/sync error cases. Loader GENERAL messages remain in the unfiltered QPA logs;
the diagnostic summary states that distinction explicitly. Both snapshots and
the original four failures remain available.

## Focused functional evidence

`test_shader_object_rq` GPU-builds BLAS/TLAS and executes hit/miss outputs107/207
from source and binary, with mixed RT/compute and ALL pushes. Two independent
VkDevices are alive concurrently; A exports and is destroyed before B imports
and traverses B's own AS/resolver context.

`test_shader_binary_profiles` imports RT-enabled A's RQ/ordinary binaries on a
ShaderObject-only B with no public AS/RQ/RTP feature. It checks saved entry and
specialization (41+push7=48 despite caller specialization99), native NoRT round
trips, null/different binary interfaces with invariant Get, a good/bad/good batch
whose successful prefix executes, full linked VERT+RQ FRAG recreation, and an
originally linked VERT imported alone after source-device destruction. Its RQ
branch is runtime-disabled during this profile test, avoiding foreign AS values.
The new fixture's native VVL/sync control passed; both new layer GPU fixtures also
passed VVL-below/sync validation in the first bridge build with zero diagnostics.
Logs: `build-resource-profile/shader-binary-v2-{native,rq-below,profiles-below}.log`.

The final FORMAT3 separate build also passes four fixture modes in both VVL
orders with synchronization validation enabled: **8/8**, zero VUID, warning or
sync diagnostics. These are RQ hit/miss, SPIR-V1.6 RQ hit/miss, cross-profile
binary imports and immutable sampler reconstruction. The DLL SHA-256 is
`2247327289901ea3dfa8b40c52dfb0cb79fed640836f44e1e7db44094ada94bb`;
`build-source-layout/shader-format3-final-gpu-summary.json` records each mode's
log path and result. The four focused host/lowerer tests also pass in this build
(`shader-format3-final-host.log`).

The SPIR-V1.6 mode changes only the copied source header version and retains
literal LocalSize. Its native VVL/sync control passes. Before the common lowerer
fix, the same actual GPU fixture fails because final validation was hardcoded to
Vulkan1.2/SPIR-V1.5; the retained failure is
`shader-format2-spirv16-software-before-fix.log`. The lowerer now selects Vulkan1.3
for a source/library version above1.5, preserving source instructions and using
the legacy environment otherwise. The final mode executes source and binary
hit/miss across independent devices in both VVL orders.

The independent immutable-sampler fixture additionally samples a border-white
texture and adds a push constant (expected262), after original sampler/layout and
source device destruction. Parent integration records native and both VVL-order
controls. It establishes real sampler-value/private-interface behavior, not just
creation success. The independent final FORMAT3 run above repeats this behavior
in both orders; logs are `shader-format3-final-immutable-{above,below}.log`.

The real-interface-linked host fixture covers source/opaque groups, all-BINARY
mixed batches, original linked peers, alias callbacks, temporary aliases,
retirement concurrency, native partial failure, cache/prefix OOM and corruption.
The refreshed FORMAT3 run records29 resealed negatives and50 allocator positions
(11 failures/39 successes), with zero live native references. Input COMP/VERT/FRAG
SPIR-V was separately checked with the real SDK validator. Unit rewriting remains
an explicit spy; real RQ source execution is proved by the GPU cases.

Earlier pre-migration/single-device evidence remains under
`build-resource-profile/` and `build-source-layout/`. These tests and CTS slices
are evidence for this implementation step, not a complete shader-object or RT
conformance claim.

## Final review follow-up

The follow-up fixes descriptor/pipeline-layout native aliases, YCbCr recipe
semantic preflight and invalid unordered linked-prefix truncation. Interface
host checks now pass **6112** assertions. Pipeline-layout host tests balance
10/10 Device and 2/2 Instance allocations, five failed-create/token paths and
all native references, including gated handle reuse and expired public callback
contexts. Linked-prefix checks retain the preceding independent COMP while
returning a failed linked group as null; the legal lone-VERT prefix still passes.

The post-alias DLL `6f4b7487...` passed **25/25 offline**, **7/7 GPU** (RTP,
maintenance6, shader objects, derivatives and binary fixtures), and **15/15 CTS**.
The final linked-prefix DLL SHA-256 is
`0ce6d6ef9d5dc5e5ecca85c68cf4a6e5be14c207f3072cbe393286bd06c67249`.
It passes **25/25 offline**, **2/2 cross-device/immutable GPU** and a fresh
**15/15 external CTS** run with zero VUID/sync/validation/performance diagnostics.
The fifteen cases contain the eight functional cases above, the four original
failures and three RTP regressions; they overlap earlier selections.

The raw runs are `shader-format3-final-checkpoint` and
`shader-format3-linked-prefix-final` under `build-rt-cts-evidence/`. The
[machine evidence](rt_shader_binary_v3_2026-10-26.json) binds each distinct
snapshot to its source baseline, DLL, case list and raw-result hashes. Earlier
129/129, 59/59, GPU controls and original failures remain separate records.
