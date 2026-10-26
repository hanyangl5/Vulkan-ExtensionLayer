<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# AS copy and typed BDA stage checkpoint - 2026-10-26

Production RQ now implements GPU CLONE/COMPACT, and the canonical layer builds
the shared typed SPIR-V compiler. **54 of 56 selected real CTS cases pass**;
the two retained failures are mandatory AS property-query/serialization gaps.
Typed BDA frames and single-dispatch stage linkage are executed components,
not a completed production RTP pipeline. The full AS/RQ/RTP goal remains active.

The [selection](rt_cts_copy_checkpoint_cases.txt) includes the previous 46 cases,
[eight new copy cases](rt_cts_as_copy_cases.txt), and [two pending-operation cases](rt_cts_as_pending_cases.txt).
The [evidence JSON](rt_cts_copy_pipeline_2026-10-26.json) records frozen DLLs,
commands, exit codes and SHA-256 of raw QPA/stdout/component logs. All earlier
reports and failures remain preserved. No external CTS source or binary changed.

## Real external CTS

The external checkout is `D:\Codes\VK-GL-CTS` at
`d18e4c68543103371715845cd914c71e5859963e`. Existing Release binary SHA-256 is
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
Device: RTX 3080, Windows x64, driver 596.49, Vulkan 1.4.329; SDK validation
1.4.357. Each exact case has a hidden isolated process, frozen manifest/DLL,
shader cache disabled, implicit layers disabled and a 60-second timeout.

| Run | Pass | Fail | Skip/crash/timeout/no result |
| --- | ---: | ---: | ---: |
| `rq-as-copy-01`, 56 cases with validation | 54 | 2 | 0 |
| `native-as-operations-01`, ten new operation controls | 10 | 0 | 0 |
| `rq-as-pending-failclosed-02`, retained failures after forwarding fix | 0 | 2 | 0 |
| `rq-as-operations-final-03`, final ten-operation regression | 8 | 2 | 0 |

All previous 46 pass again. The eight new CLONE cases cover triangle/AABB BLAS
and TLAS in compute, fragment, vertex and tessellation control/evaluation shaders.
Passing cases have no validation errors. The original 46 native controls remain
in the [previous report](rt_cts_aligned_as_2026-10-26.md); all ten new operation
controls pass on the native backend.

Both new failures first showed an actual downstream validation error:
`VUID-VkQueryPoolCreateInfo-queryType-parameter`. AS query types reached an ICD
whose AS extension was consumed by the software layer. The unsupported property
command then returned an error from EndCommandBuffer. A new CreateQueryPool hook
now rejects these unimplemented AS pool types before downstream creation, while
ordinary query pools pass through unchanged. The second frozen run still fails
both cases, now explicitly at CreateQueryPool with FEATURE_NOT_PRESENT and no
validation error. This is failure containment, not query implementation or a
passing compaction/serialization CTS result. Unsupported command stubs now mark
command recording failed instead of logging and continuing with apparent success.

## GPU copy and deferred operations

CLONE and COMPACT read actual `header.total_size` on the GPU, not the AS creation
capacity, mapped memory or CPU primitive counts. The new ordinary-compute copy
uses 64-bit BDA/size arithmetic and strided dispatch. Destination publication
uses the existing opaque-AS resolver. Internal barriers and saved compute/push
state restoration preserve application command state. TLAS instance BLAS roots
remain physical addresses in the shared BVH4 ABI. COMPACT currently retains the
same valid representation/size; it does not promise a smaller allocation.

The public API GPU fixture uses unmapped device-local AS storage with no
transfer usage. It builds 4,097 triangles, clones and compacts BLAS/TLAS chains,
checks transformed hits/getters for primitive 0 and 4096, destroys source objects
and checks the copies again. Data exceeds the first 262,144-byte dispatch stride.
The fixture passes with validation both above and below the layer. Host spies
cover high BDAs, 1-TiB/512-GiB capacities, bounds/error paths, allocation failure,
barriers and state replay. These tests complement the external CLONE CTS cases.

