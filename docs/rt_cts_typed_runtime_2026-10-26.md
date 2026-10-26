<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Canonical typed RTP, UPDATE and serialization checkpoint

The complete [goal](ray_tracing_fallback_goal.md) remains active. This checkpoint
connects typed RTP to the production Mesa/BVH4 backend and the layer-owned AS
resolver. It does not complete M0–M5, full mandatory functionality or physical
no-native-RT qualification. Source base: `03af7df`, with the changes in this
checkpoint. Historical failures remain in the machine-readable
[evidence record](rt_cts_typed_runtime_2026-10-26.json).

## External CTS

All fallback runs use `VK_LAYER_VKRT_SOFTWARE_RAY_QUERY`, including RTP cases.
The runner's new `--mode software-rt` names this canonical manifest explicitly;
At this historical checkpoint, `--mode ray-tracing` selected the older RTP
prototype; that obsolete mode and executable have since been removed.

| Matrix | Pass | Fail | Skip/crash/timeout/no result |
| --- | ---: | ---: | ---: |
| Canonical RTP expanded selection | 20 | 0 | 0 |
| Matching native RTP control | 20 | 0 | 0 |
| Canonical RQ/AS regression | 54 | 2 | 0 |

The exact RTP [20-case selection](rt_cts_typed_runtime_cases.txt) includes all six
stages, 16×16×16 launches, procedural hits, callable structure spilling, payload
updates, empty AS and GPU AS UPDATE. The RQ selection remains the existing
[56-case matrix](rt_cts_copy_checkpoint_cases.txt). Native controls for its
previously selected cases are retained in the earlier
[copy checkpoint](rt_cts_copy_pipeline_2026-10-26.md).

All selected RTP cases now pass. Intermediate 18/20 and 19/20 runs retain the
image texel-pointer/root and explicit-layout struct failures. Image atomics now
preserve exact source execution and memory semantics; resource/value aggregate
types are separated with `OpCopyLogical`. The BDA-only system-pointer classifier
is covered by an exact CTS Intersection SPIR-V fixture compiled through both
production frame transports.

The RQ failures still stop at unsupported AS property-query pool creation,
before executing compaction/serialization. GPU serialization itself has a
separate public API proof; these CTS failures are not reported as fixed.

The first canonical RTP attempt passed 0/6 at storage-image type cloning. After
that fix it passed 4/6, exposing the unused hit-attribute declaration and UPDATE
gaps. Both original cases now pass in the expanded selection. The compiler
preserves storage-image source operations; opaque image SSA is not copied into
ordinary byte frames. Values live across unsupported source-state boundaries
are still explicitly rejected.

CTS checkout: `D:/Codes/VK-GL-CTS`, revision
`d18e4c68543103371715845cd914c71e5859963e`, binary
`build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe`, SHA-256
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
Device: NVIDIA GeForce RTX 3080, driver 596.49, Vulkan 1.4.329; SDK/validation
1.4.357. Each case runs in a separate hidden process with validation enabled,
a 60-second limit and shader cache disabled in CTS. The runner snapshots the
manifest/DLL and records their hashes, environment, exact command, session
identity, stdout and QPA result. Raw artifacts remain under
`build-rt-cts-evidence/rtp-typed-capability-final`,
`rtp-typed-expanded-native` and `rq-typed-capability-final`.

Reproduce the RTP run, using a fresh output directory:

```powershell
python scripts/run_rt_cts.py --cts D:/Codes/VK-GL-CTS/build/external/vulkancts/modules/vulkan/Release/deqp-vk.exe --archive-dir D:/Codes/VK-GL-CTS/external/vulkancts/data --cases docs/rt_cts_typed_runtime_cases.txt --mode software-rt --layer-dir build-rq-goal/layers --output-dir build-rt-cts-evidence/new-typed-run --validation-layer-dir C:/VulkanSDK/1.4.357.0/Bin --timeout 60 --cts-revision d18e4c68543103371715845cd914c71e5859963e --layer-revision YOUR_SOURCE_REVISION --device-info-file build-rt-cts-evidence/device-summary.txt
```

Use `--mode native` without `--layer-dir` for the control; substitute the RQ
case list for that regression. A selected passing matrix is not conformance.

## Runtime and public API

The typed compiler links all six real RT stage libraries with the exact shared
resumable BVH4 traversal. A native SPIR-V Private array supplies per-invocation
frames, sized from actual metadata and the Vulkan default stack equation, with
dynamic-stack variants. Immutable context/SBT/push/AS data uses BDA; repeated or
simultaneous command-buffer executions do not share writable continuation frames.
Source frame offsets still have an explicit less-than-4-GiB boundary.

The public API owns shader/pipeline state, opaque 32-byte group handles, SBT
selection, stage-specific push snapshots, descriptor replay, chunked launch
dispatch and command-buffer lifetimes. Genuine native public pipeline-layout
handles preserve ordinary consumers; independently retained compatible layouts
outlive public destruction. RT pipeline/stage bits are consumed before native
dispatch. Ordinary compute pipeline/descriptors/push values are restored.
No-RTP devices pass ordinary `VK_SHADER_STAGE_ALL` pushes through unchanged.

