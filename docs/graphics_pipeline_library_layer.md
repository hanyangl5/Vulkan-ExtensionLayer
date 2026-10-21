# VK_LAYER_PA_graphics_pipeline_library

This layer transparently accelerates graphics pipeline creation on drivers supporting
`VK_EXT_graphics_pipeline_library` (GPL). It converts eligible monolithic pipelines into cached
vertex-input, pre-rasterization, fragment-shader and fragment-output libraries, then links them
without LTO. It exposes no additional Vulkan extensions to applications.

The implementation came from `xiaomi-vulkan-graphics-pipeline-library-layer`
commit `9a2dad3c3b14714ae650f050d405e8a81f9e1246`, retaining Xiaomi's Apache-2.0 notices. The loader name
is `VK_LAYER_PA_graphics_pipeline_library`; the binary and manifest use `VkLayer_PA_graphics_pipeline_library`.

## Enablement and conversion

At device creation, the layer probes capabilities and preserves features explicitly disabled by the
application. It adds only the supported extensions and omitted feature structures it needs.

| Available capabilities | Device tier |
| --- | --- |
| GPL, extended dynamic state and private data | GPL conversion with five-state dynamization |
| GPL only | GPL conversion with original static state |
| No GPL, or GPL explicitly disabled | Native passthrough |

The five promoted states are cull mode, front face, depth test enable, depth write enable and depth
compare operation. Already-dynamic application states are preserved. Depth states are promoted only
when depth/stencil state is present. Binding a converted pipeline restores its original static values
through the downstream dynamic-state commands.

Conversion proceeds as follows:

1. Check flags, extension chains, dynamic states and device-specific compatibility restrictions.
2. Partition state by GPL subset and normalize promoted values for cache lookup.
3. Reuse or build each required library, then link an executable using the application's allocator.
4. Attach the bind-time state record if states were promoted. If attachment fails, destroy the
   executable and record, then use native creation with the original request.

Mesh shaders omit the vertex-input library. Static rasterizer discard builds only vertex input and
pre-rasterization, or pre-rasterization alone for mesh shaders, without inspecting ignored fragment
state. Dynamic discard retains all applicable subsets.

## State ownership and cache keys

| State | Owning library |
| --- | --- |
| Vertex attributes/bindings, topology and primitive restart | Vertex input |
| Viewports/scissors, rasterization, line state and tessellation | Pre-rasterization |
| Depth/stencil and representative fragment test | Fragment shader |
| Blending, color writes, logic operations and blend constants | Fragment output |
| Multisampling, sample masks/locations and coverage state | Applicable fragment libraries |
| KHR fragment shading rate | Both shader libraries |
| Dynamic-rendering local-read input indices | Fragment shader |
| Dynamic-rendering attachment locations and mixed attachment sample counts | Fragment output |
| Pipeline robustness | Every applicable library; stage overrides remain with their shader |

Both traditional render passes and dynamic rendering are supported. Shader keys include view masks;
output keys include attachment formats. Fragment-shader keys also include the color-attachment count
because it sizes implicit local-read mappings. Both fragment libraries retain multisample state and
its dynamic-state declarations: opaque shaders can require per-sample execution even when
`sampleShadingEnable` is false. Ignored dynamic multisample fields are normalized in the actual
library create info as well as the key, so a reused FS library and a newly built FO library have
identical shared multisample definitions. When local read is enabled, omitted color input mappings are made
explicit on standalone fragment-shader libraries without changing their identity-map semantics.

Cache keys serialize state values and array contents, excluding ignored dynamic arrays and pointer
addresses and structure padding. Ignored dynamic scalar values are excluded as well; dynamic
primitive topology retains its topology class for devices that restrict topology changes.
Inline SPIR-V, required subgroup sizes and supported stage chains participate in shader identity.
Shader modules with equal SPIR-V and creation flags share a device-local content ID, including
inline modules. Descriptor set layouts are identified by bindings, binding flags, mutable type lists
and immutable-sampler identities. Pipeline layouts include those set-layout identities, layout flags
and push-constant ranges. Equal layout definitions can therefore reuse libraries across object handles.
Binding and push-constant array ordering is normalized where it does not change the definition.

