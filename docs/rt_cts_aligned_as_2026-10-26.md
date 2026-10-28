<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Aligned public AS values checkpoint - 2026-10-26

Production RQ now resolves opaque 256-byte-aligned AS values into physical BVH4
roots, including GENERIC objects and legal 16-byte buffer suballocations.
**46 selected real Vulkan CTS cases pass with Khronos validation.** The full
AS/RQ/RTP goal remains active. These results do not qualify a physical no-RT
device or complete mandatory API/resource-limit support.

The [previous shared-BVH4 report](rt_cts_bvh4_2026-10-26.md) and all earlier
failures remain history. The [current evidence JSON](rt_cts_aligned_as_2026-10-26.json)
records exact frozen manifests/DLLs, commands, exit codes, QPA/stdout hashes and
component logs. No external CTS source, binary or expected result was changed.

## Real external CTS

External source: `D:\Codes\VK-GL-CTS`, revision
`d18e4c68543103371715845cd914c71e5859963e`. Existing Release `deqp-vk.exe` SHA-256:
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
Windows x64, NVIDIA RTX 3080, driver 596.49, Vulkan 1.4.329, SDK validation
1.4.357. Each case runs in its own hidden process with a 60-second limit, shader
cache disabled, implicit layers disabled and raw logs retained.

| Run | Cases | Pass | Fail/skip/crash/timeout/no result |
| --- | ---: | ---: | ---: |
| `rq-aligned-values-01` | 40 | 40 | 0 |
| `rq-generic-as-01` | 6 | 6 | 0 |
| `native-generic-as-01` | 6 | 6 | 0 |
| Previous matching native controls for the original 40 | 40 | 40 | 0 |

The original [40-case selection](rt_cts_bvh4_cases.txt) was rerun after the
address change. It retains descriptor/integer conversion, helper/control flow,
dynamic AS/query indexing, shared memory, procedural candidates, transforms,
getters, culling and all six ordinary shader stages. The [six additional cases](rt_cts_generic_as_cases.txt)
build triangles/AABBs with bottom-only, top-only and both GENERIC creation types.
All current runs enable Khronos validation and have zero API validation errors.

These are selected RQ development tests. The separate legacy RTP layer still has
its previously measured 3 pass/3 fail six-case matrix and vec4 payload limitation.
The new typed compiler component is not yet the production RTP pipeline.

## Address implementation and executed regression

The [AS address ABI](../layers/raytracing/docs/AS_ADDRESS_ABI.md) documents the
pinned normative contract, wire layout and publication/lifetime proof. Each
AS-storage buffer gets one affine public range; sparse seven-level pages cover
the complete 56-bit key space without a dense allocation proportional to buffer
size. Same-offset aliases share a value; GENERIC offset differences are preserved.
Private coherent pages are allocated during fallible creation, not GetASAddress.
Stable GetASAddress calls do not allocate, flush or rewrite GPU-visible data.

RQ compilation embeds the device's resolver root as an ordinary 64-bit constant.
It does not consume an additional application binding/push range or retain a
colliding library specialization ID. A regression checks a full 64-bit literal
and preserves application SpecId 6, the same ID used by the private library.
The TLAS builder resolves public BLAS references before storing physical roots
in the existing BVH4 instance records. Both encoder and traverser use scalar
word loads and support 4-byte-aligned physical roots.

`test_gpu_build --aligned-values` now creates real GPU-built BLAS/TLAS objects
through the public API and binds their storage at the smallest legal nonzero
memory offset. On this device the alignment/bind offset are both 16; measured
buffer BDA is `0x7332010`, physical BLAS header is `0x7332110`, and the public
GENERIC value is `0x200`. The test checks 256 alignment, same-offset aliases,
same-buffer offset differences, stable values after alias destruction, query
hits/distances/barycentrics/IDs and final BVH4 structural/bounds guards.
It passes with Khronos validation and **zero validation errors**. A native
control passes too, with its native buffer-memory alignment. Unlike the earlier
ordinary-buffer probe, this executes the formerly problematic AS use end to end.

One initial manual invocation used the repository root as cwd and failed to find
the fixture's relative shader file. The next invocation passed but also ran the
fixture's deliberately invalid undersized-build negative control, producing its
expected validation VUID. The final dedicated alignment mode omits that negative
control, runs from the build directory and passes without validation errors.
All three raw logs are retained and distinguished in the JSON.

