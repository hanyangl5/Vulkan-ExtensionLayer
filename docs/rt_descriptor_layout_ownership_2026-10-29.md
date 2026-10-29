<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# DescriptorSetLayout ownership checkpoint: October 29

This checkpoint follows the [PipelineLayout ownership work](rt_layout_ownership_2026-10-29.md)
and repairs a mixed-entry ShaderObject failure found by its GPU qualification.
The complete software AS/RQ/RTP goal and every M0-M5 exit remain incomplete.

Two legal original descriptor layouts can lower to the same native definition.
For example, a storage-buffer binding visible to raygen and one visible to both
raygen and miss both become compute-visible native bindings. The old layer
keyed original definitions by the native handle. When the driver legally
aliased those equal native definitions, the second portable shader-interface
recipe lost its original miss bit, and pipeline layouts made from the first
still-live public layout became incorrectly incompatible. A valid trace with
an actual statically used SSBO then failed recording with
`VK_ERROR_FEATURE_NOT_PRESENT`.

The permanent regression calls actual public C entry points, the production RTP
implementation, the actual portable interface tracker and typed compiler. Its
native spy aliases only equal translated definitions. Before the fix, the
non-alias control succeeds and the alias case fails exactly three observations:
the second original recipe, layout compatibility and public trace recording.
Frozen source copies and hashes bind that result to `8cc6b75`.

Every public DescriptorSetLayout on an RTP device now has its own OBJECT-scoped
token with an immutable original definition and one native reference. Native
values no longer select or overwrite original semantics. The old alias counter,
definition poisoning and direct metadata-registration exports are removed.
Existing test fixtures now create and destroy real public layout objects;
an old fixture that aliased different native descriptor counts was invalid and
has been replaced with legitimate distinct-object compatibility/lifetime cases.

Public native creation, metadata publication and rollback remain under the
shader-interface lock. The wrapper consumes a successful native creation result
on every outcome. Creation publishes the public output only after both the RTP
record and portable recipe succeed. Destruction uses the current compatible
callbacks for the token and native object, removes typed annotations, and leaves
independent pipeline-layout snapshots alive. Defensive device cleanup uses
stored creation callbacks; general C++ allocator, borrowed-view concurrency and
reentrancy qualification remain open.

## Native consumers

PipelineLayout creation copies the public set array into native views once and
reuses it for all native layout clones. Descriptor allocation copies only its
layout array, preserving the original input and pNext. Descriptor-buffer size
and binding-offset queries and the VALVE host-mapping query unwrap their typed
layout arguments. Native-only devices keep their original handles and inputs.

Shader-object creation translates public arrays in both ordinary and original
NoRT-group paths. Reconstructed Ray Query interfaces already contain private
native layouts and bypass public translation. Virtual descriptor templates
continue to retain semantic inputs; push-template descriptor-layout fields are
ignored by their API contract. Debug-utils, PrivateData and legacy Debug Marker
remain keyed by public typed identity; the latter gains its missing DSL mapping.

The pinned registry366 closure classifies17 canonical commands and5 aliases,
including recursive pNext containers and generic metadata consumers. Existing
DGC restrictions close the unsupported indirect paths. No new optional-feature
exclusion is introduced by this checkpoint. Registry closure is a source audit,
not proof of every optional driver's execution behavior.

## Verification scope

The original trace, recipe and compatibility regression now passes. Actual
public API tests also exercise AS-versus-storage-buffer original types,
surviving aliases, compatible A-create/B-destroy callbacks, native/token/real
C++ bookkeeping allocation failures, portable-recipe DEVICE OOM rollback and
retry, native queries/allocation arguments, immutable arrays/pNext and cleanup.

ASan passes a focused existing ownership/replay subset using actual instrumented
RTP, public descriptor, translator and portable-interface units. It covers
original-type recipes, allocator/metadata rollback, C consumers, finish,
inherited replay and concurrent layout retirement. The two typed trace/compiler
builds run in the ordinary canonical suite and real GPU qualification. An
earlier focused ASan selection including those builds reached its180-second
bound while CPU-active; a broader attempt was deliberately interrupted. Neither
is a passing sanitizer result. Container string/vector/optional annotations are
disabled according to local MSVC headers to link existing uninstrumented
compiler/replay/SDK libraries; allocator and memory-access checks remain active.
This is not whole-compiler or leak-sanitizer qualification.

Actual-flag static analysis retains all raw diagnostics. The shader-object unit
has two warnings also present at the pinned baseline; their path review records
the unchanged `linked_prefix` bound and prepared-record invariant. There is no
zero-warning claim for the whole change.

## Mixed-entry repair and final execution

The first GPU batch passes14/15. Its final software case selects compute from
a valid module also containing raygen, while its native control passes. The
software path classifies the whole module as RQ before capture and subsequently
rejects the peer RT declaration. This is a separate source-selection defect,
not a descriptor-token collision.

A small C adapter now reuses the existing exact name/execution-model selector.
Shader-object code prepares only the current independent stage or linked group,
then uses the selected unlowered source for classification, cache capture and
native rewriting. Caller specialization, pNext and layout fields stay intact.
Later independent selection failures retain earlier successes; linked failures
retire their failure unit. The original selector/pipeline path is unchanged.
Actual-rewriter CPU controls reproduce the old rejection and pass afterward,
including FORMAT3 classification, cache export/import and a source/BINARY batch.
Legacy tiny codec fixtures use an explicit selector seam; the new real target
and GPU execution independently qualify selection.

Standalone and parent-layer builds pass. The initial offline batch passes51/51;
the final batch with the permanent real mixed-entry target passes52/52. Five
post-repair GPU checks pass, covering ordinary RTP, ordinary shader objects and
the mixed-entry repair with native controls. Loader traces bind validation to
the pinned DLL. The original14/15 batch remains recorded as such.

The unchanged external CTS d18e4c68 executable runs41 selected AS/RQ/RTP cases
through the final software DLL:41/41 pass with masked native-RT discovery,
zero timestamps, no native-RT rejection and no validation/synchronization error.
The fresh native control batch is40/41: serialization exits with
`0xE06D7363`, zero-byte stdout/QPA and no complete result. Its cause is unknown;
the elapsed time alone does not establish a watchdog. An identical-argument,
same-limit isolated recheck passes. Native primary results remain40/41.

An additional RQ shader-object source/binary attempt passes its native control
but the software run is blocked at opaque native BINARY creation by the guard's
unconditional `uninspectable_shader_binary` rule. It is not a passing boundary
qualification or fallback dispatch result. The queued SPIR-V1.6 variants are
not run after that failure. The guard is unchanged; provenance-based opaque
verification or another complete boundary proof remains future work.

Final repository formatting wraps only the new adapter's declarations and
condition. Both build entry points are rebuilt. All seven complete PE sections
are byte-identical to the preserved tested DLL; only the COFF TimeDateStamp
differs. Runtime records keep their original hashes, while the manifest binds
both images, the before/after source and the exact comparison. The formatted
adapter is also checked again with the actual-flag analyzer.

The [evidence manifest](rt_descriptor_layout_ownership_2026-10-29.json) records
commands, dependency/source/binary hashes, raw results, original failures and
the action/reason log. Complete mandatory resource budgets, generic Pipeline
ownership, remaining memory/descriptor and synchronization semantics, native
alias concurrency and physical no-RT/cross-vendor/POSIX execution remain open.