The five deferred-operation entry points are now fully layer-owned for
synchronous completion. New operations are complete with result SUCCESS,
maxConcurrency is zero, and Join/GetResult return SUCCESS. Device ownership,
saved allocator callbacks, OOM and callback reentry are tested. Twelve repeated
Release processes and six ASan processes pass, each exercising 2,304 concurrent
rounds. The dependency extension is advertised even if the native ICD lacks it,
and consumed unconditionally at device creation. Future synchronous pipeline
creation using a nonnull operation must return OPERATION_NOT_DEFERRED on success.

A public Vulkan 1.0 fixture enables only this dependency extension and zero
feature bits, exercises eight handles/all five operations, then creates an
ordinary TRANSFER_DST buffer with an unmodified allocation/bind/map request.
It passes with validation above and below the layer. This found and fixed an
integration bug: ordinary/deferred-only devices previously inherited the GPU
backend's private feature/resource requirements. Such devices now bypass AS
initialization, GPU capability queries/augmentation, BDA buffer/allocation flags
and RQ shader rewriting. Their original native features/chains stay intact.

## Typed BDA frame and linked-stage components

The compiler now emits a SPIR-V 1.5 library exporting
`void stage(uint64 frameBDA)`, in addition to the previous standalone SSBO mode.
No frame descriptor or application push-constant range is required. A fixed
256-byte system prefix carries actual launch/ray/ID/transform state, shader-record
BDA, application push snapshot BDA and context BDA. Its byte offsets are pinned
in `layers/raytracing/typed_system_abi.h`. Private frame pools/index multiplication
use 64-bit addresses; the per-source-frame layout still has a documented <4-GiB
boundary. This is not a large physical frame-pool allocation test.

The BDA test executes eight frames at a nonzero offset, all 17 RT builtin
variables including both complete transforms, shader-record reads, 128-byte
application push snapshots with no native push range, occupied application
bindings, suspension/resume and overflow/stride guards. Any-hit source also
publishes terminal IgnoreIntersection/TerminateRay dispositions, preserving
payload writes and remaining stable on redispatch.

The new pipeline linker applies default/provided specialization before analysis,
compiles typed libraries and resolves scheduler imports into one native compute
entry. It handles GLSL reference-parameter adapters, deduplicates function types,
merges compatible FP execution modes and rebuilds the final resource interface.
Conflicting per-stage FP modes fail explicitly pending instruction-level lowering.
Application source shaders are transformed as typed SPIR-V, without GLSL regex.

The linked GPU test performs 21 real RayGeneration-to-Callable calls in one
dispatch, with frame/request/payload coordination entirely on the GPU. Default
and provided bool/u16/u64 specialization variants pass, including stripped source
names, narrow wrapping and high 64-bit values. Late callee writes, persistent
aliases, saved aggregate SSA, blocked-before-ready and exactly-once source
resource side effects are checked. Its only descriptor is application binding 0;
frame access uses BDA. The final module has no RT or Linkage capability, no RT
extension/instruction and only the scheduler's private root specialization IDs.

This closes a concrete private-frame transport/linking gap. It does not prove
full application descriptor budgets: AS value cells still consume SSBOs, and
combining resources from multiple RT stages needs a complete limit strategy.
Canonical CreateRayTracingPipelines/TraceRays integration, SBT/shader selection,
full GPU traversal scheduling and unified production RTP objects remain open.
The legacy production RTP CTS record remains 3 pass/3 fail and is not replaced
by these component results.

The final standalone RTP suite passes **16/16**; analysis, typed frame/source/
request execution, pipeline linkage and BDA system checks pass **6/6 with
validation**, without validation messages. RQ offline checks pass **14/14**;
focused copy host/GPU and dependency device checks also pass. Exact logs and
their hashes are in the evidence JSON.

## Remaining mandatory work

AS property queries need execution-time GPU values, reset/availability/stride
semantics and synchronized host publication for every submitted execution,
including simultaneous/resubmitted command buffers. GPU CopyQueryPoolResults
must preserve transfer-write scope. A timestamp prerequisite or a GPU polling
loop is not a universal proof. Serialization needs its real UUID/relocation ABI
before SERIALIZATION_SIZE can be reported. UPDATE/refit, larger construction
IDs, complete resource budgets and physical no-RT/POSIX qualification remain
unfinished. See the [goal status](ray_tracing_fallback_status.md).
