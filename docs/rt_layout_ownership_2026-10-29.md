<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# PipelineLayout ownership checkpoint: October 29

This checkpoint removes a demonstrated mandatory layout limitation after the
[ShaderModule ownership checkpoint](rt_shader_ownership_2026-10-29.md). Two legal
application layouts can have different RT push-constant definitions while their
translated native definitions are identical. A native implementation may return
the same non-dispatchable value for those identical native definitions. The old
layer rejected the second application layout. The permanent public API regression
first reproduces that rejection against `35d9d3d`.

Every public PipelineLayout on a software RT device now has its own token and
original definition. Native public references are private to those tokens;
numeric native aliases no longer choose the original semantics. This removes
the canonical-alias record, linked list and original-definition rejection.
Retained private native and RT-context layouts continue to outlive public layout
destruction through recorded command/pipeline ownership.

Public tokens use OBJECT allocation scope and the current compatible Destroy
callbacks. Recording retains the semantic layout, not the public token or its
allocator context. Native creation/publication and public native retirement
retain their serialized transaction. Private native destruction occurs after
the metadata lock is released. Device teardown uses stored creation callbacks
only as defensive cleanup; normal Vulkan child-object destruction is still
required. Complete native-alias threading and C++ container allocation-callback
qualification remain open.

## Consumers and regressions

Owned layouts are unwrapped at native consumers, while replay uses retained
semantic identities until it reaches the appropriate translating hook. Private
implementation handles bypass public lookup. Nonzero unowned identities on an
RTP device no longer pass through as native layouts. The registry consumer
inventory records direct and nested layout uses and existing optional exclusions.
Descriptor templates preserve public semantic layouts while routing through RTP
push hooks; pre-unwrapping those layouts would translate them twice.

Debug-utils names/tags and PrivateData use typed sidecars keyed by the public
token. Legacy Debug Marker additionally needed the missing PipelineLayout
object-type mapping. Its unchanged regression fixture fails five checks before
that mapping and passes afterward. Equal native values keep independent names,
tags and private data; destruction forgets only the retired public object.

The canonical host suite calls production RTP entry points and production debug
handling, with explicit native and rewriter spies. The native layout spy aliases
only equal actual flags, descriptor-set layouts and push ranges; different
private definitions receive different native handles. Tests cover simultaneous
original RT definitions, compatibility, immutable inputs, per-object sidecars,
compatible allocator contexts, OOM rollback, and inherited replay after public
destruction. The concurrent public native retirement/Create oracle is retained
with semantic and live-reference checks. Scope and source hashes distinguish
frozen before fixtures from subsequent expanded tests. These host tests do not substitute for real shader
execution or physical no-RT qualification.

## Verification and remaining work

Standalone and parent-layer builds pass. The offline batch passes 51/51; after a
test-only cleanup improvement for unexpected success in a negative OOM case,
the changed suite is rebuilt and checked separately. Eleven real public Vulkan
dispatch runs pass: nine software runs and two native controls. Software runs
cover classic and maintenance6 layout consumers, push descriptors, descriptor
buffer offsets, debug names and PrivateData, retained Trace execution and
immediate native compute-state restoration. Loader traces bind every run to the
pinned validation DLL. Native-layout alias equivalence remains a separate host
oracle; the GPU tests do not force a driver's handle-allocation policy.

The unchanged external CTS d18e4c68 executable from `D:/Codes/VK-GL-CTS` runs 41
selected AS/RQ/RTP cases, including graphics RQ stages and an empty RTP layout.
Fresh native controls pass 41/41. The software primary batch passes 40/41; its
`data_spill.pipeline_interface.ray_payload` case terminates with the CTS
30-second touch-watchdog message. The runner categorizes that termination as
`crash`, while the QPA records `Timeout`; it is not evidence of an access
violation. An isolated recheck passes with the same binary, command arguments
and watchdog limits, changing only output isolation and case selection. The
primary batch remains 40/41. Successful primary cases plus that recheck cover 41
distinct cases, with no skips, validation/synchronization errors, downstream
native-RT rejections or timestamp writes. Timing stability and the original
watchdog cause remain open; concurrent CPU work is not established as the cause.

Two changed production translation units pass actual-flag Clang analysis with
zero diagnostics; four checker-family control diagnostics are observed.
Pinned ub-mode crapper, dryer and mutator are invoked on the changed C++ unit
and report no supported source files. Their versions do not support C/C++;
no native CRAP, duplication threshold or mutation score is claimed. The earlier
Python quality debt remains open.

A focused ASan run also passes the unchanged layout ownership/replay cases
through instrumented production RTP, shader and debug units. The scratch test
translation unit selects the existing cases, including allocator/OOM and
concurrent retirement checks. It does not sanitize the linked typed compiler,
replay or SDK libraries. Local MSVC headers document the string, vector and
optional container annotation-disable macros needed to link those libraries;
memory-access and allocator ASan checks remain active. Initial string/optional
annotation link mismatches and SDK missing-PDB link warnings are retained.
The separate instrumented canonical suite also completes within its 600-second
bound and passes. This remains the stated native-unit instrumentation scope,
not full compiler or leak-sanitizer qualification.

The [evidence manifest](rt_layout_ownership_2026-10-29.json) binds commands,
sources, binaries, raw logs, validation and CTS selections to this checkpoint.
The full AS/RQ/RTP goal and every M0-M5 exit remain incomplete. Generic Pipeline
ownership, DescriptorSetLayout original-definition identity, complete resource
budgets, command/synchronization coverage, native-alias concurrency and physical
no-RT/POSIX execution remain qualification work. The earlier supplemental generic
Pipeline reproduction produced no execution evidence; it remains unverified.
