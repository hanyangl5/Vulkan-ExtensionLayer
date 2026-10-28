<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Ray tracing directory consolidation, 2026-10-27

The implementation remains in `layers/raytracing`. Its root now contains only
`CMakeLists.txt`, `README.md` and five directories:

| Directory | Contents |
| --- | --- |
| `src` | Production C/C++, typed compiler/runtime and shared CPU/shader ABI headers |
| `shaders` | Production GPU construction, encoding, traversal and runtime shaders |
| `tests` | Fixtures, test shaders, independent CPU oracles and diagnostic tools |
| `cmake` | Build modules, templates, manifest/export definitions and build helpers |
| `docs` | Design and provenance records, including pinned upstream references |

The former `bvh`, `bvh4` and `traversal` trees are merged according to these
roles. The former `upstream` tree is under `docs/upstream`; it is reference
material and is not compiled into the layer. Test-only CPU BVH code and helper
headers are under `tests`. No `vkrt_layer` directory is introduced.

All 280 moved files matched their original bytes immediately after relocation.
Subsequent changes update include paths, build dependencies, CI and documentation.
The 188 pinned source blobs also match their manifest after staging, including
original line endings. Their `.gitattributes` entries follow the new location.

The canonical CMake entry supports both the complete layer and the former
standalone BVH4 checks. Component-only configuration now uses:

```sh
cmake -S layers/raytracing -B build-bvh4 -DVKRT_BVH4_COMPONENTS_ONLY=ON
cmake --build build-bvh4 --parallel
ctest --test-dir build-bvh4 --output-on-failure
```

Add `-DVEL_RT_BVH4_GPU_TESTS=ON` to include the GPU component checks. Generated
binary names, staged layer name and application-facing interfaces are retained.

## Executed verification

The canonical layer, the parent-project layer with selected public fixtures,
and a fresh component-only configuration all build successfully using Clang and
Vulkan SDK 1.4.357.0 on Windows. After consolidation:

| Check | Result |
| --- | --- |
| Canonical offline tests | 27/27 pass |
| Independent BVH4/address components, including three GPU tests | 5/5 pass |
| Public software RTP, shader binary, resource boundary and 1,024-AS pool pressure | 4/4 pass |
| External Vulkan CTS regression slice with SDK validation | 16/16 pass |

The external run uses the existing `D:/Codes/VK-GL-CTS` executable at revision
`d18e4c68543103371715845cd914c71e5859963e` on NVIDIA GeForce RTX 3080. Its exact
[case list](rt_cts_directory_merge_cases.txt) includes RQ, AS compaction and
serialization, RT pipeline control flow/callables, shader binary interfaces,
and four cases that crashed in the preceding 226-case run. The runner uses a
120-second outer/total timeout and a 30-second watchdog touch interval.
No `Validation Error`, `VUID-` or `SYNC-HAZARD` diagnostic appears in its stdout
or QPA files. The frozen layer SHA-256 is
`ed5589534291e35465c44bc4c2fb96342ba1d6b72e5a2b9476700d0358664cb5`.

The preceding run remains **220 pass, 2 NotSupported, 4 crash**: three early
`0xc0000008` exits and a separate internal watchdog abort. Passing these cases
once after consolidation does not identify or resolve those failure causes.
Full mandatory RT behavior and physical no-RT qualification remain open.

The [machine-readable record](rt_directory_layout_2026-10-27.json) binds each
case result and local evidence log to its hash. Raw logs, QPA files and the
frozen layer remain under `build-rt-cts-evidence/directory-merge-rt`; build/test
logs and the relocation map remain under `build-source-layout`.
