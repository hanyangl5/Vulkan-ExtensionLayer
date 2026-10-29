<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Ray tracing branch review with ub-mode: October 29

The review covers the changes from `main` (`38ced57f`) through `e4e7ace`
and the subsequent working tree, including production code, GPU shaders,
build/test selection and authored Python tooling. It applies the installed
[ub-mode rules](https://github.com/unclebob/ub-mode/tree/57a8bdd28e45ed41900ace0bafa2ff76dd9ed70f).
This is a scoped engineering review and regression checkpoint. The complete
AS/RQ/RTP goal and every M0–M5 exit remain open.

**One confirmed mandatory P1 remains open:** virtual RT ShaderModule tokens and
ordinary native ShaderModule values can collide within one device. The review
and passing selections do not establish safe general ShaderModule ownership.

The accompanying [evidence manifest](rt_ub_review_2026-10-29.json) records final
source hashes, commands, logs, review inventories and retained observations.
Component inventories distinguish behavior tests, focused source inspection,
interface inspection and unqualified areas. A file inventory is not a proof
of every legal Vulkan program or every line's correctness.
The inventory snapshot contains 553 existing changed files: 365 authored files
and 188 byte-verified pinned references. All 110 production source/header/shader
files have an explicit bounded review entry; unrelated historical fixtures can
remain inventory-only.

## Confirmed findings and changes

| Area | Finding | Change and independent check |
| --- | --- | --- |
| HPLOC empty builds | A zero-active-leaf result used zero bounds, violating the shared IR encoder's empty representation | Use positive/negative infinity bounds; retain the actual failing GPU build and active-count/structure/ray oracles |
| LBVH publication | Vulkan memory-model arrival atomics synchronized ordinary private child/node accesses across invocations | Mark the communicating buffers coherent; an actual SPIR-V pointer/access probe fails on the six old private accesses and passes after the fix; no deterministic GPU corruption is claimed |
| Ray Query result ABI | Seven integer getters assumed the helper's signedness matched the application's SPIR-V result type | Call the helper using its declared type and bitcast same-width integer results; 14 observable SSBO fixtures fail against the old lowerer and validate after the fix |
| Traversal arithmetic | Reciprocal slab arithmetic could prune actual intersections | Use conservative subtraction/division intervals with explicit noncontraction and uncertainty handling; old GPU helper misses 657/4112 analytic cases, replacement passes all 4112 |
| Object retirement | Instance destruction left physical-device aliases referring to a freed owner | Remove all owner aliases together, with registry lifecycle regressions |
| Shader metadata | An unused global native-module cache could overwrite an AS entry when unrelated non-dispatchable handles shared a value | Delete the cache and its unused metadata; retain the actual shader rewriting and RTP source registry |
| Allocation failure | Registry failures could report successful creation without tracking the object | Return registration failure, pre-register queues and roll back native objects/callback initialization; actual wrapper and outer-device transaction probes |
| Device enumeration | Startup-only enumeration missed later physical-device/group aliases | Register the actual public enumeration results, including incomplete/retry and allocation-failure paths |
| Mixed shader entries | A module containing RT and native entry points was rejected for every native pipeline | Select the exact stage/name, preserve its resources/modes and lower selected RQ code through the actual compiler; native and previous-layer controls plus public compute/graphics spy tests |
| Capability reporting | Native subgroup properties advertised RT stages rejected by the continuation compiler | Mask six RT bits in both property structures for the software profile; preserve other fields, verified through real public property queries in both layer orders |
| Diagnostic reproduction | Debug runs lost saved guard profiles, inherited unrelated guard settings, reused proof logs, and could dereference a missing native layer record | Sanitize guard keys, restore saved overrides, use fresh diagnostic logs and handle the real native summary shape; failing-before CPU captures and targeted killed mutants |
| Test selection | Missing labels excluded software fixtures and included native controls in the default selection | Label the software/native families and wire the debugger environment regression into offline checks; inspect generated CTest selection |
| CI dependencies | Ubuntu 24.04 distro headers cannot parse an unguarded current production type | Pin official Vulkan-Headers and its matching registry in both workflows; retain the old-header failure and successful pinned-header production syntax/generator probes |
| CI triggers | Root CTS runner/test changes did not select either RT workflow | Add the three actual script paths to both event filters and verify the before/after selection |

No confirmed defect is hidden by converting a failure into a skip. Native
physical-interface pipeline creation failures recorded in the preceding
checkpoint remain separate, unresolved observations.

## Open P1: mixed ShaderModule handle namespaces

`vkrt_rtp_create_shader` publishes an allocation address as an RT module handle,
while `vkrt_CreateShaderModule` exposes the ICD's ordinary module value directly.
Nothing reserves those namespaces against each other. An isolated CPU spy
returns the live RT token's value for a newly created ordinary native module:
both creations succeed with the same public value, native compute creation
then returns `-8` without reaching the ICD, and destroying the ordinary module
removes the still-live RT module without destroying its native counterpart.

The same valid compute source passes when only the native handle value differs.
A second-device control using exactly the same bits also passes; the current
RTP registry already scopes lookup by device. This is a **same-device** namespace
defect. The probe links the actual frozen RTP object and verbatim extracted
public hook bodies, with a pass-through rewrite spy for the no-RQ source. It is
not a physical-driver failure or a complete linked-layer test.

The next implementation unit must give every exposed ShaderModule one
device-owned namespace, retain the native module where applicable, unwrap
native compute/graphics consumers, and cover destruction, allocator rollback
and module-consuming APIs. Address tags or an allocation guard cannot guarantee
separation from arbitrary legal native handle values. Add the collision and
cross-device controls to the persistent regression suite before that change;
qualify optional module consumers explicitly. This fix is pending, with the
full goal active.

## Integrated implementation

Production construction now selects eligible HPLOC above the bounded PLOC
range, retains LBVH/refit policy fallbacks, and uses checked 32/64-bit Morton
records. Sorting uses explicit dispatch passes and workgroup collectives;
HPLOC uses range arrival/publication without a global spin scheduler.
The required sort shared-memory budget is checked independently from PLOC's
larger optional budget. Unused prefix-sum/pairing entry shaders and their
production build entries, unused pairing helper and argument types are removed
after checking consumers. The leaf component fixture and pinned upstream
originals remain.

The typed compiler now accepts the supported GLSL450/Vulkan source memory
models, upgrades mixed stages after frame lowering and preserves actual Device
scope operands. ShaderCall synchronization maps to Device scope. Source
release/acquire, atomics and available/visible operands have CPU/SPIR-V checks;
public BDA pointer/nested Callable and six-stage interface fixtures exercise
the integrated path. This does not qualify every memory operand, execution
mode, descriptor-root combination or Vulkan memory-model behavior.

## Verification and tool limits

Standalone and top-level target builds pass, as do **51/51 offline checks**.
There are **13 positive integrated GPU runs**, followed by **4 affected builder
runs against the final LBVH-fix DLL**. These are separately hash-bound; the
earlier runs are not relabeled as final-DLL executions. Both **32/32 software
CTS cases and 32/32 matching native controls** pass, with zero skips, crashes,
timeouts or validation/synchronization markers. The final software profile has
masked RT discovery and zero timestamps: 45,352 checked calls, zero rejected
native boundaries or timestamp writes, four Event signals and four status polls.

Commands and source/binary bindings are recorded in the manifest.
External cases come from `D:/Codes/VK-GL-CTS`, use separate native
controls, and include AS builds/update/compaction/serialization, RT interfaces
and integer Ray Query getters. Masked discovery, zero timestamps and artificial
XYZ dispatch limits are distinct profiles. Their success does not establish
physical no-RT, cross-vendor, POSIX or complete conformance qualification.

The pinned `crapper`, `dryer` and `mutator` tools were actually run on authored
Python. They do not support this project's C/C++/GLSL production languages.
Dryer's covered selection found no candidates at 0.8. The initial targeted
mutation run killed 20/24; added behavioral assertions kill the four former
survivors, and two debugger helper mutants are killed separately. These are
targeted results, not a whole-repository mutation score.

Partial Python coverage is 53% across nine measured files; four generator and
verifier modules lack measured coverage. Twenty-five measured functions remain
at CRAP 10 or above, including `audit_records` and `cts_runs`. The quality gate
is therefore **not all green**. Broad splitting to manipulate a metric is not
part of this correctness patch. The next Python work should add independent
schema/ownership oracles, then separate parsing, validation and execution while
preserving the failing-before evidence.

Read-only Clang analyzer coverage uses actual build flags for all production
C/C++ translation units: 37/37 analyzed, zero errors/timeouts and three raw
warnings, each triaged as a lost-invariant false positive with source reasoning
and applicable allocator/prefix tests. Diagnostics and their disposition are preserved;
neither a clean analyzer run nor passing selected tests proves full API,
resource, synchronization or lifecycle coverage. Historical snapshots and
superseded test observations remain labeled in the manifest.

## Action and reason log

1. Install and hash the unchanged pinned skill and its companion tools, so the
   requested rules have a reproducible source and a known language boundary.
2. Inventory the whole development branch and divide compiler, API, traversal,
   tooling and build contracts, so review extends beyond the newest edits.
3. Retain failing controls for confirmed counterexamples, using old objects,
   old DLLs, pinned sources or actual wrappers as appropriate. Recovered old
   controls are distinct from chronological test-first runs; the review does
   not retroactively claim TDD for every historical branch change.
4. Remove unused shader metadata and unused entry shaders, so shared registry
   state and build complexity shrink rather than grow around the defects.
5. Repair the owning contract, add observable oracles and run targeted mutants;
   preserve native batch outputs, callbacks, resources and legal entry selection.
6. Rebuild final bytes, run offline/public GPU checks and external CTS, and bind
   logs to source/binary hashes instead of reclassifying earlier observations.
7. Record remaining quality and goal gaps, keep the development branch local,
   and leave the complete goal active. Record the late confirmed ShaderModule
   collision as an open P1 and the next independent implementation unit.
