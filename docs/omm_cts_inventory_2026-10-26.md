<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# OMM external CTS inventory and native control, 2026-10-26

This is an external test inventory and **native-driver control**. No software OMM
layer was loaded, and none of these results establishes software OMM support.
The current goal remains the complete EXT device contract in
[the specification](opacity_micromap_layer.md) and
[the implementation plan](opacity_micromap_plan.md).

## Pinned inputs

| Input | Value |
| --- | --- |
| External CTS checkout | `D:/Codes/VK-GL-CTS` |
| Source revision | `d18e4c68543103371715845cd914c71e5859963e` |
| Executable | `build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe` |
| Executable size | 63,076,864 bytes |
| Executable SHA-256 | `8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380` |
| QPA release | `vulkan-cts-1.4.7.0-103-gd18e4c68543103371715845cd914c71e5859963e` |
| Resource archive | `external/vulkancts/data` |
| SDK, headers, registry, validation | Vulkan SDK 1.4.357.0 |
| Exact case list | [omm_cts_smoke_cases.txt](omm_cts_smoke_cases.txt) |
| Case-list SHA-256 | `5e5bb093aa827d77656f46ece308aac2d895a69286e1bc17f4dd38cba6f4dff7` |
| Runner | Existing `D:/Codes/Vulkan-ExtensionLayer/scripts/run_rt_cts.py`, read without modification |
| Runner SHA-256 | `b499e5b63c2b5140568d4a3d63986478f79033e271b9cfcf24a6eff13b3fa163` |

The two OMM EXT source files and both mustpass lists were compared with their
HEAD blobs, normalizing CRLF/LF; all four match the declared revision. A sandboxed
Git attempt could not read the checkout, so the definitive comparison used a
read-only approved command. No CTS source or executable was modified.

## Available CTS coverage

These are exact entries in `external/vulkancts/mustpass/main/vk-default`, counting
only `.opacity_micromap.ext.` and excluding the separate KHR groups.

| Mustpass file | EXT cases | Main coverage |
| --- | ---: | --- |
| `ray-query.txt` | 4,145 | 4,032 render cases: 1,344 each in vertex, compute and ray-generation stages; 65 copy/misc; 36 uint16 association cases; 12 update cases |
| `ray-tracing-pipeline.txt` | 4,886 | Opacity/instance/ray flags, both encodings, special/null indices, levels 0-15, nonzero base, 36 uint16 cases, four many-triangle cases, 12 updates, two property queries |

Useful initial exact prefixes:

- `dEQP-VK.ray_query.opacity_micromap.ext.render.compute_shader.no_flags.`
- `dEQP-VK.ray_query.opacity_micromap.ext.copy.clone.` and `.copy.compact.`
- `dEQP-VK.ray_query.opacity_micromap.ext.index_type.uint16.`
- `dEQP-VK.ray_query.opacity_micromap.ext.update.`
- `dEQP-VK.ray_tracing_pipeline.opacity_micromap.ext.no_flags.`
- `dEQP-VK.ray_tracing_pipeline.opacity_micromap.ext.many_triangles.`
- `dEQP-VK.ray_tracing_pipeline.opacity_micromap.ext.query.`

Each line in the smoke list is checked against one of the pinned mustpass lists.
Cases above a reported subdivision limit must remain NotSupported, not passes.
The current native limits are 12 for both encodings; inventory levels 13-15 are
therefore not an expectation that this device can execute those levels.

The pinned RQ tests exercise a smaller flag space than RTP: FORCE_OPAQUE instance
and ray, DISABLE_OPACITY_MICROMAP instance, and both FORCE_2_STATE controls. RTP
also exercises FORCE_NO_OPAQUE and opacity culling. RQ NO_OPAQUE, culling, ordinary
AABB opacity, query arrays/helpers/reinitialization and exact candidate/committed
state need independent regressions. No EXT end-to-end micromap serialization case
was identified in these two files: the two size queries do not test serialization,
compatibility or relocation. Full lifecycle and synchronization requirements must
not be inferred from a render match.

## Independent indexing and state oracle

Pinned CTS source paths, relative to `external/vulkancts/modules/vulkan`:

- `ray_query/vktRayQueryOpacityMicromapTests.cpp:372-425` implements
  `calcSubtriangleCentroid(index, subdivisionLevel)` using bit unshuffle and
  prefix XOR. The same inverse algorithm is in
  `ray_tracing/vktRayTracingOpacityMicromapTests.cpp:290-343`.
- RQ lines 1039-1103 decode packed random 2/4-state data into expected result
  classes. Rays target the corresponding microtriangle centroids.
- RTP lines 737-826 resolve expected opacity and culling. They are useful test
  evidence; the pinned Vulkan specification remains authoritative for precedence.

For an independent indexing test, generate each centroid using the CTS inverse,
then require the implementation's forward map to return its original index.
Exhaust low levels, sample higher levels deterministically, and add separate
edge/vertex/diagonal tests. A round trip alone does not test permitted hardware
floating-point uncertainty at triangle boundaries.

The Vulkan [OMM reference mapping](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/appendices/VK_EXT_opacity_micromap.adoc)
provides the forward algorithm. It clamps coordinates, quantizes them, fixes the
outer diagonal and fractional orientation, then interleaves the two derived bit
planes. That source is CC-BY-4.0; preserve attribution if adapting its code.

