<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Unified ray-tracing source and retired prototype paths

All fallback implementation now lives under `layers/raytracing`. The top-level
`vkrt_layer` hierarchy and `layers/common/rt_fallback` are removed. The active
[goal](ray_tracing_fallback_goal.md) remains incomplete; this is an implementation
and source-maintenance checkpoint after `b9cf53b`.

| Previous location | Current location |
| --- | --- |
| `vkrt_layer/layer/src` | `layers/raytracing/src` |
| `vkrt_layer/layer/bvh` | `layers/raytracing/bvh` |
| `vkrt_layer/layer/traversal` | `layers/raytracing/traversal` |
| `vkrt_layer/tests` | `layers/raytracing/tests` |
| `layers/common/rt_fallback` | `layers/raytracing/bvh4` |
| Imported RQ design, IR and status | `layers/raytracing/docs` |

The move preserved all 160 relocated files byte-for-byte before path updates.
The canonical CMake entry is `layers/raytracing/CMakeLists.txt`. Both top-level
`BUILD_RAY_TRACING_FALLBACK` and compatibility `BUILD_RAY_QUERY_FALLBACK` select
that entry once, including when both are enabled. Compiler component tests link
the same typed compiler as the public AS/RQ/RTP layer. Win32 parent builds define
`NOMINMAX` for all compilers, fixing the Clang parent-build macro collision.

## Removed code

Dependency analysis identified the old HANYANG layer, direct-call/GLSL compiler,
private AS layout, D3D-derived builder/runtime and their private integration tests
as superseded by the canonical Mesa/BVH4 and typed-continuation path. Their
manifest and build target are removed. The obsolete CTS runner mode is removed
rather than redirected to a different implementation. Historical test records
retain their original identities and remain available with Git history.

The shared `CompileGLSL` declaration is extracted to `glsl_compiler.h`, and the
small memory helper used by continuation analysis is retained in its test fixture.
The live continuation-runtime adaptation and pinned upstream sources remain.
The canonical compiler no longer includes SPIRV-Cross headers.

Also removed are the unreachable RTP/query rejection wrappers, the fixture-only
old AS query-pool shim, fictional serialization command names, and two orphan
shader fixtures. The real public query entry now tests native passthrough.
CPU reference fixtures still used by registered tests and planned Mesa builder
variants remain. Fifteen audited worktree files (193,851 bytes before deletion)
were removed; function-level cleanup additionally removes the shadow entries.

## Verification

Fresh standalone and top-level builds succeed. The top-level configuration enables
both compatibility flags and builds the canonical layer target. The independent
BVH4 build passes all five host/GPU checks. The unified build passes 24 offline
checks and 29 GPU checks; the final standalone and parent DLLs each pass the same
three actual external CTS smoke cases. The Windows script harness confirms that
`-LE "^native_rt$"` preserves `no_native_rt` tests, optional native controls work,
and CTest failures propagate to the caller.

The expanded external RT matrix and shader-binary interface outcomes are recorded
in the [query/control-flow evidence](rt_query_controlflow_2026-10-26.md) and
[binary interface report](rt_shader_objects_2026-10-26.md). All cases use the
unmodified external `D:/Codes/VK-GL-CTS` executable and resource archive with
Khronos validation; skips and failures remain separate from passes.

The 226-case RT matrix records 224 passes and two matching native skips, with
zero failures, crashes, timeouts or stdout validation diagnostics. The separate
binary-interface comparison records two software passes/two failures against
four native passes. The last rebuilt DLL repeats three RT smoke passes and the
same two binary-interface passes/two failures. These remain known implementation
gaps and are not hidden by the successful RT selection.

Raw move/deletion hashes, build and CTest logs are under `build-source-layout/`.
The large RT run snapshots the DLL before the parent-build `NOMINMAX` adjustment;
the final standalone and parent DLL smoke runs identify their separate hashes.
Subsequent optional custom-resolve header guards and the same-value result alias
preserve the current SDK behavior; the final seven-case delivery check and
three focused host/GPU checks follow their rebuild. These header compatibility
checks do not constitute a Linux runtime qualification.
No result from the earlier startup crashes is replaced by a later passing run.
