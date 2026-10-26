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
`VK_EXT_custom_resolve`; an unsupported custom-resolve source chain fails before
native creation and an unsupported custom-resolve envelope is incompatible.
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

The implementation is in `layers/raytracing/src/vkrt_shader_objects.c` and uses
`vkrt_rewrite_shader_module` for the same RQ lowering as shader modules. Shader
handles remain native. This component does not qualify the complete RT backend,
activate the resource-budget property planner, or establish complete M2/M4 support.

## Public source behavior

On a virtual RT device, SPIR-V RQ source is lowered before native shader-object
creation. Unsupported RT execution models/instructions fail before the native
call. Explicit RT push-stage bits are stripped, RT-only ranges are omitted, and
the special `VK_SHADER_STAGE_ALL` value remains intact. Ordinary source on an
ordinary device reaches native creation through the original create-info array.
The layer separately copies source metadata before that call. Every native
creation retains the original batch size and shader allocator, native partial
creation, result code, and native handle representation. Pre-native failures
null every output handle. Borrowed arrays/chains are never modified.

Binary reconstruction restores the original source entry point and specialization,
including captured specialization data rather than a binary caller's changed
value or null entry name. It uses the binary caller's stage, flags, next-stage,
and descriptor/push-layout interface: the specification does not make all these
members irrelevant for binary input. This is insufficient for arbitrary binary-call
interfaces: see the CTS gap below. Subgroup-size and custom-resolve creation
chains are serialized; unsupported semantic chains fail closed before creation.
Descriptor-heap source serialization is not implemented.

