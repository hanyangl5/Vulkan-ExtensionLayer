<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# Graphics Ray Query checkpoint, 2026-10-29

The experimental software backend now instruments descriptor-sourced queries in
monolithic graphics pipelines. It retains shader sources, specialization data,
pipeline layouts and private compatible render passes so a later descriptor
context can create a native variant after application creation objects have been
destroyed. This is progress toward the full goal; no complete milestone or EXT
conformance claim is made. Default discovery and native pass-through are unchanged.

All code remains under `layers/raytracing/omm` in the managed OMM worktree. The
independent `D:/Codes/Vulkan-ExtensionLayer` RT checkout was not edited.

## Behavior

Graphics pipelines preserve every shader stage and their static/dynamic state.
Legacy RenderPass1/2 creation is tracked with independently owned compatible
clones; dynamic rendering is supported without a render-pass object. Safe
snapshots respect ignored pointers, including rasterizer discard, mesh inputs
and dynamic state. Private compilation strips dead derivative handles and
application feedback destinations; public batch failure, flags2, compile-required
results, specialization and feedback retain their semantics.

Descriptor binding only changes the shadow context. Public pipeline binds and
subsequent draws/dispatches resolve the native variant. A private bind happens
only when needed. Deferring graphics changes until a draw preserves
[dynamic-state lifetime](https://docs.vulkan.org/guide/latest/dynamic_state.html).
The loader covers all 28 draw symbols in the installed headers, including aliases,
multi-draw, mesh and device-address forms. Compute indirect2 and QCOM tile dispatch
also refresh the compute context. Task/mesh stages are explicitly included when
mapping BindDescriptorSets2; ALL_GRAPHICS does not include them.

A required private pipeline bind during active transform feedback rejects the
recording instead of violating
[VUID 02323](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindPipeline.html).
Graphics libraries, NV shader groups, generated commands and shader objects
remain unsupported. Their hidden stages or GPU-selected bindings cannot silently
bypass context instrumentation.

Public MM/AS operations are unconditional, while the compute dispatches used to
implement them obey conditional rendering. Such internal operations now reject
active or inherited conditional scopes. Explicit End commands still balance
native scopes after a recording error. Ordinary application conditional dispatch
is preserved. Supporting unconditional producers inside those scopes remains
required for the full goal.

## Verification

Runtime: RTX 3080, driver 621.01/API 1.4.357, copied official VVL 1.4.366 with sync
validation. External CTS remains unmodified at
`d18e4c68543103371715845cd914c71e5859963e` under `D:/Codes/VK-GL-CTS`.
The SDK's older VVL 1.4.357 cannot handle a newly advertised driver feature
structure; the previous lifecycle report retains that diagnosis and provenance.

The frozen final layer SHA-256 is
`81f0f333a2390764c301f6ec911757a566c2e48e699c8ae3b275ad05c49a8aec`.

| Check | Actual result |
| --- | --- |
| External CTS, validation above OMM | 69/69 Pass, no validation diagnostics |
| External CTS, validation below OMM | 69/69 Pass, no validation diagnostics |
| Graphics consumer GPU component | 200 checks, 24 native pipelines, Pass |
| Public OMM API, each layer order | 100 checks, zero failures/errors/warnings |
| Public graphics API, each layer order | 8 checked results, zero failures/errors/warnings |
| Non-loader component suite | 27/28 first run; corrected failed GPU fixture passes targeted rerun, all 28 covered |
| Final integration and parent offline suites | 12/12 Pass in each build |
| Parent OMM target build / host-only core | Build succeeds / 3/3 Pass |

The component suite contains 12 offline and 16 GPU targets. Existing unrelated
native proofs were not rerun after the isolated GPU-fixture correction. The
graphics component and corrected Proceed fixture use VVL 1.4.366 with sync
validation. The final public and external results use the frozen DLL above.

The 69-case union contains 26 vertex OMM cases, seven ordinary graphics RQ cases,
34 compute OMM cases and two size queries. The graphics controls include fragment,
geometry, tessellation-control and tessellation-evaluation stages. Compute
level-12 cases are retained. Exact inputs are the union of
[graphics cases](omm_cts_graphics_cases.txt) and
[the existing RQ cases](omm_cts_rq_consumer_cases.txt).

The pinned OMM CTS renderer has vertex, compute and ray-generation forms, with
no fragment OMM form. Its vertex path discards rasterization. The component GPU
fixture therefore separately runs packed four-state OMM through vertex and
fragment queries, with legacy passes and dynamic rendering. It uses CPU-reference
canonical packed storage and explicit metadata identities; public loader
graphics tests separately build ordinary opaque/nonopaque AS objects through
intercepted Vulkan entry points. These are complementary scoped proofs.

The public graphics fixture destroys original shader modules, layouts and render
passes before deferred variants. It checks the valid sequence static bind,
dynamic viewport/scissor setters, descriptor switch, dynamic bind, draw. Further
descriptor changes exercise direct, indirect and indexed drawing, with
independent output buffers and both vertex/fragment queries.

## Failures retained and corrected claims

The first vertex OMM preflight ended with process code `0xe06d7363`, empty stdout
and empty QPA. A debugger run of the same DLL reached the test but ended on the CTS
watchdog. The same immutable DLL then passed both preflight cases in 4.18 seconds
with a larger timeout. This does not establish a root cause or prove a GPU hang.
Both failures and retries are retained separately from the final runs.

The first 28-component run had 27 Pass and one VVL error: an unused query-containing
struct in the Proceed GPU assembly template violates
[VUID 04667](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html#VUID-StandaloneSpirv-None-04667).
The unused declarations were removed from the GPU template; arrays, helpers and
its 1,020 dispatches/2,040 traces were preserved. The query graph test now
distinguishes 11 Vulkan-valid shapes, four non-Vulkan opaque-aggregate IR shapes
and four prohibited pointer graphs. Earlier reports calling all 15 shapes valid
were inaccurate. Historical reports/manifests remain unchanged as dated evidence;
this correction supersedes that legality claim.

The first public graphics run computed all eight results correctly but reported a
fixture CreateDevice error: API 1.2 requires the synchronization2 extension
dependency when requesting opacity micromap. The fixture now requests that
extension. Validation was not disabled and the initial log is retained.

## Open work

The full goal still requires integer-derived AS runtime resolution, the
late-created/late-bound BLAS registry case, complete graphics feature interaction,
production RTP/SBT transport, AS UPDATE/CLONE/COMPACT/serialization and the full
synchronization/retirement matrix. Native RTP mechanism proofs are not production
pipeline integration.

A new ignored radix-registry design note records unresolved CPU slot publication,
early lookup, address reuse and bounded reclamation obligations. No runtime
registry has been implemented or counted as delivered. AS copy also cannot share
a source metadata body: replaying its original BUILD command rewrites that buffer
and would change the clone's metadata independently of its native AS. Copy-owned
snapshot capacity for a future larger source build remains a design gate.

## Evidence and reproduction

`docs/omm_graphics_consumers_2026-10-29.json` pins source, artifact, runtime and
evidence hashes. Raw current artifacts are in `build-omm-integration`, parent
checks in `build-omm-parent`, and host-only checks in `build-omm-host-only`.
Current CTS runs use `build-omm-cts-evidence/graphics-final-above` and
`graphics-final-below`. Prior attempts use distinct directories and are retained.

Use `run_omm_cts.py` with the pinned CTS executable/archive, the generated
`build-omm-integration/graphics-combined-cases.txt`, built layer directory,
compatible validation directory, experimental-software backend and each
validation order. Every summary records the exact command line/environment,
timeout, result, validation status and hashes. The selected CTS matrix is not a
substitute for completing the remaining mandatory contract.
