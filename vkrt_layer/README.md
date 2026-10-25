# Software Ray Query layer

`VK_LAYER_VKRT_SOFTWARE_RAY_QUERY` replaces acceleration-structure builds and supported `VK_KHR_ray_query` shader instructions with a GPU LBVH builder and software traversal. Applications continue to use the Vulkan AS and Ray Query APIs. The layer rewrites AS descriptors to storage buffers and lowers Ray Query instructions to functions inside the application's shader module.

This is a bootstrap implementation, not a complete Vulkan RT implementation. It still relies on the underlying driver exposing the native RT extensions and accepting the application's RT feature requests. Running on a device without native RT requires additional capability and device-creation virtualization that is not implemented here.

## Relationship to the RT pipeline fallback

This layer and [`VK_LAYER_HANYANG_ray_tracing_fallback`](../docs/ray_tracing_fallback_layer.md) are separate experimental implementations. This directory implements inline Ray Query traversal; the other layer implements a compute-based RT pipeline path. They use different acceleration-structure formats and object tracking.

**Enable only one of these two layers at a time. Their AS handles, device addresses, backing buffers, and serialized data are not interchangeable.** They cannot be stacked to combine their supported features.

## Build

The layer builds independently of Vulkan-Samples and does not require a private Mesa checkout. The required builder sources and BVHIR declarations are included under `layer/bvh/`; the original Mesa reference revision is recorded in [the IR contract](IR_CONTRACT_V2.md). No `vkrt_layer/mesa` submodule is required.

Required build tools and libraries:

- CMake, Ninja, Python 3, and a C11/C++17 compiler. On Windows, use a terminal with the x64 compiler environment initialized.
- Vulkan headers and loader, plus the matching `vk.xml` registry used to generate the `pNext` size table. Set `VKRT_VULKAN_REGISTRY` if CMake cannot find the registry in the SDK or system installation; Vulkan-Utility-Libraries is not required for the standalone build.
- `glslc` and `spirv-val`.
- SPIRV-Headers and SPIRV-Tools development headers/libraries, including the linker and optimizer components. A full Vulkan SDK supplies these dependencies; a loader-only installation does not.

From the repository root:

```sh
cmake -S vkrt_layer/layer -B build/vkrt_layer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/vkrt_layer --parallel
ctest --test-dir build/vkrt_layer -L offline --output-on-failure
```

Alternatively, enable `-DBUILD_RAY_QUERY_FALLBACK=ON` in a configured top-level VEL build. The standalone build avoids the dependencies of unrelated VEL layers.

Builder and traversal SPIR-V are compiled, validated, and embedded in the layer. Keep their debug names: the current linker identifies traversal functions by name, so adding an optimization pass that strips `OpName` is not supported.

## Run and test

The generated layer manifest is staged under the build's `layers` directory. For a standalone build, enable it for one application as follows:

```sh
export VK_LAYER_PATH="$PWD/build/vkrt_layer/layers"
export VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY
./your-application
```

Windows PowerShell:

```powershell
$env:VK_LAYER_PATH="$PWD/build/vkrt_layer/layers"
$env:VK_INSTANCE_LAYERS='VK_LAYER_VKRT_SOFTWARE_RAY_QUERY'
./your-application.exe
```

Disable other RT fallback layers before running. The GPU tests currently require native RT capability exposure and the software backend's real prerequisites: BDA, scalar block layout, shader int8/int16/int64, buffer 8/16-bit access, and the Vulkan memory model with device scope.

CTest defines seven tests: four offline tests and three GPU tests. The `cpu_fixture` test uploads a CPU-built fixture but still executes a shader on the GPU. The native GPU test disables this layer; the software GPU tests enable it through their CTest environment.

```sh
# All tests, including software and native GPU execution:
ctest --test-dir build/vkrt_layer --output-on-failure

# Host/IR/SPIR-V checks only:
ctest --test-dir build/vkrt_layer -L offline --output-on-failure
```

On Windows, `vkrt_layer\run-tests.bat` configures and builds the standalone layer and runs CTest. It stops and returns a nonzero exit code if a step fails. An optional external Vulkan-Samples executable and asset working directory can be supplied:

```bat
vkrt_layer\run-tests.bat
vkrt_layer\run-tests.bat "C:\Vulkan-Samples\build\app\bin\relwithdebinfo\AMD64\vulkan_samples.exe" "C:\Vulkan-Samples"
```

The optional sample launches `sample ray_queries` after the tests. It is interactive and has no frame limit supplied by this script. You can also supply the executable through `VKRT_SAMPLE_EXE` and its working directory through `VKRT_SAMPLE_DIR`.

## Implemented path and limits

- GPU BUILD of triangle/AABB BLAS and instance TLAS into the shared BVHIR v2 container, including instance transforms, masks, and geometry/primitive/instance IDs.
- Resumable software candidate/committed traversal, triangle confirmation, generated AABB intersections, and the getters currently mapped by `vkrt_spirv_lower.cpp`.
- AS descriptor/layout/pool translation and compute build-state restoration; AS dependencies are extended for Barrier/Barrier2 and Submit/Submit2.
- The shader lowering currently binds one TLAS storage-buffer binding and discards the AS value passed to `RayQueryInitialize`. Multiple AS bindings, shader-side AS descriptor arrays/indexing, and arbitrary AS value provenance are not supported.
- RT pipeline shaders, SBT dispatch, UPDATE/refit, AS copy/compaction/serialization, indirect builds, micromaps, and motion geometry are not implemented. PLOC/HPLOC sources are not active runtime backends.
- AABB opacity handling does not yet apply all ray/instance overrides. Some unsupported commands only log an error; unsupported host AS paths and event-based AS synchronization are not fully intercepted. Do not treat an unimplemented API as a supported no-op.
- Builds require a compute-capable queue outside a render pass. Node-address and dispatch limits constrain build size. Multi-queue ownership and cross-vendor execution have not been comprehensively validated.

See [STATUS.md](STATUS.md) for the original test records and known issues, [IR_CONTRACT_V2.md](IR_CONTRACT_V2.md) for the current data layout, and [the original design](rayquery_software_rt_layer_design.md) for the implementation milestones. Historical test results are retained as evidence for their original revisions; importing the patch into VEL is not a new GPU validation result.

## Source provenance

Source files retain their original copyright, license, and provenance notices. The Mesa-derived builder files carry their own notices. The traversal source also records its stated Arm Avalon DDK origin, including passages described as retained verbatim. Those source notices are preserved by this import; a file's SPDX tag alone does not establish the redistribution rights of the referenced original DDK material. Confirm the applicable rights before redistributing material whose original authorization has not been established.