These decisions follow the conditional SPIR-V entry-point requirements and shader
layout compatibility rules in
[`VkShaderCreateInfoEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/VkShaderCreateInfoEXT.html).
In particular, descriptor layout objects need only survive the create command.
The implementation retains no descriptor-layout handle for later source rebuilding.

## Versioned binary contract

Every successful source creation has a device-owned canonical SOURCE envelope.
It stores original SPIR-V, entry name, specialization map/data and supported
creation metadata. Its fixed-width little-endian format contains no pointers,
callbacks, Vulkan object handles or resolver address. Imports decode every member,
check lengths/overflow/checksum/format/identity, and validate embedded SPIR-V with
SPIRV-Tools before making a native SPIR-V call. Validation uses the Vulkan 1.4
grammar and permissive scalar, UBO-standard and workgroup-scalar layout options;
it does not restrict compatible imports according to an instance API version.

All recognized imports reconstruct SPIR-V, including ordinary shaders in linked
batches. Therefore a linked ordinary VERT plus RQ FRAG binary batch becomes a
uniform native SPIR-V batch. There is no mixed SOURCE/opaque-native fallback.
Unrecognized raw native binaries are incompatible on every logical profile.

The advertised UUID is the first 128 bits of SHA-256 over a fixed namespace,
native UUID, format version and physical backend-eligibility domain. It is
independent of logical RT enablement. Native binary version ordering is retained.
Changes breaking envelope compatibility must change the format/UUID domain.
Repeated Get calls return identical bytes while the native handle is live. A
short output buffer receives no bytes, reports size zero, and returns `VK_INCOMPLETE`.
These behaviors implement the identity, version, cross-device and invariance
requirements in
[Binary Shader Compatibility](https://docs.vulkan.org/spec/latest/chapters/shaders.html#shaders-objects-binary-compatibility).

Physical eligibility here means shared lowering prerequisites, not full RT feature
qualification. Every eligible ShaderObject-enabled logical device receives those
private native prerequisites, even if the application enabled no public RT feature.
An imported RQ envelope lazily creates that destination device's resolver under
a state lock. The immutable public `virtual_rt` flag still controls public buffer,
descriptor and memory translation. Ordinary devices without ShaderObject skip
this state/backend setup. Other source feature restrictions remain applicable;
for example, the normative shaderFloat64 rule concerns shader code, beyond the
SPIR-V capability-specific condition in
[Features](https://docs.vulkan.org/spec/latest/chapters/features.html).

## Lifetime and concurrency

The cache state, canonical record and blob use DEVICE scope and the original
DeviceCreate allocator, falling back to the original InstanceCreate allocator.
The device allocator is captured even when maintenance9 permits zero queues.
Native shader allocation callbacks pass through unchanged and are never retained
by the shared cache. Equal native non-dispatchable handles can alias multiple
live creations: reference counting preserves the first canonical byte string
until the final alias disappears. Native Create/publication and native
Destroy/cache-retirement are serialized to prevent a reused native handle from
merging with a retired source record. Device destruction also releases remaining
cache state. This follows the allocator hierarchy and lifetime rules in
[Host Memory](https://docs.vulkan.org/spec/latest/chapters/memory.html#memory-host).
Allocation callbacks cannot call Vulkan commands.

## Verification and limits

The focused host and public GPU tests passed 5/5 before source-directory migration:

- `vkrt.shader_objects`
- `vkrt.shader_object_rq.software` and `.native`
- `vkrt.shader_binary_profiles.software` and `.native`

The host spy covers partial native failure, pre-native failure, borrowed inputs,
UUID SHA-256 vectors and a related-native-UUID/profile collision regression,
short-buffer nonwrites, forged checksums with malformed source, raw binary
rejection, device allocator OOM, instance fallback, alias allocator retirement
and final cleanup. A subsequent Windows concurrency regression pauses native
Destroy, starts replacement Create, verifies serialization, and checks the
reused handle exports the replacement source. The address-cell host fixture
also covers zero-queue allocator capture and OOM cleanup.

The RQ GPU fixture builds BLAS/TLAS through public commands and executes hit/miss
outputs 107/207 from source and exported binaries, with mixed RT/compute and ALL
push layouts. Two independent VkDevices are created; after A exports its shaders
and is destroyed, B imports and executes against B's own AS/resolver context.

The profile fixture exports on RT-enabled A and imports on ShaderObject-only B
with no public AS/RQ/RTP features. RQ uses a runtime-disabled AS branch, so it
never consumes an invalid cross-device AS value. It verifies original entry and
specialization (41 + push 7 = 48 despite binary-call specialization 99), ordinary
NoRT source/binary round trips, and linked ordinary VERT + RQ FRAG binary creation.
Logs show distinct live resolver addresses, 74964992 and 171945984. Linked shaders
are creation-tested, not rendered. A recomputed-checksum malformed source import
returns incompatible without a native SPIR-V diagnostic.

Both GPU fixtures passed again against the final migrated/formatted
`build-raytracing-unified/layers` DLL with synchronization validation and
validation above or below the layer. All four processes exited zero, with zero
VUID/warning/error messages. Current logs and exit statuses are retained under
`build-source-layout/`:

- `shader-object-rq-above.log`, `shader-object-rq-below.log`
- `shader-binary-profiles-above.log`, `shader-binary-profiles-below.log`
- `shader-vvl-results.json`

Earlier build, host concurrency and GPU evidence remains under `build-resource-profile/`:

- `shader-binary-final-build.log`, `shader-binary-final-tests.log`
- `shader-binary-concurrency-host.log`
- `shader-object-rq-final-above.log`, `shader-object-rq-final-below.log`
- `shader-binary-profiles-final-above.log`, `shader-binary-profiles-final-below.log`

External CTS source inspection identified an additional unresolved interface case:
`dEQP-VK.shader_object.binary.query.comp.unlinked.shader_from_binary_null_layout`
and `shader_from_binary_different_layout` in
`external/vulkancts/modules/vulkan/shader_object/vktShaderObjectBinaryTests.cpp`
deliberately import with absent or incompatible layouts. The outer BINARY call
is not subject to SPIR-V-only descriptor/push-interface VUIDs
10064/10065/10383/10384/10385. Reconstructing native SPIR-V with those incoming
interfaces can therefore create a new native validation failure. Full binary
qualification needs transport/reconstruction of the original compilation
interface plus a native-binary bridge or equivalent layout treatment. The current
envelope does not retain descriptor-layout definitions or immutable-sampler
definitions, and no claim of complete binary compatibility is made. This source
inspection is now confirmed by actual external CTS. The exact four-case list is
`docs/rt_cts_shader_binary_cases.txt`, pinned to VK-GL-CTS
`d18e4c68543103371715845cd914c71e5859963e`. Native controls with SDK validation
passed 4/4, including null and different-layout imports. Software-RT using the
final migrated/formatted DLL passed 2/4 and failed 2/4. Neither run had skips,
crashes, timeouts or missing results:

| Suffix under `dEQP-VK.shader_object.binary.query.comp.unlinked` | Native | Software-RT |
| --- | --- | --- |
| `same_shader` | Pass | Pass |
| `shader_from_binary` | Pass | Pass |
| `shader_from_binary_null_layout` | Pass | Fail: 10 API usage errors, `codeType-10383` |
| `shader_from_binary_different_layout` | Pass | Fail: 10 API usage errors, `codeType-10384` |

The two successful software cases emitted no validation errors. Each failed
case emitted ten errors of its respective VUID before validation's duplicate
message limit. These are real layer-induced native SPIR-V interface violations
on valid BINARY inputs, not malformed caller input or native-driver failures.
Complete QPA/stdout and reproducibility metadata remain in
`build-rt-cts-evidence/shader-binary-interface-native/summary.json` and
`build-rt-cts-evidence/shader-binary-interface-software/summary.json`. The tested
software snapshot SHA-256 is
`f3253f70c3208328036740c04e2b5d4a5e1f07cf3aaf9620b9ade5e5704266e2`.

The next implementation step is to capture the original compilation interface
while its public layouts and samplers are alive; serialize definitions rather
than handles, including descriptor binding/immutable-sampler semantics and push
ranges; reconstruct destination-owned private compilation layouts; then bridge
to native binary creation or another equivalent path that preserves the outer
BINARY caller's legal interface behavior. This needs coverage on ordinary
ShaderObject-only devices as well as virtual RT devices. Keeping incoming
layouts, comparing handle values, or merely rejecting these valid matching-UUID
inputs cannot establish the required compatibility guarantee. The two failing
CTS cases must pass with validation before binary compatibility is qualified.

These focused tests complement external CTS; they do not replace it. General
optional shader-object interoperability (including descriptor heaps and other
unsupported creation chains), exhaustive malformed-input coverage and dedicated
concurrent lazy-registry initialization scheduling remain outside this evidence.

The final delivery rebuild after optional-header guards and the result-enum
alias repeats the three RT smoke passes and the same two binary passes/two
layout failures (5 pass, 2 fail overall). Its independent snapshot SHA-256 is
`560d4f98108dd27b848429b79d89ce8d9b35381ec580ada861f81a162f79e0c3`. Raw evidence is in
`build-rt-cts-evidence/unified-source-delivery-check/summary.json`; the rebuilt
three focused host/GPU checks pass in `build-source-layout/shader-delivery-final.log`.
