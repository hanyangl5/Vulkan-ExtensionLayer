<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# OMM device components and native adapters, 2026-10-26

This checkpoint extends `4c37e44` with actual device-side build/lookup and native
consumer/compiler mechanisms. It does not complete M0-M5 or advertise emulated
`VK_EXT_opacity_micromap`. Public micromap operations, AS sidecars and complete
consumer instrumentation remain required by the original goal.

## Executed checks

| Check | Actual result and scope |
| --- | --- |
| Standalone integrated CTest | 9/9 Pass, including five GPU targets with SDK validation and synchronization validation |
| Parent VEL integration | New targets build; 4/4 offline tests Pass |
| Host-only build | 2/2 Pass with native proofs disabled and no Vulkan dependency |
| Parallel GPU build | 25 cases Pass; GPU-generated triangle/data inputs, canonical comparison, clone, invalid-input canaries and source destruction |
| Large build | Both encodings through level 12, 26 records, 8,389,344 output bytes |
| GPU lookup | 748,406 checks Pass after destroying the actual GPU build inputs; independent inverse centroids, boundaries, indices and corrupt-input checks |
| Native RQ | 26 explicit scenes, 1,000 CPU/GPU opacity comparisons, 96 packed-micromap lookups at real native barycentric intersections Pass |
| Native RTP | 37 checks Pass, including 8 legal zero triangle hit-group controls/replacements, procedural side effects and application Any-Hit behavior |
| Query state compiler | Input/output SPIR-V validation, actual GPU arrays/helpers/reinitialization in capture/exposure modes Pass |
| Independent compiler graphs | 720 checks Pass across 15 valid shapes plus 4 rejected Vulkan pointer graphs; includes mixed structs, signed flags and multiple stages |

The existing CPU evaluator and storage reference suites continue to pass. Each
generated test/device shader is compiled and then checked by `spirv-val` as a
build dependency. A successful compiler process alone is insufficient.

Hardware/software match the prior native CTS inventory: RTX 3080, driver 596.49,
device Vulkan 1.4.329, SDK/validation 1.4.357.0 and LLVM 20.1.3. No fallback or OMM
layer is loaded by these tests. Native RTP proof devices enable native RTP/AS;
ordinary RQ proof devices independently enable native RQ/AS.

The final integrated run is `build-omm/device-integration-final-vvl-sync.log`.
Raw component evidence remains in `build-omm-storage`, `build-omm-gpu-lookup`,
`build-omm-native-pipeline` and `build-omm-query-graph`. Hashed component artifacts
and source fingerprints are recorded in
[the evidence manifest](omm_device_components_2026-10-26.json).

## Implementation behavior

`omm_build.comp` first validates GPU-resident records and emits canonical header
and record offsets. A compute dependency precedes parallel word-owned encoding.
No CPU read of the GPU triangle metadata or status is needed to dispatch either
phase. The test uses a completion fence only when checking results or releasing
inputs. The payload writer does not race adjacent packed bytes.

The large build waits measured about 12.9-46.7 ms in the component runs. These
are host submission/completion measurements, not isolated GPU timestamps or a
performance guarantee. Large record-count planning is still serial and needs
scalability qualification; the large payload test contains only 26 records.

`omm_lookup.glsl` consumes the canonical blob through known BDA spans and performs
bird indexing, packed state decode and EXT association. Special indices are
recognized at the stored width before base addition. The raw 8-bit helper is a
format utility; EXT association exposes uint16, uint32 and implicit indices.
The native RQ fixture connects this lookup to actual candidate barycentrics.

`omm_query_rewrite.cpp` retains native query objects and adds parallel logical
flags with matching Private/Function arrays and structs. Ordinary struct members
keep their index through internal placeholders. Helper signatures/calls receive
the corresponding state pointers. Initialization captures logical flags and
GetRayFlags reads them, including signed 32-bit forms. Unrelated shaders remain
byte-identical; failure leaves the entire output unchanged.

Capture-only preparation preserves native traversal behavior. Candidate exposure
requests NoOpaque and strips opacity/culling/force-two-state flags from the native
operation; it must be followed by a complete candidate wrapper. This is an
internal compiler stage, not a finished public shader instrumentation API.

The RTP proof retains native TraceRay, intersection and shader calls. An injected
Any-Hit filters OMM candidates and dispatches application behavior only when
logically required. A procedural Intersection guard applies logical opacity
culling before application side effects. Explicit context access is through BDA;
the proof does not consume an extra application trace recursion level.

## Findings retained and remaining gates

- Runtime 64-bit overflow division in lookup caused an NVIDIA NVVM pipeline
  compilation error. Exact split-product arithmetic and power-of-two alignment
  checks replace that construct without narrowing the integer domain. Failed and
  passing probes remain available.
- Valid SPIR-V can contain query members in mixed structs. Independent validation
  exposed the missing shadow shape; that path is implemented and covered. The
  attempted GLSL runtime struct fixture produced an illegal UniformConstant
  object, caught by the mandatory validator. Its source and invalid SPIR-V remain
  in `build-omm/query-state-struct-glslang*`; native runtime struct behavior still
  needs an independently valid executable fixture.
- Query Phi/Select pointers in the relevant Private/Function storage classes are
  prohibited by Vulkan 1.2. They remain negative validation regressions; unused
  production branches for them were removed. This does not waive AS-value
  Phi/Select provenance, which is a different and still open requirement.
- BDA access requires known readable owned spans. Word-based reads require
  readable tail padding; public legal buffer tails, metadata lifetime and
  synchronization need complete adapter handling. Arbitrary nonzero addresses
  cannot be authenticated through shader arithmetic.
- Native AS provenance, descriptor writes/copies/indexing, GPU-generated
  instances, automatic candidate wrapping and native OMM declaration removal are
  not connected to these compiler components yet.
- RTP contexts remain explicit, single-launch experiments. Nested/callable
  execution, shader-record data, general SBT rewriting and indirect inputs remain
  open. Native null-group controls do not prove the complete SBT contract.
- Public build recording must preserve application state. Public object commands,
  queries/availability, serialization/relocation and cross-queue synchronization
  are not replaced by the internal blob/reference helpers.

The external CTS native baseline remains **8 Pass, 2 Fail**, with both failures at
`vkCreateQueryPool` in the two micromap size-query cases. No software OMM CTS pass
is claimed here; see [the exact native inventory](omm_cts_inventory_2026-10-26.md).
