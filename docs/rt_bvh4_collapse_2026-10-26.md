<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Production GPU BVH4 collapse, 2026-10-26

The direct Mesa encoder now produces two-to-four-child nodes in the production
AS build path shared by Ray Query and typed RT pipelines. This completes a
construction component of the active M0-M5 goal. Refit, complete builder/API
qualification and the full software RT goal remain open.

## Production path and retained ABI

`layers/raytracing/src/vkrt_build.c` records the existing GPU init, leaf, Morton
sort, LBVH and bounds kernels into transient Mesa IR, followed by
`layers/raytracing/shaders/bvh4_mesa_encode.comp`. The latter now collapses binary
levels before writing final shared-runtime nodes and leaf records. Source IR and
encoder maps remain construction scratch; traversal retains the final runtime
representation.

The runtime header/node/leaf layouts remain BVH4 ABI 2. Internal nodes are still
128 bytes with four scalar FP32 child bounds and tagged IDs. The encoder push
ABI remains 80 bytes. Instance records retain resolved physical BLAS addresses
and the original public BLAS values used by serialization.

For input capacity C, output regions retain their worst-case C-1 internal-node
capacity (zero when C <= 1) and C leaf capacity. GPU header node_count describes
the dense retained nodes, while total_size and region offsets retain the capacity
plan. This change reduces live topology and preserves the existing AS size-query
contract; a shrinking COMPACT operation remains separate work.

## Seven shader phases and dynamic passes

The host and component fixture share `bvh4_mesa_gpu_abi.h` schedule helpers.
For C > 1, J(C) is ceil(log2(C)) rounded upward to an even number; J is zero for
C <= 1. The complete encoder executes 5 + J(C) logical passes:

| Phase | Work |
| --- | --- |
| 0 | Initialize leaf maps/input parents and output counts; validate source metadata and extents. |
| 1 | Assign unique input internal parents and compact referenced leaves using atomic publication. |
| 2 | Check counts, connectivity and conservative finite bounds; initialize parent-jump bank A with one-edge parity. |
| 3 | Read bank A and write doubled ancestor/parity results to bank B. |
| 4 | Read bank B and write doubled ancestor/parity results to bank A. |
| 5 | Allocate dense output IDs for even-depth input internals; mark skipped internals invalid. |
| 6 | Emit retained nodes, leaf records, parent links, root ID and root bounds. |

Phases 3 and 4 alternate for J(C) total pointer-jump passes. The even round count
leaves the final parity in bank A. A valid input tree has fewer ancestor edges
than its capacity, so doubling covers every possible input depth without a
selected fixed depth ceiling or CPU active-count readback. Each logical pass
supports multiple dispatch chunks, with `first` and all address arithmetic in
64 bits. A compute write-to-read/write barrier separates dependent passes.
There is no inter-workgroup polling or assumption that invocations run in order.

Each jump word packs an ancestor index with the parity of the represented path.
A non-root starts at its input parent with parity one; the root starts at the
invalid-ancestor sentinel with parity zero. A round reads the ancestor's old
jump, replaces the ancestor with that jump's ancestor, and XORs the two path
parities. Source index capacity leaves room for this parity bit. Phase 5 requires
the terminal sentinel and uses the accumulated parity as the root-depth parity.

The map workspace has C + 4*I uint32 words, with I = max(C,2)-1: the leaf map,
input parents, jump A, jump B, and retained-node IDs. The production scratch
planner and component planner use the same checked-capacity formula; the byte
offsets and size products remain uint64.

## Collapse and parent invariants

The root is depth zero. Every even-depth input internal survives. Its leaf
children remain leaves; each odd-depth internal child is replaced by that
child's two children. The resulting frontier has two, three or four entries.
Every surviving internal and every leaf has exactly one retained parent and one
emission owner. The root has an invalid parent, and a singleton leaf keeps its
invalid parent without an internal wrapper.

Retained internal IDs are dense atomic allocations. Each internal parent is the
mapped input grandparent, and leaf parents name their retained emission owner.
The encoder copies bounds from the validated source frontier and preserves the
original primitive/geometry/instance IDs, complete geometry flags, instance
transforms and BLAS relocation fields. Unused child IDs are invalid and unused
child bounds are zeroed. Existing GPU status errors still atomically invalidate
the complete output header.

