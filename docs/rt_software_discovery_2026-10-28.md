<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Software RT discovery and compatibility checkpoint

This checkpoint extends `461adaf52bff47cd261828d0730dddc73a0d0f95` on
`codex/ray-tracing-fallback`. The complete AS/RQ/RTP goal remains active.
The user explicitly allowed incompatibility with newer optional extensions and
did not require all tests to pass. The mandatory baseline is still the target;
complete optional interoperability and a completely green CTS suite are not
exit conditions. The [evidence manifest](rt_software_discovery_2026-10-28.json)
identifies the distinct source/binary observations below.

## Implemented changes

Discovery, device creation and resource planning now use the same software
profile. Its base AS/RQ/RTP bits, properties and supported AS vertex formats do
not depend on native RT advertisement. Extension enumeration includes the
pipeline-library dependency, preserves count/fill semantics, and filters
unsupported optional names. Software properties overwrite stale native RT
outputs while preserving unrelated structures and `pNext`. The fifteen AS
vertex formats share the builder's decode predicate. Disabled optional commands
belonging to enabled base extensions retain layer-owned entry points and cannot
forward virtual objects to native RT host/indirect functions.

Private traversal and builder modules are genuinely compiled for Vulkan 1.1
with SPIR-V 1.4. Typed compilation, optimization and linking select environments
from the actual input versions and retain newer application requirements.
Vulkan 1.1 device creation injects the required available backend extension
aliases, including SPIR-V 1.4/float-controls for an AS-only application that did
not request those private shader dependencies. Applications below Vulkan 1.1
are outside the software candidate profile.

The unused destination PrivateArray transport and its diagnostic fixture are
deleted. Production RTP uses execution-owned BDA arenas. Source Function and
Private variables, pointers, initialization and memory semantics remain in the
compiler and tests. The source-Private offset limit is still checked: the 8 GiB
regression proves explicit rejection, not execution of an 8 GiB frame.

Pool allocation preflight checks native maximum buffer and allocation sizes
before the corresponding native calls. This closes those particular size
checks, not complete application/private allocation-count or memory budgets.

Public replay now owns push-descriptor/template data and restores descriptor
history across internal RTP compute. The descriptor-buffer repair restores the
global buffer binding before offsets and embedded samplers, including native
graphics bindings in the journal. Template destruction and overwritten source
arrays after recording do not invalidate the captured data. A CPU oracle covers
subsequent graphics Draw state; real graphics Draw execution is not part of the
public GPU fixture.

Debug names/tags and native PrivateData slots work with owned virtual AS,
RTP pipeline, RT shader-module, template, AS query-pool and deferred-operation
objects. Registries use full-width typed ownership keys. Destruction removes
ownership and annotations before token reuse; slot data is cleared before
native slot destruction. Ordinary native objects pass through. Metadata
initialization also covers ordinary DHO-only devices.

## Explicit compatibility limits

The software profile excludes these newer optional interfaces because they
consume shader/pipeline objects for which no complete virtual adapter is
provided:

- `VK_EXT_shader_module_identifier`
- `VK_KHR_pipeline_executable_properties`
- `VK_EXT_pipeline_properties`
- `VK_KHR_pipeline_binary`
- `VK_EXT_device_generated_commands`
- `VK_NV_device_generated_commands`
- `VK_NV_device_generated_commands_compute`

Ten corresponding feature bits are masked and true requests on virtual-RT
devices are rejected. A false feature structure alone is accepted. Pipeline
binary is also rejected by extension name because key queries can consume
shader modules with its feature false. Ordinary non-RT device creation retains
these native features. Discovery on a software-qualified physical device
reports the software profile, so it intentionally filters those optional names
even before an application's eventual device choice.

Legacy `VK_EXT_buffer_device_address` remains incompatible with the private
core/KHR BDA backend, but ordinary native-only devices can still enable it. A
regression and old-behavior negative controls cover the formerly unconditional
rejection/filter. Existing optional RT restrictions remain: host AS commands,
capture/replay, indirect traces, RT maintenance/position-fetch and vendor RT
extensions are not newly implemented. Ordinary compute push descriptors and
descriptor buffers have bounded interoperability coverage; RT pipeline layouts
using those optional descriptor transports are not claimed supported.

## Evidence and retained failures

