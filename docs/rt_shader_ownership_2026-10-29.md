<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ShaderModule ownership checkpoint: October 29

This checkpoint resolves the same-device ShaderModule identity defect recorded
as open P1 in the [preceding review](rt_ub_review_2026-10-29.md). The complete
AS/RQ/RTP goal and M0–M5 exits remain incomplete. Source/binary bindings and
qualified observations are recorded in the [manifest](rt_shader_ownership_2026-10-29.json).

Every public ShaderModule on a software RT device now uses the same
device-owned token registry. Records distinguish retained RT source from an
owned native module. A native handle can equal a source token numerically
without changing either object's identity. Native compute/graphics creation
borrows the underlying native module; source entry selection/lowering runs
only for source records. Internal native modules continue to use the ICD
directly.

Native module creation keeps its flags, pNext and caller allocator. If token or
registry allocation fails after native creation, the native module is destroyed
using the live creation callbacks and the public output stays null. Public
destruction uses its supplied compatible allocator for the native module and
object-scoped token. Stored creation callbacks are only the fallback for
defensive device teardown; Vulkan requires destroyable children to be retired
before Device destruction. This does not establish complete C++ container or
compiler host-allocation callback qualification.

The public create/destroy hooks moved mechanically from `vkrt_layer.c` to
`vkrt_shader.c`, with body equality recorded before the behavior change.
Permanent tests link the actual production C unit and RTP implementation;
the preceding extracted-wrapper reproducer remains historical evidence.

## Regression evidence

The permanent suite first fails against `26e88cf`, then passes with unified
ownership. It covers raw-first/native-first collisions, identical native bits
across devices, multiple public modules backed by one native value, native
compute/graphics arguments, independent destruction, compatible callback
contexts, native failures, token OOM and defensive teardown. A fixed-storage
fault fixture intercepts one actual C++ registration-node allocation after
successful native creation: it returns host OOM with a null public output,
one native destroy, no live native module and balanced callback allocations.

The CPU rewriter and native driver are explicit spies. Actual SPIR-V rewriting,
pipeline creation and dispatch are qualified independently by public Vulkan
oracles and external CTS. Initial fixture counter pollution and a fault that
hit the fixture's allocation tracker are retained as corrected measurements;
neither is classified as a production regression.

Six changed production translation units pass read-only Clang analysis with
actual build flags and no warnings/errors. A separate positive control emits
the four expected analyzer diagnostics. This is bounded static evidence,
not a lifetime, concurrency or conformance proof.

## Optional consumers and admission

Owned shader debug annotations and PrivateData stay in sidecars keyed by the
public token, preserving distinct identities even if native values alias.
The existing identifier/executable/binary/generated-command restrictions remain.
ARM data graph and provisional AMDX shader enqueue are now discovery-masked and
feature-rejected for software RT because their native creators consume modules
or layouts without an adapter. Native-only devices and unrelated mesh/tensor
features remain unchanged. CPU controls cover every affected bit and chain
order; no ARM/AMDX hardware execution is claimed.

Ordinary maintenance5 inline modules retain their original native stage chain.
Inline source requiring software RQ/RT is explicitly rejected before native
pipeline creation until a complete stage-chain rewrite is implemented. This is
a documented newer-mechanism incompatibility, not a global maintenance5 mask.
Actual Vulkan creation controls show that ordinary inline source succeeds and
native inline Ray Query is valid. The previous DLL lets that Ray Query source
reach the downstream rejection guard; the new path rejects locally with zero
downstream rejections. These are pipeline-creation/admission checks, not dispatches.

Opaque, user-authored instance debug-message payloads are not remapped as typed
module operations. Other non-dispatchable namespaces, including mixed native
and software Pipeline/Layout ownership, still require focused qualification.
A supplemental Pipeline collision reproduction subtask was stopped by automatic
review with a possible-cybersecurity-risk message and produced no execution
evidence; it is not recorded as a confirmed counterexample.

## Header and validation environment

The device environment changed between checkpoints: the preceding report used
NVIDIA 596.49 / Vulkan 1.4.329; a fresh probe now reports 621.01 / 1.4.357.
The unchanged CTS binary exposes a newer cooperative-matrix feature structure
which SDK357 validation does not recognize. The initial native40 run therefore
fails entirely during device validation, independently of the software layer.
These failures and the initially supplied older device report are retained.

An isolated official VVL nightly (release427606911, commit655424a5) recognizes
the feature and passes the native control. SDK357's generated pNext table also
lacks its40-byte size, so software creation still fails for a legal prefix
before the stripped RT feature nodes. The project now pins official
Vulkan-Headers v1.4.366/registry (commit73814d00) and derives codegen's header
from the compiler's selected include path on every configure. This also removes
a stale independent find_path cache hazard when changing SDKs. Native matrix
features remain available; no exclusion was introduced to obtain passing CTS.

Permanent CPU controls preserve all five flags, both RT-relative chain orders,
native-only behavior and caller links. Exact old357 metadata fails the two
prefix controls; matching366 metadata passes. The larger registry adds one
buffer prefix type; the exhaustive prefix-preservation fixture was extended
and keeps its registry-derived count assertion. Compiler/SPIR-V libraries stay
at SDK357 and the runtime feature floor is unchanged. Actual Ubuntu execution,
all future extension structures and general optional interoperability are not
qualified by these updates.

## Final verification

Final standalone/top-level builds and 51/51 offline tests pass. Eleven integrated
dispatch runs precede the header upgrade; four final-DLL dispatch runs cover
Vulkan memory model, mixed entries, debug names and PrivateData with the pinned
validation DLL confirmed by loader traces. Four final real-Vulkan inline
creation/admission checks also pass; these are not dispatches.

External CTS uses the unchanged `D:/Codes/VK-GL-CTS` d18e4c68 executable. All
40 native controls pass. The software batch has 39 passes and one internal
30-second-touch-watchdog termination on serialization; the runner labels it
crash, while stdout identifies the watchdog. An isolated recheck with identical
arguments except its log path passes in 8 seconds. The original batch remains
39/40, not an all-pass run. The 39 primary successes plus the recheck cover all
40 selected cases, including eight graphics RQ stages, with no skips,
validation/synchronization markers, native-RT rejections or timestamp writes.
The original watchdog cause and timing stability remain unqualified.

Successful CTS guard totals are 60,639 checked calls, four Event signals and four
status polls. Per-run commands, original failures, SDK/header/validation hashes
and remaining work are retained in the manifest. This is selected evidence,
not Vulkan conformance or completion of the full goal.

## Action and reason log

1. Revalidate the clean `26e88cf` worktree and open P1; preserve the full goal.
2. Move the two hooks without changing their bodies, so permanent tests execute
   production entry points rather than copied wrappers.
3. Capture corrected failing controls, then introduce one typed ownership
   registry and transactional native wrapping.
4. Exercise real bookkeeping allocation failure and allocator/consumer behavior;
   retain fixture corrections separately from implementation defects.
5. Audit module-bearing registry paths, close demonstrated optional admission
   holes, and reject unsupported inline software source before the ICD boundary.
6. Build both canonical entries, run offline, actual Vulkan and external CTS
   controls, and bind observations to their original source/binary hashes.
7. Keep remaining object/resource/synchronization and platform qualification
   work explicit, with the complete goal active and the branch unmerged.