## Other checks

- RQ offline suite: **12/12**. New independent registry wire decoding covers all
  56 key bits, max key, 16-TiB virtual buffer/1-TiB offset arithmetic, aliases,
  range/page reclamation, 300 churn cycles, eight-thread alias stress, coherent
  prerequisite and injected failures at every host/GPU creation step. AS cells
  retain allocator ownership and fallible creation-only noncoherent flushing.
  Translator spies check Flags2 tracking, native-create failure and cleanup if
  registry metadata allocation fails after downstream buffer creation.
- Production traversal helper, CPU fixture, complete software GPU BUILD suite
  and native alignment control: **4/4**; full software BUILD takes about 151 s.
- Shared BVH4/resolver components: **5/5**, also **5/5 under ASan/UBSan**.
  The dedicated ordinary-compute resolver additionally passes Khronos validation;
  it checks sparse misses, high keys, aliases, malformed headers, append stability
  and physical roots at 4/16/256 alignment without native RT features.
- POSIX mutex initialization failures now return an error and unwind without
  destroying an uninitialized mutex. These POSIX branches were not run here.

## Typed RTP request component

The actual source-CFG compiler now emits TraceRay, ExecuteCallable and
ReportIntersection requests with all source arguments and typed frame references.
Persistent module backing retains payload/incoming payload, callable/incoming
callable and hit attributes independently of saved SSA values. The opt-in AS
frame policy stores exact opaque 64-bit AS values rather than reloading a changed
descriptor on resume; physical traversal still requires registry resolution.

The new GPU test compiles real RayGeneration, ClosestHit and Callable sources
and executes 23 TraceRay plus 23 Callable requests through nested helpers. A host
test scheduler transfers typed backing between synchronized GPU dispatches.
Boolean/vec3/double/packed-u16-array payloads at distinct nonzero incoming/outgoing
locations receive callee writes through persistent aliases, while saved pre-call
SSA snapshots remain unchanged. Every request field is checked. Separate GPU
cases cover uint64/uvec2 AS conversion, signed32 Trace integer operands,
Intersection Report/hit-attribute backing and a non-suspending incoming callable
whose source has no uint32 type. Compile regressions cover descriptor arrays,
NonUniform, unused incoming interfaces and 16/64-bit access-chain indices.

The standalone RTP repository suite passes **14/14**; analysis, frame execution,
source CFG and RT requests pass **4/4 with Khronos validation**. The corresponding
build/test/validation logs and source files are recorded in the evidence JSON.
Independent review found two issues during development, uvec2 AS conversion and
signed Trace operand transport; both were fixed and have executed regressions.

This is a compiler/request component. Production pipeline creation, shader/SBT
selection, builtins, traversal and scheduler integration remain unfinished.
AS descriptor remap metadata still needs production feature/layout/resource
integration. The external CTS results above validate production RQ; they do not
establish production RTP support or erase its retained 3/3 pass/fail result.

## Reproduce

Build the standalone RQ layer as described in its README. From the repository:

```powershell
ctest --test-dir build-rq-goal -L offline --output-on-failure
ctest --test-dir build-rq-goal -R 'vkrt.gpu_build.aligned_values' --output-on-failure
python scripts/run_rt_cts.py --cts D:/Codes/VK-GL-CTS/build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe --archive-dir D:/Codes/VK-GL-CTS/external/vulkancts/data --cases docs/rt_cts_bvh4_cases.txt --mode ray-query --layer-dir build-rq-goal/layers --output-dir build-rt-cts-evidence/new-aligned-run --validation-layer-dir C:/VulkanSDK/1.4.357.0/Bin --timeout 60
```

Repeat with `docs/rt_cts_generic_as_cases.txt` and a fresh output directory. For a
native control use the same case list, `--mode native` and omit `--layer-dir`.

Full-limit AS descriptor transport and native allocation budgets remain open;
the resolver alone does not settle those M0 proofs. Mandatory Copy, update,
compaction, serialization and RTP's unified object/runtime integration remain
unfinished. Future Copy/deserialize destinations must publish their resolver
mapping before GPU use. The optional capture/replay/device-address command paths
are not supported by this implementation. Physical no-RT and POSIX qualification
are still required.