Public GPU tests run through normal advertised RTP feature/extension enablement,
GPU BLAS/TLAS construction, hit/miss traces, stack queries, shader destruction,
layout destruction with maintenance4, command-buffer reset/reuse and ordinary
compute state restoration. Validation plus synchronization validation pass both
above and below the layer (`build-rq-once/rtp-api-normal-sync-{above,below}.log`).
Base discovery retains native AS/RQ/RTP truth; it does not fabricate base support
on an ICD without RT. Four optional RTP feature bits and unsupported RT-family
extensions remain masked/rejected; consumed base requirements never reach the ICD.

Standalone typed RTP tests pass 17/17. The runtime proof covers source scalar,
vector, uint16 and bool payload state, preserved SSA, nested Trace, dynamic
Callable calls, GPU SBT records, Any-hit Ignore/Terminate, multiple procedural
Reports, accepted-attribute snapshots, Report return values and RayTmax updates,
first-hit termination and procedural Any-hit termination. A separate unused
`vec3` triangle hit-attribute program verifies the real CTS regression while
defined barycentric reads remain a valid `vec2`. Final runtime validation reports
zero diagnostics (`build-rt-emitter/hit-attribute-final-validation.log`).

An independent native probe reduced NVIDIA's NVVM compile rejection to the
valid 64-bit overflow-bound expression `a > UINT64_MAX / b`. Exact limb-based
multiplication checks preserve the full 64-bit ABI and allow the complete
scheduler to compile. General 64-bit division/modulo remain supported.

## AS operations and synchronization

GPU UPDATE uses complete reconstruction through the shared builder. It preserves
the in-place physical header/public value and does not modify out-of-place source
storage. The queried update scratch requirement covers the full reconstruction.
This is semantic UPDATE support, not a refit performance implementation. Current
COMPACT retains the original footprint; true shrinking would require revisiting
compacted UPDATE sizing. Tests use unmapped AS/scratch memory, compare complete
source bytes via GPU copies and exercise updated TLAS/BLAS hits. Software/native
controls and both validation orders with synchronization validation pass
(`build-rq-cells/update-rebuild-*.log`).

Serialization follows pinned Vulkan-Docs
`d184375dcc5da2b06ca375a8d7d1f9d21ca64a76`: driverUUID, format compatibility UUID,
serialized/deserialized sizes, BLAS reference count/list and aligned BVH4 payload.
BVH4 ABI 2 retains public BLAS values at instance byte 120 separately from the
physical root at byte 0. Deserialization resolves rewritten public references,
including sibling-derived GENERIC values and created, unbuilt BLAS objects.
The AS resolver context ABI remains version 1.

The serializer fixture rewrites repeated BLAS references, destroys originals,
deserializes a TLAS before building replacements, verifies transformed hits and
reserializes the new handles. Physical roots are deliberately 16 modulo 256;
public values remain 256-aligned. Host allocator/UUID/overflow/state checks and
downstream validation pass. Stage-aware barrier translation now maps AS
TRANSFER_READ/WRITE scopes to shader accesses while retaining genuine transfer
scopes. The original failing synchronization log remains preserved.

The GPU property-query component computes execution-time sizes and formats
32/64-bit/availability/stride results into TRANSFER_DST-only buffers, with repeated
CB execution and validation in both layer orders. It is not a public query-pool
implementation: correct concurrent host publication/reset/reuse remains open.

Canonical integration checks pass 15/15 offline and 7/7 selected GPU tests
(`build-rt-cts-evidence/typed-capability-final-{offline,gpu}.txt`).
After the final compiler changes, all seven typed/runtime tests pass again in
`build-rt-review/compiler-exact-cts-final-ctest.log`.

## Remaining goal work

Complete public AS query pools with defensible execution/host synchronization;
expand shader pointer/opaque provenance and aggregate type correctness; complete
descriptor-layout/disturbance and optional pipeline interoperability; qualify full
resource limits, builder variants/large IDs/numerics, cross-queue lifetimes and
physical no-RT devices. An internal 8-byte native compute push context with a
separate private layout could cache native pipelines across Trace commands;
descriptor replay must use that layout and restore original compute state.
The current per-Trace specialization recompiles native compute pipelines.
No milestone is closed by this checkpoint.

The next resource proof has measured input rather than assuming spare bindings:
this RTX reports 1,048,576 core storage-buffer descriptors per stage/set, 16
dynamic storage buffers and `UINT32_MAX` memory allocations. With 16 exposed AS
descriptors and 16 dynamic buffers, a possible public static set SSBO cap is
1,048,544; the per-stage cap would be 1,048,560. The set cap must bound the union
of all RT stages, not just one stage. UBOs, update-after-bind max-pair accounting,
images/samplers, inline blocks and aggregate resource limits require analogous
proofs. This is a design calculation, not an implemented or qualified profile.
Per-AS cells and radix pages still create separate private memory allocations;
a fixed allocation-count subtraction cannot prove their unbounded lifetime cost.

The regenerated production linked runtime is SPIR-V 1.5 with Shader, Int64,
Int16 and PhysicalStorageBufferAddresses capabilities, and validates for Vulkan
1.2 (`build-rt-cts-evidence/typed-runtime-profile-compiled.spv`). Standalone
compute-entry compiler fixtures can emit SPIR-V 1.6; that does not establish the
production library route's minimum. The active builder predicate now checks native Vulkan 1.2, compute subgroup
BASIC/BALLOT/SHUFFLE, float32 signed-zero/Inf/NaN preservation, 128 invocations
and X size, 2068 shared bytes and required push/dispatch dimensions. Fourteen
independent missing-property cases reject before native device creation;
ordinary/deferred-only devices skip this GPU predicate. Full variant/resource
qualification still precedes no-RT discovery.