The native-RT guard now masks RT extensions, feature/property outputs, AS vertex
format bits and RT function lookup. Masked evidence requires mask-version 1;
execution-only and forced-dispatch profiles cannot satisfy that audit. Masking
an RTX 3080 does not qualify a physical GPU without RT support.

A frozen preliminary DLL, SHA-256
`482a74431c156efa6b56110b60c0351dcfdfecf10b0ab3df67a92496b8620ffa`,
passed twelve public masked GPU runs with validation above/below the layer and
50 external CTS cases. Those observations preceded the final SPIR-V 1.4,
metadata, descriptor-buffer repair and compatibility gates. They are retained
as preliminary evidence rather than attributed to the final DLL.

Initial observations are also retained:

- A matrix launched from the wrong working directory could not locate a shader.
- Outer validation exposed invalid positive-fixture inputs: an intentionally
  rejected UPDATE/undersized-AS sequence and an 8-byte instance-pointer offset.
  Negative calls now have a separate mode; the pointer array uses 16-byte
  alignment. The validation callback wrapper remains intact in including tests.
- The first ordinary descriptor-buffer compute test produced correct values but
  validation reported `VUID-vkCmdSetDescriptorBufferOffsetsEXT-pBufferIndices-08065`.
  This was an implementation defect: internal work disturbed the global buffer
  binding. The repair replays that binding before restoring descriptor offsets.

Final build, public GPU and external CTS results are recorded in the manifest
with their own DLL and input hashes. Native controls, unsupported results and
raw failed observations remain separate from software passes. The external CTS
binary comes from `D:/Codes/VK-GL-CTS`, revision
`d18e4c68543103371715845cd914c71e5859963e`; no conformance claim follows from
the selected slice.

| Observation | Result and scope |
| --- | --- |
| Unified offline tests | 46/46 passed; subsequent EXT_BDA host tests and formatted-lowering tests also passed |
| Selected GPU matrix | 30/30 passed: 28 public API runs and two compiler-component runs; seven native controls; no validation error markers or guard rejections |
| Selection controls | Masked native-only and previous `461adaf` DLL both returned unsupported (77); current DLL selected/created successfully (0) |
| Final external CTS | 13/13 passed: AS compaction/serialization, RQ AS operations, pipeline library, geometry-transform UPDATE and data spill |
| Integration | Standalone build and top-level RT target passed; full M0-M5 qualification remains open |

The GPU matrix used DLL SHA-256
`e03d2180d83a33ecd468865d952be827859e1e4ae78d42fe58e2c7f707b3d0b1`.
After whitespace-only formatting of the RQ lowering source, the rebuild,
selection control and final CTS used
`327e13480f8986817af7d174fb15d274975378ef66b2f73f90ba8111310c78d5`.
The preliminary 50 cases and final thirteen are distinct observations and
overlap; do not report them as 63 distinct cases or 50 final-DLL passes. Most
registered tests and optional suites were not selected for this checkpoint.

The public nested-Trace fixture exercises depths 2 and 31, optimized and
unoptimized pipelines, two inputs and 27 three-dimensional invocations. It
checks retained seventeen-word activation locals, payloads, entry/return order
and padding against an independent CPU oracle. Software public stack sizes are
exactly `1376 + depth * 1248`: 3872 and 40064 bytes. A reported depth of 31 is
the tested software profile cap, not a normative KHR maximum. This fixture's
Vulkan 1.1 application also requests synchronization2 and maintenance4 through
the shared fixture initialization; it does not prove every minimum-base device.

## Remaining mandatory work

M0-M5 are not complete. In particular, discovery's backend feature/resource
predicate does not yet qualify all queue, device-group and protected execution
combinations. The query implementation requires a compute/timestamp substrate;
compute queues with `timestampValidBits=0` are a concrete remaining case because
AS property writes themselves do not require timestamps. Multi-device/protected
combinations also need explicit qualification. Generic reset/copy queue flags
alone do not establish a legal graphics/video-only AS-query scenario: the
pool's query-type queue-family constraints must also hold. These are baseline
qualification questions, not newer optional-extension exclusions.

Complete private/application resource budgets, remaining shader pointer and
memory-model semantics, AS construction/count/numerical qualification and full
logical-stage synchronization/native-query interactions remain open. Physical
no-RT, cross-vendor and POSIX qualification is also outstanding. Continue with
these baseline gaps and representative external CTS, without expanding this
checkpoint into support for every newer native extension.
