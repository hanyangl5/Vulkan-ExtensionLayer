# Software ray tracing layer

`VK_LAYER_VKRT_SOFTWARE_RAY_QUERY` is the canonical implementation of the active
[software AS/Ray Query/RT Pipeline goal](../../docs/ray_tracing_fallback_goal.md).
Its existing layer name is retained for application and test compatibility. AS,
Ray Query, and RT Pipeline share the layer's object model, GPU builder, portable
BVH4 representation, AS address resolver, and resumable traversal core.

The complete mandatory Vulkan RT contract remains unfinished. Public API tests
and selected external Khronos CTS cases have native controls and validation
records for specific revisions. These results do not establish full conformance
or qualification on physical GPUs without native RT. See the
[implementation status](../../docs/ray_tracing_fallback_status.md) and
[baseline review](../../docs/ray_tracing_fallback_review.md).

## Source layout

| Directory | Purpose |
| --- | --- |
| `src/` | Production C/C++: Vulkan layer, typed compiler/runtime, and shared CPU/shader ABI headers |
| `shaders/` | Production GPU construction, BVH4 encoding, traversal, AS operations, and typed runtime GLSL |
| `tests/` | Test programs/shaders, CPU reference oracles, GPU test helpers, and diagnostics |
| `docs/` | Design, ABI and provenance records; pinned D3D12/DXC references live in `docs/upstream/` |
| `cmake/` | Build modules, generated-header templates, layer manifest/export files, and build helpers |

The layer root contains only this README and the canonical CMake entry.
BVH4 component checks use the same entry with
`-DVKRT_BVH4_COMPONENTS_ONLY=ON`; `-DVEL_RT_BVH4_GPU_TESTS=ON` adds their GPU checks.

## Build

The standalone build requires no Vulkan-Samples or private Mesa checkout. Use:

- A 64-bit C11/C++17 compiler, CMake 3.22.1 or newer, Ninja, and Python 3.
- Vulkan headers, loader, and the matching `vk.xml` registry. Set
  `VKRT_VULKAN_REGISTRY` if the registry cannot be found in the SDK or system
  installation. Vulkan-Utility-Libraries is not required for this build.
- `glslc`, `spirv-val`, SPIRV-Headers, SPIRV-Tools development libraries including
  optimizer and linker components, and glslang development libraries. A full
  Vulkan SDK supplies these; a loader-only installation does not.

From the repository root, in an initialized x64 compiler terminal on Windows:

```sh
cmake -S layers/raytracing -B build-raytracing -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-raytracing --parallel
ctest --test-dir build-raytracing -L offline --output-on-failure
```

Release builds avoid mixing debug and release runtimes with SDK import libraries.
Builder and traversal SPIR-V are compiled, validated, and embedded during the
build. Traversal library function names are used by the linker; stripping those
`OpName` records requires a corresponding linker change.

For a configured top-level VEL build, `BUILD_RAY_TRACING_FALLBACK=ON` enables this
same target. `BUILD_RAY_QUERY_FALLBACK` is a compatibility alias. Both options
together still build the canonical target once.

## Run and test

The canonical manifest and library are staged in the build's `layers` directory.
For an individual application:

```sh
export VK_LAYER_PATH="$PWD/build-raytracing/layers"
export VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY
./your-application
```

Windows PowerShell:

```powershell
$env:VK_LAYER_PATH="$PWD/build-raytracing/layers"
$env:VK_INSTANCE_LAYERS='VK_LAYER_VKRT_SOFTWARE_RAY_QUERY'
./your-application.exe
```

CTest selects the software or native environment for each registered case:

```sh
ctest --test-dir build-raytracing --print-labels
ctest --test-dir build-raytracing -L offline --output-on-failure
ctest --test-dir build-raytracing -L gpu --output-on-failure
ctest --test-dir build-raytracing -L native_rt --output-on-failure
```