The CPU reference encoder retains its largest-area frontier policy. Production
uses deterministic depth-pair collapse, so their exact topologies can differ.
Both target the same runtime ABI and conservative coverage/parent invariants.

## Independent geometry and topology oracles

The component GPU test separates two sources of expected results:

- Original test geometry and raw-byte fixtures supply expected leaf records,
  original IDs, full flags and instance relocation/transforms. These remain
  independent of the GPU builder's emitted records.
- After execution and completion synchronization, the CPU Mesa adapter reads
  the actual GPU construction IR to predict the even-depth internal count and
  the distribution of two-, three- and four-child nodes. This topology is never
  uploaded as a construction input.

The verifier independently walks the resulting runtime hierarchy, checking
dense IDs, unique leaf coverage, conservative child/root bounds, parent links,
unused slots and allocation guards. The actual LBVH test starts its source IR
poisoned and runs construction plus encoding in one command buffer. Its eight
triangle/AABB scenes cover capacities 0, 1, 321 and 1025, multiple geometries,
inactive NaN slots and full geometry flags. Larger producer scenes explicitly
exercise four-child nodes. Additional direct-IR fixtures include balanced
four-leaf triangle/AABB/TLAS trees, sparse source slots, empty/singleton roots,
a 4096-leaf chain, TLAS transforms/relocation and malformed-input status paths.

## Executed evidence

The independent Release build `build-bvh4-collapse` uses Clang 20.1.3 and Vulkan
SDK 1.4.357.0. Actual GPU runs select NVIDIA GeForce RTX 3080. The fixtures enable
compute prerequisites and no native RT extension or feature. Khronos validation
and synchronization validation were enabled; the fixture's validation error
counter remained zero. The five CTest results are all executed passes:

| Test | Scope | Result |
| --- | --- | --- |
| portable_bvh4_reference | Wire ABI, layouts, random/deep CPU reference and rejection checks | PASS |
| portable_bvh4_mesa_adapter | Scalar IR adaptation, original records and malformed input | PASS |
| portable_bvh4_gpu_encode | Generic hierarchy component plus direct Mesa collapse/records and edge cases | PASS |
| portable_bvh4_mesa_gpu_build | Actual GPU construction and four-child encoding in one command buffer | PASS |
| portable_as_address_gpu_resolver | Existing public-value/root resolution regression | PASS |

Logs are `build-source-layout/bvh4-collapse-configure.log`,
`bvh4-collapse-build.log` and `bvh4-collapse-validation.log`; CTest's detailed
output is retained in `build-bvh4-collapse/Testing/Temporary/LastTest.log`.

The production layout check also passes with capacities through `2^29-1`,
allocations and scratch offsets above 4 GiB, and enough even pointer-jump rounds
for the maximum tree depth at each capacity. Its evidence is
`build-source-layout/bvh4-collapse-production-layout.log`. These are arithmetic
checks; those giant AS allocations/builds were not executed.

The combined first main integration build also passes six public GPU tests:
software/native ordinary build, aligned values and pool-pressure modes. Its
log is `build-source-layout/resource-collapse-build-gpu-first.log`. This is a
log-only integration record shared with concurrent resource work; no DLL hash
is inferred from its mutable build directory. The main checkpoint report owns
final DLL bindings and external CTS records.

## Remaining gates

- Mesa construction IDs retain their existing 32-bit byte-offset ceiling, and
  the original builder dispatch-capacity restriction remains. Large runtime and
  scratch planning does not qualify construction at the mandatory count limit.
- Active source bounds remain finite; scaled FP32 inverse transforms still need
  numerical qualification for ill-conditioned legal inputs.
- UPDATE still performs a complete GPU reconstruction. A true refit path must
  update leaf data and propagate bounds through the retained topology separately.
  The current leaf producer prunes mask-zero and empty-BLAS instances; a refit
  design must preserve their legal recoverability. The specification distinguishes
  inactive references from degenerate instances and permits updates of degenerate
  instances. See [Acceleration Structure Update Rules](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html).
- PLOC/HPLOC host contracts, additional builder variants, traversal numerical
  coverage, complete resource/synchronization behavior and physical no-RT/POSIX
  qualification remain under the full goal.

See the [implementation status](ray_tracing_fallback_status.md) for complete
milestone gates and external CTS snapshots. These component results establish
the production collapse step and preserve the remaining scope.
