<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Software RT Fallback: Implementation Status

Checkpoint: 2026-10-25 (UTC). The [implementation goal](ray_tracing_fallback_goal.md)
is in progress. **The complete AS/RQ/RTP fallback is not implemented.**
The [review](ray_tracing_fallback_review.md) records the pre-change `f18c40d3`
baseline; goal alignment was committed as `82251ec3`.

This file is the repository progress checkpoint. GitHub Issues are disabled in
this repository. It records project work; it does not activate a hosted execution
service or imply that an external scheduler is running.

## Goal progress

- [x] Review the committed implementation and align the complete goal/route.
- [x] Add the initial portable BVH4 ABI, checked planner, CPU reference encoder,
  and structural regression tests.
- [x] Fix base RQ opcode 6021 lowering and candidate/committed SBT-offset lookup.
- [x] Fix procedural AABB opacity precedence and add an executed helper regression.
- [ ] M0: complete all capability, no-native-RT, resource ABI, and continuation proofs.
- [ ] M1: integrate Mesa builders, GPU BVH4 encoding/refit, and shared AS objects.
- [ ] M2: complete Ray Query lowering and state semantics.
- [ ] M3: complete typed RTP continuation compilation and runtime integration.
- [ ] M4: complete mandatory API, resource, synchronization, and lifecycle behavior.
- [ ] M5: qualify the supported configurations and physical no-RT GPUs.

Component checkmarks do not complete M0/M1/M2 or the full goal.

## Code in this checkpoint

| Component | Implemented and tested | Remaining boundary |
| --- | --- | --- |
| [Portable BVH4 reference](../layers/common/rt_fallback/README.md) | C/C++ wire-layout checks, tagged indices, checked 64-bit layout planning, iterative binary-to-BVH4 reference encoding, parent links, malformed-input rejection | Mesa adapter, GPU encoder, runtime traversal integration, record/image emission, refit |
| Base RQ SBT offset | Lower opcode 6021; select candidate/committed instance; return only the low 24 offset bits; retain helper in the library | Existing single-AS/provenance architecture remains unchanged |
| AABB opacity | Apply the existing ray-over-instance-over-geometry precedence before opacity culling and expose the correct candidate opacity | Does not complete all RQ traversal or no-native-RT layer behavior |

The BVH4 planner covers allocations larger than 4 GiB and the mandatory
`2^29-1` primitive-count size case without allocating those giant buffers.
Structural tests cover 64 generated random trees, an 8192-leaf unbalanced tree,
empty/singleton inputs, parent/leaf coverage, conservative bounds, and rejection
paths. This is a CPU reference, not evidence of GPU builder adoption.

The RQ getter test lowers real base Ray Query instructions and validates generated
SPIR-V, including candidate/committed selectors and signed/unsigned result ABI
coverage. The helper compute test executes the production GLSL functions over
30 valid opacity/instance/cull combinations and distinct SBT offsets with high
flag bits set. It enables no native RT extension, feature, or fallback layer.
It verifies those production helpers; it is not the complete no-native-RT proof.

## Reproduce the targeted checks

With the documented toolchain and Vulkan dependencies installed:

```sh
cmake -S layers/common/rt_fallback -B build-bvh4 \
  -DCMAKE_BUILD_TYPE=Debug -DVEL_RT_BVH4_SANITIZERS=ON
cmake --build build-bvh4 --parallel
ctest --test-dir build-bvh4 --output-on-failure

cmake -S vkrt_layer/layer -B build-rq -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-rq --parallel 2
ctest --test-dir build-rq -L offline --output-on-failure

# Select the intended compute ICD; Lavapipe was used for this checkpoint.
ctest --test-dir build-rq -L no_native_rt --output-on-failure
```

Local results: BVH4 ASan/UBSan checks pass; five RQ offline tests and the compute
helper semantic test pass. LeakSanitizer was disabled locally because this
container's tracing environment prevents it from starting; leak checking is not
claimed. The helper tests also reject the old missing-getter/geometry-only-opacity
implementations in mutation checks.

The existing commit-policy check rejected the normal GitHub merge subject in
historical `f18c40d3`. It now excludes multi-parent merge subjects from authored
subject-style validation while preserving source-format checks and ordinary
commit validation. Focused checks cover merge, valid ordinary, and invalid
ordinary subjects.

CI builds the shared BVH4 reference separately and runs the RQ offline and
no-native-RT helper labels. Physical GPU qualification, the full no-RT device
path, and full fallback/CTS completion remain open.

## Next work

Run M0 proof work in parallel with shared-backend construction:

1. Establish one capability/device/object translation path for genuinely no-RT
   drivers, including stripping unsupported usages/stages/access flags.
2. Replace the single-AS shortcut with value-preserving AS descriptor/BDA lowering,
   including arrays, dynamic indexing, and helper/control-flow passage.
3. Prove resource transport at full application limits and typed continuation
   frames with alias/opaque-value/ReportIntersection/callable stress shaders.
4. Implement the Mesa IR adapter and GPU BVH4 encoder, then migrate both RQ/RTP
   consumers to the shared runtime. Wire PLOC/HPLOC only with complete host contracts.

Follow the goal's [iteration and stop policy](ray_tracing_fallback_goal.md#iteration-and-stop-policy).