The `no_native_rt` label includes host and compute helper checks; passing that
label does not prove the complete layer on a physical no-RT device. Native
controls require the corresponding native RT features. A skipped case is not a
passing result. GPU execution requires the backend's actual compute, subgroup,
BDA, integer/storage, scalar-layout, memory-model, and descriptor prerequisites.
Physical no-RT and cross-vendor qualification remain open.

External CTS is part of development validation. The repository's
[`run_rt_cts.py`](../../scripts/run_rt_cts.py) records exact cases, executable and
layer hashes, QPA results, and failures. Use the case lists and commands in the
linked records to reproduce a slice. Local fixtures and selected CTS slices serve
different purposes; neither completes the full goal by itself.

## Current implementation and evidence

The GPU path builds triangle/AABB BLAS and instance TLAS through Mesa construction
IR and encodes the shared BVH4 runtime. It supports semantic UPDATE through
reconstruction, GPU CLONE/COMPACT, serialization and relocation, and bounded
public AS property-query publication. COMPACT retains the same representation and
size; UPDATE is not a refit optimization. The
[AS address contract](docs/AS_ADDRESS_ABI.md) distinguishes public values,
physical roots, and publication lifetime.

Ray Query lowers supported typed SPIR-V operations to traversal in the consuming
shader stage. Canonical RTP links all six RT stages into a compute scheduler with
GPU SBT selection, nested traces and callables, invocation-private continuation
storage, descriptor-backed opaque provenance, and cached native stack variants.
The adapter restores application compute pipeline, descriptor, push, and
shader-object state after internal work. Shader-object binaries retain source,
specialization and complete linked-group state. Ordinary groups transport their
native binary; RQ groups reconstruct owned descriptor/sampler/push interfaces
against the destination resolver, then use native BINARY creation. Selected
null/different-layout, partial-batch and cross-device imports have external CTS
and actual GPU evidence; complete optional-feature compatibility remains open.

Evidence records retain the tested revision, device, commands, failures, and
scope:

- [Shared typed runtime and initial external CTS](../../docs/rt_cts_typed_runtime_2026-10-26.md).
- [Opaque resource provenance across suspension](../../docs/rt_opaque_provenance_2026-10-26.md).
- [External control flow, private state, nonuniform arguments, and barriers](../../docs/rt_cts_controlflow_2026-10-26.md).
- [Public AS query publication](../../docs/rt_query_publication_2026-10-26.md) and
  [external query/control-flow results](../../docs/rt_query_controlflow_2026-10-26.md).
- [Pipeline creation hints and derivatives](../../docs/rt_pipeline_creation_2026-10-26.md).
- [Resource profile and boundary checks](../../docs/rt_resource_profile_2026-10-26.md).
- [Shader-object source and binary interoperability](../../docs/rt_shader_objects_2026-10-26.md).

Full pointer provenance, complete shader/API/resource and synchronization
coverage, advertised-limit qualification, larger builder variants, and physical
no-RT qualification remain work under the active goal. Unsupported optional
features retain explicit boundaries. Evidence records distinguish failures,
unsupported cases, and startup crashes from passes; an isolated successful
recheck does not replace the original run's result.

## Source provenance

Source files retain their original copyright, license, and provenance notices.
The Mesa-derived builder files carry their own notices; the original reference
revision and construction interface are recorded in
[`docs/IR_CONTRACT_V2.md`](docs/IR_CONTRACT_V2.md). Imported milestones and test
results remain in the [historical status](docs/imported_ray_query_status.md) and
[original design](docs/rayquery_software_rt_layer_design.md); they describe their
original revisions.

The traversal source also records its stated Arm Avalon DDK origin, including
passages described as retained verbatim. Those source notices are preserved by
this import; a file's SPDX tag alone does not establish the redistribution rights
of the referenced original DDK material. Confirm the applicable rights before
redistributing material whose original authorization has not been established.
