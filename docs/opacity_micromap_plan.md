<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Software Opacity Micromap Layer: Implementation Plan

**Status:** planning only; no milestone is complete. This plan implements the
[goals and specification](opacity_micromap_layer.md). Milestones are development
checkpoints, not progressively smaller definitions of the final extension.

## Delivery sequence

### M0: Prove native adapter mechanisms

- Pin the Vulkan/SPIR-V revisions used for implementation and record dependency,
  feature, property, entry-point, and interaction inventories.
- Prototype metadata lookup for descriptor AS operands and integer-derived AS
  operands, including descriptor indexing and function arguments.
- Prove internal resource access without assuming spare application bindings or
  push constants. Document any hardware capability prerequisites.
- Prove the Ray Query candidate wrapper with mixed OMM/non-OMM triangle and AABB
  inputs, checking committed state and original flags.
- Prove a native RTP wrapper across ordinary, absent, and zero/null hit groups,
  procedural Intersection side effects, nested traces, and callable execution.
- Specify and validate the context/SBT ABI, including indirect/GPU-generated input
  handling, before treating it as an implementation contract.

**Exit:** executable semantic evidence for each mechanism, a reviewed capability
contract, and an explicit resolution or escalation for every failed proof. No
production extension-advertisement claim at this stage.

### M1: Implement the shared OMM core

- Define versioned storage and metadata layouts independent of native/software BVHs.
- Implement object tracking, size queries, GPU build, format decoding, association,
  and complete opacity/flag resolution.
- Add CPU reference vectors and GPU evaluator comparisons, including every reported
  subdivision level and special index.
- Define synchronization translation and ensure build products survive legal reuse
  of input and scratch buffers.

**Exit:** reusable core with deterministic isolated tests. It must not require the
software RT fallback to build or run.

### M2: Integrate native AS and Ray Query

- Translate geometry/instance metadata alongside native AS build and update.
- Implement complete query rewriting and restore all application-visible state.
- Cover multiple geometries, multiple BLASes, instancing, dynamic descriptors,
  GPU-generated instances, multiple live queries, and helper-function use.

**Exit:** Ray Query acceptance rows pass across the reported native configurations;
RTP remains a required independent milestone where supported.

### M3: Integrate native Ray Tracing Pipelines

- Implement Any-Hit and Intersection wrappers and original-control-flow handling.
- Implement pipeline/group-handle translation, SBT remapping, original-record
  access, and context transport without increasing application recursion depth.
- Cover pipeline libraries, stack-size accounting, nested Ray Query, indirect
  trace variants, and GPU-generated SBTs according to the exposed native features.

**Exit:** RTP acceptance rows pass with side-effect checks, including mixed triangle
and procedural scenes. Native traversal remains in use.

### M4: Complete API and lifecycle coverage

- Finish clone/compact, queries, serialization, compatibility reporting, and AS
  sidecar propagation/relocation through permitted lifecycle operations.
- Complete barriers, events, semaphore/queue paths, query result copying, command
  buffer replay, and multithreaded resource bookkeeping.
- Audit every advertised entry point and feature interaction against the pinned
  spec. Exercise every mandatory entry point in a test.

**Exit:** all mandatory device requirements are implemented; the advertised
capability contract matches tested behavior.

### M5: Validate the complete EXT implementation

- Run applicable CTS cases and the dedicated matrix below, with native OMM as a
  differential reference when available and a CPU oracle for unambiguous cases.
- Run unmodified OMM application scenarios for both native consumer APIs.
- Verify native pass-through and RQ-only devices without inventing RTP support.
- Document actual prerequisites, properties, optional features, and unresolved
  defects. Fix semantic failures before declaring the extension supported.

**Exit:** all acceptance rows pass for the supported contract. Formal conformance,
if pursued, follows the applicable Khronos process separately.

## Acceptance matrix

| Area | Required cases and observable checks |
| --- | --- |
| Discovery | Enumeration, proc lookup, enabled features, reported limits, driver sanitization, native pass-through |
| Encoding | 2/4 state, every special index, all reported levels, microtriangle interiors/edges/vertices |
| Association | Implicit/indexed mapping, valid index types/strides, `baseTriangle`, usage-count forms, special-only null handle, multiple geometries |
| Opacity | OMM enable/disable, force-2-state, ray/instance overrides, opacity culling, ordinary geometry in mixed scenes |
| Ray Query | Proceed visibility, auto-confirm, candidate/committed getters, original flags, Confirm/Generate/Terminate, AABBs |
| Query storage | Multiple concurrent queries, arrays, reinitialization, function arguments, supported shader stages |
| AS identity | Descriptor writes/copies, dynamic indexing, helper passage, integer-to-AS conversion, all exposed binding paths |
| RTP behavior | Any-Hit invoked only when logically required, no-duplicate invocation guarantees, Ignore/Terminate, payload/attributes, first/closest hit |
| Procedural behavior | Intersection invocation counts/side effects, report acceptance, logical opacity, application Any-Hit behavior |
| Shader groups and SBT | Missing Any-Hit, legal zero/null groups, multiple hit/miss records, original record data, stride/alignment limits |
| Execution context | Nested traces, callable shaders, query inside RT stages, multiple launches, stack-size queries/commands |
| GPU-generated data | Instance buffers, SBT records, indirect trace arguments, ordering without CPU readback |
| Micromap lifecycle | Build, clone/compact, property sizes, serialization round trip, foreign compatibility rejection |
| AS lifecycle | Build/update permissions, shared BLASes, clone/compact, address relocation, serialization with metadata |
| Memory and synchronization | Released/reused inputs and scratch, barriers/events, ownership, cross-queue semaphores, query availability/results |
| Command execution | Command-buffer reuse, secondary buffers where valid, concurrent submissions, preserved application bindings/state |
| Feature interactions | Each exposed descriptor/pipeline/shader/trace variant, cache/binary round trips, resource limits, optional-feature reporting |

Candidate enumeration and first-hit selection may have specification-permitted
variation. Compare unambiguous scenes exactly; otherwise compare allowed result
sets and invariant side effects. Do not require identical candidate order or the
same first primitive where Vulkan provides no such guarantee. Boundary comparisons
must likewise account for permitted floating-point and edge behavior.

Record GPU time and memory overhead for build, metadata, instrumentation, and
traversal as engineering data. Hardware OMM-equivalent speed is not an acceptance
requirement for this compatibility project.

## Follow-up work

After the native adapter works, connect the shared evaluator to the independent
software RT traversal backend. Its AS builder and traversal format remain separate
decisions. Add optional EXT host commands/capture replay and the KHR API/resource
bridge as explicitly scoped work with their own acceptance coverage.