The [Vulkan 1.4.357 traversal specification](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/v1.4.357/chapters/raytraversal.adoc)
defines active OMM lookup before opacity overrides: ignored results end processing;
force-two-state maps the two unknown states to ignore/opaque; instance overrides
follow and ray opacity overrides take precedence over them. Opacity culling uses
the resulting class. Geometry opacity applies when no OMM is active. EXT-enabled
RQ consumes OMM without the KHR execution mode. These rules require preserving
logical flags even if native candidate enumeration uses different flags.

## Native capability and execution result

Fresh `vulkaninfoSDK.exe --text`, with implicit layers disabled and inherited
`VK_INSTANCE_LAYERS` removed, reports:

| Property or feature | Native value |
| --- | --- |
| GPU | NVIDIA GeForce RTX 3080 |
| Driver | 596.49 |
| Vulkan device API | 1.4.329 |
| `VK_EXT_opacity_micromap` | revision 2 |
| `micromap` | true |
| 2-state maximum subdivision level | 12 |
| 4-state maximum subdivision level | 12 |
| `micromapCaptureReplay` | false |
| `micromapHostCommands` | false |
| Native AS, RQ, RTP and BDA | true |

Thus this driver supplies a native OMM differential reference; the GPU model alone
must not be used to infer absence of the extension. A software adapter will need
an explicit development selection to exercise emulation on this machine while
retaining native pass-through as the default.

Evidence directory: `build-omm-cts-evidence/native-baseline` in the OMM worktree.
`summary.json` SHA-256:
`ca1b7be52cbfa24c382125f2d8ec675ba6993f5e2fbfb11d91b88e4aac1891e3`.
Device report SHA-256:
`62edabde0a4deb8375d0be2140bbe6cd7f756bb43ef7b6e06028afbbc88d66e3`.

| Selected case area | Native result |
| --- | --- |
| RQ 2-state level 1 | Pass |
| RQ 4-state level 1, nonzero base | Pass |
| RQ special-only null micromap handle | Pass |
| RQ uint16 special index | Pass |
| Micromap clone, 2-state level 1 | Pass |
| Micromap compact, 4-state level 1 | Pass |
| RTP force-two-state, unknown-transparent special index | Pass |
| RTP 4-state level 1 | Pass |
| EXT compacted-size property query | **Fail** |
| EXT serialization-size property query | **Fail** |

Total: **8 Pass, 2 Fail, 0 NotSupported, 0 crash, 0 timeout, 0 missing result**.
Both failures occur at `vkCreateQueryPool`, returning `VK_ERROR_UNKNOWN`, before
submission of the property-write commands. The run contains no `VUID-` diagnostic
in case stdout. The root cause is unresolved; these failures are retained and
must not be reclassified as expected NotSupported or an emulation-layer defect.
The passing eight cases confirm useful native reference paths, not complete
native conformance.

Each case ran in a separate process, with a 60-second outer timeout, CTS validation
enabled, the SDK validation manifest selected, and `VK_LAYER_VALIDATE_SYNC=1`.
The deprecated `VK_LAYER_ENABLES` variable was removed. The runner cleared inherited
layer variables and disabled implicit layers. Its recorded environment contains
no fallback or OMM layer. The exact command, per-case stdout/QPA, binary hashes,
SDK hashes and environment are in the retained summary.

Reproduction, from the OMM worktree (PowerShell; use a fresh output directory):

```powershell
Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
Remove-Item Env:VK_LAYER_ENABLES -ErrorAction SilentlyContinue
$env:VK_LAYER_VALIDATE_SYNC = '1'
& 'C:/Users/hylu/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' `
  D:/Codes/Vulkan-ExtensionLayer/scripts/run_rt_cts.py `
  --cts D:/Codes/VK-GL-CTS/build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe `
  --archive-dir D:/Codes/VK-GL-CTS/external/vulkancts/data `
  --cases docs/omm_cts_smoke_cases.txt --mode native `
  --output-dir build-omm-cts-evidence/native-baseline `
  --validation-layer-dir C:/VulkanSDK/1.4.357.0/Bin --timeout 60 `
  --cts-revision d18e4c68543103371715845cd914c71e5859963e `
  --device-info-file build-omm-cts-evidence/native-device.txt
```

## Implementation audit consequences

The pinned SDK registry requires native AS plus synchronization2 or Vulkan 1.3
for EXT. It lists nine device-path commands and five optional host-path commands.
Do not promote the KHR extension name: KHR has a different resource model and a
`VK_KHR_device_address_commands` dependency.

M0 must prove descriptor AS provenance, internal resource delivery at application
limits, and native-candidate wrapping before advertising emulated micromap=true.
The default native path should continue to pass ordinary geometry to the driver.
The emulation test selection must force the intended backend, preserve unrelated
feature reporting, and record the selected backend in evidence. It cannot count a
native pass-through run as software OMM validation.

Shader transformations must preserve ignored OMM results even under force-opaque,
remove native OMM declarations only when emulating, and preserve application-visible
original ray flags. RQ needs transparent candidates filtered, opaque candidates
auto-confirmed, and AABB state preserved. RTP also needs procedural Intersection
side effects checked before later Any-Hit filtering; those effects cannot be undone
by rejecting a reported intersection afterwards.
