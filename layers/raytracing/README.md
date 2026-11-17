# Software ray tracing layer

`VK_LAYER_PA_RTP` is the canonical implementation of the
[software AS + RT Pipeline goal](../../docs/ray_tracing_fallback_goal.md).
It was named `VK_LAYER_VKRT_SOFTWARE_RAY_QUERY` before 2026-11-17; Ray Query
is not part of the delivered contract. AS and RT Pipeline share the layer's object model, GPU builder, portable BVH4 representation, AS
address resolver, and resumable traversal core.

The complete mandatory Vulkan RT contract remains unfinished. Public API tests
and selected external Khronos CTS cases have native controls and validation
records for specific revisions. These results do not establish full conformance
or qualification on physical GPUs without native RT. See the
[overview](../../docs/rtp_overview.md).

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
- `glslc`, `spirv-val`, `spirv-link`, SPIRV-Headers, SPIRV-Tools development libraries including
  optimizer and linker components, and glslang development libraries. A full
  Vulkan SDK supplies these; a loader-only installation does not.

The local checkpoint uses Vulkan-Headers v1.4.366 and its matching registry,
with compiler/SPIR-V libraries from Vulkan SDK 1.4.357.0. CI pins that same
header/registry revision. Older SDK headers lack sizes for newer native feature
structures and can reject valid chain ordering during software RT translation.
Select matching headers and registry together; the private shader target remains
Vulkan 1.1 with SPIR-V 1.4. The CI configuration has local header/generator
probes, while the actual Ubuntu build remains unqualified.

For a separate Vulkan-Headers checkout, add
`-DVulkan_INCLUDE_DIR=<checkout>/include` and
`-DVKRT_VULKAN_REGISTRY=<checkout>/registry/vk.xml` to the configure command.

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
export VK_INSTANCE_LAYERS=VK_LAYER_PA_RTP
./your-application
```

Windows PowerShell:

```powershell
$env:VK_LAYER_PATH="$PWD/build-raytracing/layers"
$env:VK_INSTANCE_LAYERS='VK_LAYER_PA_RTP'
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

The software discovery candidate reports base AS/RQ/RTP from its compute and
resource prerequisites, independently of native RT advertisement. Applications
must request Vulkan 1.1 or later; the backend currently requires native Vulkan
1.2 and available private dependencies. Private shaders use genuine SPIR-V 1.4.
Complete queue/group/protected and resource qualification remains open. Newer
optional object interfaces may be explicitly excluded, and every test need not
pass.
The normal query backend uses per-execution native Events and accepts compute
families without timestamps, without privately requiring hostQueryReset.
The no-Event portability fallback has explicit timestamp prerequisites.

External CTS is part of development validation. The repository's
[`run_rt_cts.py`](../../scripts/run_rt_cts.py) records exact cases, executable and
layer hashes, QPA results, and failures. Case lists are in
[`scripts/rt_cts_cases`](../../scripts/rt_cts_cases). Local fixtures and selected CTS slices serve
different purposes; neither completes the full goal by itself.

## Current implementation and evidence

This layer delivers `VK_KHR_acceleration_structure` and
`VK_KHR_ray_tracing_pipeline` only. `VK_KHR_ray_query` is not advertised, not
lowered, and rejected at shader-module creation; Ray Query only fixtures and CTS
selections were removed with the scope pivot.

The GPU path builds triangle/AABB BLAS and instance TLAS through Mesa construction
IR and encodes the shared BVH4 runtime. UPDATE refreshes leaf records and refits
bounds while retaining the runtime topology. It also supports GPU CLONE/COMPACT,
serialization and relocation, and bounded public AS property-query publication.
Non-null zero-mask and empty-BLAS instances retain recoverable leaves. The
[AS address contract](docs/AS_ADDRESS_ABI.md) distinguishes public values,
physical roots, and publication lifetime.

Ray tracing pipelines link all six RT stages into a compute scheduler with GPU
SBT selection, nested traces and callables, invocation-private continuation
storage in execution-owned BDA arenas, descriptor-backed opaque provenance, and
one cached native variant per pipeline with execution-time stack budgets. The
software traversal is embedded as GLSL (`shaders/sw_traversal.glsl`) and
compiled into each generated stage. The adapter restores application compute
pipeline, descriptor, push and shader-object state after internal work.

Dated evidence records (Oct/Nov 2026) are summarized in the
[overview](../../docs/rtp_overview.md) and kept in Git history (last at `6bc7c24`).

Logical StorageBuffer pointer Phi/Select and inlined helper values use typed
route/selector snapshots across suspension. RT pipeline libraries retain typed
definitions and link through the same compute-backed runtime. Full pointer
provenance, complete shader/API/resource and synchronization coverage,
advertised-limit qualification, larger builder variants, pipeline compile cost
and physical no-RT qualification remain work under the active goal. Unsupported
optional features retain explicit boundaries.
## Source provenance

Source files retain their original copyright, license, and provenance notices.
The Mesa-derived builder files carry their own notices; the original reference
revision and construction interface are recorded in
[`docs/IR_CONTRACT_V2.md`](docs/IR_CONTRACT_V2.md). Imported Ray Query
milestones are retained in git history only; they describe a superseded revision
and are not part of this contract.

The traversal source also records its stated Arm Avalon DDK origin, including
passages described as retained verbatim. Those source notices are preserved by
this import; a file's SPDX tag alone does not establish the redistribution rights
of the referenced original DDK material. Confirm the applicable rights before
redistributing material whose original authorization has not been established.
