# Software Ray Query layer

The active project target is [complete software AS/RQ/RTP fallback](../docs/ray_tracing_fallback_goal.md), with a shared Mesa-build/portable-BVH4 backend. This page describes the current RQ prototype. Its production builder and traversal now use the shared BVH4 ABI; unified RQ/RTP objects and complete capability virtualization remain unfinished. See the [baseline audit](../docs/ray_tracing_fallback_review.md).

`VK_LAYER_VKRT_SOFTWARE_RAY_QUERY` replaces acceleration-structure builds and supported `VK_KHR_ray_query` shader instructions with a GPU LBVH builder and software traversal. Applications continue to use the Vulkan AS and Ray Query APIs. The layer rewrites AS descriptors to storage buffers and lowers Ray Query instructions to functions inside the application's shader module.

This is a bootstrap implementation, not a complete Vulkan RT implementation. It consumes emulated AS/RQ device extensions/features before downstream device creation and translates supported resource/synchronization requirements. Base extension/feature discovery still relies on native RT exposure; complete no-RT discovery, limits and mandatory APIs are not implemented or qualified yet.

## Relationship to the RT pipeline fallback

This layer and [`VK_LAYER_HANYANG_ray_tracing_fallback`](../docs/ray_tracing_fallback_layer.md) are separate experimental implementations. This directory implements inline Ray Query traversal; the other layer implements a compute-based RT pipeline path. RQ uses the shared portable BVH4 runtime format; RTP still uses its earlier runtime representation and separate object tracking.

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

Disable other RT fallback layers before running. The GPU tests currently require native RT capability exposure and the software backend's real prerequisites: BDA, scalar block layout, shader int8/int16/int64, buffer 8/16-bit access, the Vulkan memory model with device scope, SSBO dynamic/nonuniform indexing, variablePointersStorageBuffer and a compatible HOST_VISIBLE | HOST_COHERENT memory type for private resolver buffers. The [AS address ABI](../layers/common/rt_fallback/AS_ADDRESS_ABI.md) explains public values, physical roots and publication lifetime.

CTest currently defines fourteen offline tests and eight GPU tests. The `cpu_fixture` test uploads a CPU-built fixture but still executes a shader on the GPU. Native GPU tests disable this layer; software AS tests enable it through their CTest environment. `gpu_build.aligned_values` checks legal AS suballocations and GENERIC values. `as_copy.software` builds, clones and compacts unmapped device-local BLAS/TLAS storage, including copies beyond the first dispatch stride. The `no_native_rt` traversal test executes production helpers on ordinary compute without enabling native RT or the fallback layer; it validates those helpers, not a complete no-RT AS/layer path.

```sh
# All tests, including software and native GPU execution:
ctest --test-dir build/vkrt_layer --output-on-failure

# Host/IR/SPIR-V checks only:
ctest --test-dir build/vkrt_layer -L offline --output-on-failure

# Production opacity/SBT helpers on a compute device, with no native RT enabled:
ctest --test-dir build/vkrt_layer -L no_native_rt --output-on-failure
```

On Windows, `vkrt_layer\run-tests.bat` configures and builds the standalone layer and runs CTest. It stops and returns a nonzero exit code if a step fails. An optional external Vulkan-Samples executable and asset working directory can be supplied:

```bat
vkrt_layer\run-tests.bat
vkrt_layer\run-tests.bat "C:\Vulkan-Samples\build\app\bin\relwithdebinfo\AMD64\vulkan_samples.exe" "C:\Vulkan-Samples"
```

The optional sample launches `sample ray_queries` after the tests. It is interactive and has no frame limit supplied by this script. You can also supply the executable through `VKRT_SAMPLE_EXE` and its working directory through `VKRT_SAMPLE_DIR`.

## Implemented path and limits

- GPU BUILD of triangle/AABB BLAS and instance TLAS through Mesa construction IR in application scratch, then direct GPU encoding to shared BVH4. Final AS storage contains only BVH4 header, nodes, leaves and parents; it preserves original IDs, full geometry flags, transforms and runtime BLAS addresses.
- GPU CLONE/COMPACT reads execution-time BVH4 total_size, publishes the destination, copies with 64-bit strided addressing and restores application command state. COMPACT retains the same representation/size. Synchronous deferred operations are layer-owned and need no native RT extension.
- GPU UPDATE reconstructs through the shared builder, preserving in-place AS addresses and out-of-place source storage. The update scratch requirement covers full reconstruction; this is semantic UPDATE support rather than a refit optimization.
- GPU serialization/deserialization uses execution-time BVH4 sizes, exact driver/format UUIDs and relocatable public BLAS references. ABI 2 retains public and physical BLAS values separately; deserialization accepts created, not-yet-built replacement BLAS objects. Complete public AS property-query pools remain open, so CTS compaction/serialization cases still fail before reaching these operations.
- Resumable software candidate/committed traversal, triangle confirmation, generated AABB intersections, and the getters currently mapped by `vkrt_spirv_lower.cpp`, including the base instance SBT-offset getter with candidate/committed selection.
- AS descriptor/layout/pool translation and compute build-state restoration; dependencies translate barriers, submit, events, timestamps and render-pass masks.
- Constant-space parent traversal visits all four child slots, preserves TLAS/BLAS cursors across candidate suspension, and has no fixed-stack overflow truncation. GPU regression covers depth140 and Confirm-driven pruning.
- Value-preserving AS lowering reads lifetime-owned address cells, retains scalar/array selection and integer conversion, and stores a separate root in each query. Supported helper/Phi and NonUniform paths have compile/CTS evidence. Each cell still consumes an SSBO descriptor and an allocation; full layout/budget transparency is not proven.
- Canonical RTP now connects typed SPIR-V stages to the same BVH4 builder/traverser and AS resolver, with GPU SBT selection, all six stages, nested traces, callables and invocation-private continuation storage. Public API execution and selected external CTS have evidence; full shader/resource/API coverage remains incomplete. refit performance, complete AS property queries, indirect builds, micromaps and motion geometry remain open. PLOC/HPLOC sources are not active runtime backends.
- AABB opacity applies ray/instance/geometry precedence through the shared opacity helper. Unsupported command stubs mark recording failed; unsupported AS query pools are rejected before downstream creation. Optional host AS paths are not fully intercepted.
- Builds require a compute-capable queue outside a render pass. Node-address and dispatch limits constrain build size. Multi-queue ownership and cross-vendor execution have not been comprehensively validated.

See [STATUS.md](STATUS.md) for the original test records and known issues, [IR_CONTRACT_V2.md](IR_CONTRACT_V2.md) for construction IR, [the shared ABI](../layers/common/rt_fallback/bvh4_abi.h) for runtime layout, and [the original design](rayquery_software_rt_layer_design.md) for the implementation milestones. Historical test results are retained as evidence for their original revisions; importing the patch into VEL is not a new GPU validation result.

Actual external CTS on RTX3080 now exercises the canonical shared backend with Khronos validation: [20/20 RTP and 54/56 RQ/AS](../docs/rt_cts_typed_runtime_2026-10-26.md), with the two public AS query-pool failures retained. This is a selected development matrix, not full conformance or physical no-RT qualification.

## Source provenance

Source files retain their original copyright, license, and provenance notices. The Mesa-derived builder files carry their own notices. The traversal source also records its stated Arm Avalon DDK origin, including passages described as retained verbatim. Those source notices are preserved by this import; a file's SPDX tag alone does not establish the redistribution rights of the referenced original DDK material. Confirm the applicable rights before redistributing material whose original authorization has not been established.