Render passes reuse identities only for equal serialized definitions within the same creation API;
this does not implement the broader Vulkan render-pass compatibility rules. Known multiview,
input-aspect, separate stencil layout, depth/stencil resolve, shading-rate and synchronization2
structures are serialized by value. Unknown object extension chains retain a distinct identity for
each object lifetime. Samplers currently use content identity only without extension chains.
Destroyed handles lose their live mapping, so reusing a handle for different content cannot return
stale libraries. Colorless render-pass subpasses omit ignored blend state.

Content IDs are interned using complete serialized definitions, with string equality resolving hash
collisions. One copy of each unique definition, including supported shader SPIR-V, remains until
device destruction, alongside the library cache. Pipeline keys contain short IDs instead of copying
SPIR-V for every module-backed request. Application object handles, allocators and lifetimes are
preserved; only the internal libraries are shared.

Top-level extension headers are copied separately for each library. Payload arrays are borrowed only
during synchronous creation and serialized by value for cache lookup. They are not retained.

The cache synchronizes builds of each key so concurrent callers do not compile duplicate libraries.
The LRU cache retains at most 16,384 ready libraries per device, shared across all GPL subsets. Eviction drops cache ownership;
in-progress links and live executable pipelines retain shared references to their libraries.
An evicted library is destroyed only after its last user releases it, with the default allocator.
Executable pipelines use the application's allocator. Dependencies remain alive through downstream
executable destruction, including pure-GPL pipelines without private data. This follows the
[pipeline library lifetime rules](https://docs.vulkan.org/spec/latest/chapters/pipelines.html#pipelines-library).
A pipeline record contains only the promoted-state mask and original values. Active application
pipelines and concurrent builds can keep more than 16,384 libraries alive; the cap bounds extra cache
retention, not memory required by the application's live objects.

## Native fallback and failure handling

- Unknown chains, dynamic states or flags, capture and LTO requests stay native. Flags2 takes precedence over legacy flags, and supported flags are routed to their owning
  libraries without adding a Flags2 carrier to applications that did not use one.
- Batches containing derivative dependencies, library requests or compile-required control stay
  intact. Independent entries may convert separately while preserving successful handles, failure
  results and early-return behavior.
- On devices without `graphicsPipelineLibraryIndependentInterpolationDecoration`, pipelines with an
  active fragment shader stay native. Opaque shader modules cannot be checked for the extra GPL
  interpolation-matching requirement. Static discard and pipelines without a fragment shader remain
  eligible.
- Failed library creation, linking or private-data attachment falls back to the original monolithic
  request. Failed attachment destroys the linked executable with its matching allocator and releases
  the record before fallback. Only completed conversions count toward conversion statistics.
- If augmented device creation fails, the layer restores the consumed downstream layer link and
  loader output-slot contents, retries the original request once, and registers a successful retry
  as passthrough. A failed retry returns its own error.
- Device-generated commands disable automatic dynamization because they can bind pipelines without
  passing through the CPU bind hook. Pure GPL conversion remains available.
- Intel Arc 130T (`8086:7d51`) with Vulkan driver version `1661338` uses native creation for multiview
  with instance-rate or dynamic vertex input, for both rendering paths.
- On NVIDIA devices, pipelines that can rasterize lines combine vertex input and pre-rasterization
  into one cached library. This preserves static line width across static/dynamic pipeline switches.
  The combined key includes vertex-input state; both subsets'
  dynamic states and creation flags are retained. Fragment libraries remain separate, and conversion
  still uses GPL without LTO. Mesh pipelines continue to omit vertex-input state.

Conversion has no pipeline-count or library-sharing threshold. Eligible requests continue to use
GPL for the device lifetime, even when every request needs new shader libraries. Conversion and
expensive-library counts are diagnostic statistics only. Device capability checks and per-request
native fallback still apply. Cache eviction changes library residency without disabling conversion.

Single-pipeline shader-module-identifier requests use the same GPL partitioning as module-backed
requests. The application's pipeline cache is passed to every library build and link, including
`FAIL_ON_PIPELINE_COMPILE_REQUIRED` on all internal calls for speculative requests. This execution
control does not distinguish library keys. Identifier bytes do distinguish shader libraries from
module-backed libraries: libraries created with identifiers impose additional compile-control
requirements when linked. The driver's pipeline cache resolves their shader equivalence. Zero-size
identifiers are ignored. Failed builds are not retained in the library cache, so a compile-required
miss can be retried after the application primes its pipeline cache. Dependency/control batches
retain their original native handling.

Successful conversion reports whole-pipeline creation duration in feedback, clears unavailable
per-stage feedback, and does not claim an application pipeline-cache hit. Native fallback preserves
the driver's feedback.

## Implementation conventions

The layer follows the existing extension layers' namespace, dispatch-table and loader-export layout.
Promoted commands use core or extension entry points according to the application's API version.
It uses the same `vku::concurrent::unordered_map` registry as synchronization2, the shared enumeration
and assertion helpers, and `cast_utils.h` for pointer/integer Vulkan handles on both 32- and 64-bit
builds. Sources use the repository's clang-format configuration.

The library cache remains layer-specific because it coordinates in-progress builds and owns driver
pipelines. Extension-chain allowlists and lifetime generations protect conversion correctness;
capability checks and native fallback preserve valid application requests. File logging supplements
the shared logging facilities.

## Build

Use the repository's [build instructions](../BUILD.md). The layer is built and installed alongside the
existing layers and requires Vulkan 1.4 headers.

## Enable and diagnose

Point the loader at the directory containing the generated manifest and binary. For a Windows
multi-configuration build:

```bat
set VK_ADD_LAYER_PATH=C:\path\to\Vulkan-ExtensionLayer\build\layers\Debug
set VK_INSTANCE_LAYERS=VK_LAYER_PA_graphics_pipeline_library
```

For Linux or a single-configuration build:

```sh
export VK_ADD_LAYER_PATH=/path/to/Vulkan-ExtensionLayer/build/layers
export VK_INSTANCE_LAYERS=VK_LAYER_PA_graphics_pipeline_library
```

Alternatively enable the same layer name in `vkCreateInstance` or Vulkan Configurator.
Set `PA_GPL_LOG=2` for per-pipeline diagnostics and `PA_GPL_LOG_FILE=<path>` for file output. The
original `XIAOMI_GPL_LOG` and `XIAOMI_GPL_LOG_FILE` remain aliases; PA variables take precedence.
Default output reports the device tier and destruction-time statistics.

Set `PA_GPL_STATS=1` before launch for per-request cache snapshots. On Android,
`adb shell setprop debug.pa_gpl.stats 1` enables the same diagnostics on the next launch,
without a process wrapper. Each snapshot identifies the device and reports successful library
builds and cache hits separately for VI, PR, FS, FO and combined VI/PR, plus failed and in-progress
builds. Cache hits count reuse operations, not distinct reused objects. Converted PSO counts can
lag library lookups while other threads are building or linking. Detailed logging is disabled by
default; cache totals are also reported at device destruction. `resident` counts LRU entries,
`live`/`peak` count actual library handles (including executable dependencies), and `evicted` counts
removed cache entries, which need not destroy a handle immediately.
`resident_by_subset` and `evicted_by_subset` split those totals in VI/PR/FS/FO/combined-VI-PR order.
Build counts include recreating an evicted key; they are not counts of distinct state definitions.

### Android

Build the layer and its dependencies for arm64-v8a with the Android NDK and c++_static.
See the Android section of BUILD.md. Android loads the .so without a JSON manifest.
Default diagnostic output uses logcat with tag PA_GPL; file output remains supported.

Per-app layer settings persist across reboots. To disable GPU debug layers:

```sh
adb shell settings put global enable_gpu_debug_layers 0
```

To inspect diagnostics:

```sh
adb logcat -s PA_GPL
```

For per-app deployment and loading restrictions, see the
[Android layer guide](https://developer.android.com/ndk/guides/graphics/validation-layer).
