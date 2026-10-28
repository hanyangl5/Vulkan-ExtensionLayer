<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Typed resource provenance across ray tracing suspension

The typed state compiler now transports traditional descriptor-backed images,
samplers, combined sampled images and image texel pointers across source CFG
states and TraceRay, ReportIntersection and ExecuteCallable suspension. It never
serializes native opaque handles or pointers.

## Representation and semantic contract

Each resource SSA result has a compiler-owned provenance recipe. Its root is a
traditional `UniformConstant` descriptor variable; supported constructors are
access chains, copies, descriptor loads, `OpSampledImage`, `OpImage` and
`OpImageTexelPointer`. Numeric operands defined in the function are captured
using the existing typed frame codecs when the original instruction executes.
These include descriptor indices, texel coordinates and sample numbers.
Constants remain constants. A child result copies its parents' captured fields
into its own frame fields, so later execution of a parent definition cannot
alter the child's captured identity.

Within a source state the original native SSA object is reused. At a later
state, only the descriptor identity and the supported constructor instructions
are reconstructed from the captured fields. Source image samples/reads,
storage-buffer loads and atomic operations remain at their original source
positions and are never replayed to reconstruct a resource. Atomic scopes and
memory semantics remain unchanged. Snapshot storage participates in the checked
frame stride, stack budget, and BDA/PrivateArray bounds.

This relies on Vulkan's traditional descriptor-consumption contract, including
update-after-bind: descriptor contents actually consumed by a command remain
unchanged from the first permitted consumption until that command completes.
Update-after-bind allows earlier updates, before consumption begins; it does
not allow changing a descriptor consumed by a pending command. Updating unused
descriptors does not affect a captured descriptor identity. This reasoning does
not extend to mutable descriptor memory supplied through a descriptor buffer or
descriptor heap. Those paths are outside this recipe predicate and the public
layer's supported feature boundary.

Primary references, consulted 2026-10-26:

- [Vulkan descriptor consumption and update-after-bind](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#descriptorsets-binding).
- [SPIR-V 1.6 revision 8, Shader capability validation rules](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#_validation_rules_for_shader_capabilities):
  image, sampler and sampled-image objects cannot be Phi/Select operands in the
  traditional Shader model. `OpSampledImage` results are consumed in their
  defining block. The compiler reconstructs such a result in its consuming
  generated block. Pointer Phi/Select, bindless conversions, mutable opaque
  storage, unknown provenance and descriptor loads with memory operands remain
  explicit unsupported boundaries.

## Verification

The original 52-case external state-spill matrix had 39 passes and 13 failures;
the unchanged native control passed all 52. Every failing opaque case was rerun
after this implementation using the actual external CTS binary:

- CTS revision: `d18e4c68543103371715845cd914c71e5859963e`.
- GPU: NVIDIA GeForce RTX 3080.
- Isolated layer build: `build-rq-once`, worktree after `762ebd4`.
- Khronos validation enabled by CTS; no validation errors were printed.
- Exact case list: `build-rt-cts-evidence/rtp-opaque-recheck-cases.txt`.
- Results: **13 pass, 0 fail, 0 notSupported, 0 crash, 0 timeout, 0 noresult**.
- Evidence: `build-rt-cts-evidence/rtp-opaque-provenance-final/summary.json` and
  the adjacent QPA/stdout files. The runner snapshots and hashes the layer DLL.

The cases cover image, sampler, combined and texel-pointer identity across each
of ExecuteCallable, ReportIntersection and TraceRay, plus image-pointer identity
across ExecuteCallable. The texel cases modify shader input through the callee
between two atomic operations, so replaying a mutable source read would change
the selected texel. These tests provide external semantic coverage rather than
only checking that generated SPIR-V compiles. This subset is not a conformance
claim and does not replace rerunning the full integration matrix.

The existing source CFG and continuation-analysis component tests also pass.
Focused source tests cover plain CFG crossings, suspended texel-pointer reuse,
dynamic coordinates, copied pointers and dynamic descriptor-array indices,
including independent typed snapshot allocation and preservation of atomic
side-effect counts/scopes/semantics. Their generated SPIR-V is validated. The
existing source CFG execution test still runs real repeated suspension/resume
over 32 invocations. Scalar block layout is enabled in state-compiler validator
options, matching the public backend's required scalar-layout feature.
