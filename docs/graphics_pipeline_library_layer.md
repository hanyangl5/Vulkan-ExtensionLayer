# VK_LAYER_PA_graphics_pipeline_library

This layer transparently accelerates graphics pipeline creation on drivers supporting
`VK_EXT_graphics_pipeline_library` (GPL). It converts eligible monolithic pipelines into cached
vertex-input, pre-rasterization, fragment-shader and fragment-output libraries, then links them
without LTO. It exposes no additional Vulkan extensions to applications.

The implementation and original host tests came from `xiaomi-vulkan-graphics-pipeline-library-layer`
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
Libraries use the default allocator and live until device destruction; executable pipelines use the
application's allocator. A pipeline record contains only the promoted-state mask and original values.

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
  into one cached library. This preserves static line width across static/dynamic pipeline switches
  (reproduced on RTX 3080, driver 596.49). The combined key includes vertex-input state; both subsets'
  dynamic states and creation flags are retained. Fragment libraries remain separate, and conversion
  still uses GPL without LTO. Mesh pipelines continue to omit vertex-input state.

Conversion has no pipeline-count or library-sharing threshold. Eligible requests continue to use
GPL for the device lifetime, even when every request needs new shader libraries. Conversion and
expensive-library counts are diagnostic statistics only. Device capability checks and per-request
native fallback still apply; cached libraries remain owned until device destruction.

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
the shared logging facilities. Host tests share explicit helpers rather than including another test
program or redefining its entry point.

## Build and test

Use the repository's [build instructions](../BUILD.md). The layer is built and installed alongside the
existing layers and requires Vulkan 1.4 headers. Host tests run with `BUILD_TESTS=ON`, or independently
of GoogleTest/SPIR-V test dependencies with:

```sh
cmake -S . -B build -DUPDATE_DEPS=ON -DBUILD_TESTS=OFF -DBUILD_GPL_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
ctest --test-dir build -C Debug -R graphics_pipeline_library --output-on-failure
```

The two host suites cover dynamic-state routing, ignored arrays, rendering cache keys, Flags2,
inline shaders, handle reuse, discard, feedback and batch semantics. A 1,024-pipeline regression uses
unique VS/FS identities to verify continuous conversion without shader-library sharing. Content
identity tests cover equal shaders, layouts and render passes, immutable samplers, inline modules,
changed definitions after handle recycling, unknown chains and creation failures. Failure-path
regressions cover private-data attachment and native error propagation, all five bind-time restored
values, consumed loader links and output slots on device retry, and independent-interpolation fallback.

Desktop validation uses Clang with `BUILD_WERROR=ON`. A GPU smoke probe on an NVIDIA GeForce RTX 3080
creates, binds and submits four state variants with validation enabled; the expected summary is
`converted=4`, `expensive_libraries=2`. This probe does not compare rendered images,
and the migrated PA layer has not undergone a complete CTS run here.

A targeted CTS regression run on RTX 3080 with driver 596.49 covers 3,042 cases across 56 groups,
including dynamic rendering, multiview and mesh shaders. The 26 image failures found before the
VI/PR and fragment-state fixes now pass while still converting through GPL. Their validation run
also passes; a separate 141-case validation set reports 133 passes and 8 unsupported cases.
The original selected suite retained four failures also seen without the layer and one
shader-module-identifier pipeline-cache quality warning. After routing identifier requests through
GPL, all 64 selected shader-module-identifier cases pass with validation enabled; the original
`hlsl_tessellation` warning no longer occurs. These results are not a complete CTS conformance claim.

After removing the conversion gate, the three previously downgraded groups (`cache`, `depth`,
`extended_dynamic_state`) were rerun with validation enabled: 578 passes, 30 unsupported cases and
no failures across 608 cases. They converted 1,564 PSOs, including requests past the former threshold.

Content-based cross-object reuse was checked against the same selected 3,042 cases with validation:
all case statuses and validation VUID sets remain unchanged. PR/FS library creation decreased from
8,502 to 2,832 for the same 4,292 converted pipelines. This is a library-count comparison rather
than a controlled compilation-time benchmark; see the [content reuse report](testing/gpl-cts-2026-10-20/content-reuse.md).

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

### Android

Build the layer and its dependencies for arm64-v8a with the Android NDK and c++_static.
See the Android section of BUILD.md. Android loads the .so without a JSON manifest.
Default diagnostic output uses logcat with tag PA_GPL; file output remains supported.

On 2026-10-20, the connected ARM64 Android device (0182200148488540, API 37) loaded
libVkLayer_PA_graphics_pipeline_library.so from the debuggable Vulkan Samples app data directory.
The per-app GPU debug configuration selects com.khronos.vulkan_samples and
VK_LAYER_PA_graphics_pipeline_library. The original settings were saved locally in
build-android/gpu-settings-before.txt before deployment.

The hello_triangle sample completed 60 frames with tier=dynamize, converted=1 and
expensive_libraries=2. Logs are saved locally in build-android/device-layer-log.txt.
The device reports an NVIDIA GeForce RTX 5090 Vulkan driver, GPL support and
FastLinking=false. This verifies loading and conversion, not mobile GPU performance.

Per-app layer settings persist across reboots. To disable this deployment:

```sh
adb shell settings put global enable_gpu_debug_layers 0
```

To inspect diagnostics:

```sh
adb logcat -s PA_GPL
```

For per-app deployment and loading restrictions, see the
[Android layer guide](https://developer.android.com/ndk/guides/graphics/validation-layer).