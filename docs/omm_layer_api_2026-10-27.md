<!--
Copyright 2026 The Khronos Group Inc.
SPDX-License-Identifier: Apache-2.0
-->

# Experimental OMM loader, public device operations and automatic RQ

This checkpoint follows `5486eff` toward the unchanged complete EXT goal. It adds
a real Vulkan loader adapter and public device-operation path. It does not yet
support unmodified OMM rendering applications, and no full milestone is complete.

## Current routing

`VK_LAYER_VEL_opacity_micromap` normally forwards native operations. It adds no
device-extension enumeration or feature support to a device without native OMM.
With `VK_VEL_OMM_EXPERIMENTAL_SOFTWARE=1`, captured at instance creation, an
EXT-enabled device can explicitly exercise the incomplete software backend.

The development mode currently rejects RQ/RTP-enabled device creation and all AS
builds. Native AS storage is never interpreted; layer-owned micromap handles are
never forwarded into native AS builds. Complete association/provenance and shader
integration must replace these temporary gates. Queries and serialization reject
recording; optional host/capture features are false. No software qualification
claim follows from the native OMM extension remaining available on this RTX 3080.

The embedded kernels currently require Vulkan 1.2, BDA and shaderInt64. Device
creation checks instance and physical-device versions and supported features
before privately enabling the needed feature bits. A lower extension-only
environment still needs implementation/qualification, rather than an assumption
that SPIR-V 1.5 can be sent to it. KHR opacity micromaps remain separate scope.

## Implemented device path

- Public create/destroy, build-size queries, GPU build and clone/compact now reach
  the canonical software representation through the actual loader interface.
- Original buffer sizes/usages are tracked. Micromap input/storage flags are
  converted for the native compute/BDA backend; native word padding covers tiny
  logical inputs. Allocation flags and both buffer-bind entry points are handled.
- Copy uses the active size from the GPU header, not a CPU guess based on the
  last recorded build. A larger source capacity can copy into a smaller valid
  destination. Replaying the same copy CB after active size shrinks from 2608 to
  64 bytes is tested without rerecording.
- Internal work changes no descriptor binding. The command tracker restores the
  selected compute pipeline or shader object and replays original push writes
  chronologically across stages. Private push-compatible layouts survive legal
  application layout destruction, and are released with CB state/device teardown.
- Layout tracking counts public creations when native immutable handles alias;
  a canonical snapshot survives until the last public destroy and any retained
  CB references. The tracker handles OOM before snapshot creation, control-block
  allocation or registry insertion without a duplicate native destroy.
- Barrier/event/timestamp and submit stage masks are translated for compute
  accesses. A logical micromap read at AS_BUILD also adds a compute stage scope
  when translating the associated access mask. Ordinary function lookup retains
  native availability; emulated EXT procedures remain available for an enabled
  logical EXT extension even though that name is stripped downstream.

All input and scratch dependencies end after the build completes. Actual buffer
padding/aliasing, sparse/external memory, inherited CB state, maintenance9 events
and the complete queue/feature interaction matrix still require qualification.
NV command-state inheritance is explicitly rejected in the development mode.
Partial bind success with `VkBindMemoryStatus` is tracked per entry when a global
`vkBindBufferMemory2` result reports another bind's failure.

## Actual execution evidence

| Check | Result |
| --- | --- |
| Standalone non-loader CTests | 15 Pass, 0 Fail/skip |
| Public API software fixture, VVL above | 54 checks Pass; no validation/sync errors or validation/performance warnings |
| Public API software fixture, VVL below | Same 54 checks Pass |
| Parent VEL | New module/targets build; 8 offline targets Pass |
| Device backend | 25 checks Pass: real GPU build/copy, replay, input lifetime, OOM and bounds |
| Command-state unit | 55 checks Pass, including byte/history allocation faults and exact ALL-stage replay |
| Synchronization unit | 296 checks Pass, including six allocation faults and unchanged original arrays |
| Actual loader registry with mock native aliases | 61 checks Pass: alias lifetimes, three allocation failure budgets, PFN availability, teardown |
| Automatic RQ transform | 116 new checks plus the unchanged 720-check graph suite Pass |
| Automatic RQ on actual GPU | 1020 dispatches/2040 traces Pass with VVL and sync enabled |

The public API fixture checks canonical bytes and guard regions for two tiny
one-byte inputs and a mixed strided scene. It records copy work before executing
the source build, submits with logical micromap stage semaphore dependencies,
replays, resets/re-records, and copies after destroying all build inputs/scratch.
It hashes all 128 application push bytes after internal work. Maintenance4 is
explicitly enabled for the legal destroy-layout/rebind scenario.

The RQ source contains ordinary native Proceed operations. The transform injects
the filtering loop, restores original flags and candidate AABB opacity, repairs
Phi predecessors and handles original loop headers. Its typed state holds flags,
AABB opacity and explicit per-initialization scene metadata. EXT declarations
are removed before running without the native micromap feature. The fixture's
classifier uses CPU-oracle decisions; AS provenance and the actual sidecar/packed
lookup bridge remain required before admitting general consumer devices.

## Real external CTS

The unchanged external executable and revision from the prior inventory are used:
`d18e4c68543103371715845cd914c71e5859963e`, executable SHA-256
`8cf824c598181ef43ccd74d67acce89c9cc6fc47029a164440f0b0ea74af0380`.
The new runner creates an immutable DLL/manifest snapshot and records command,
environment, hashes, QPA/stdout, timeouts and process status for every exact case.

| Run | Result and meaning |
| --- | --- |
| `layer-native-final` | 8 Pass, 2 Fail; default native pass-through matches the driver-only baseline |
| `layer-software-admission` | 3 early process failures, 0 CTS results/passes; device creation returns `VK_ERROR_FEATURE_NOT_PRESENT` at the incomplete consumer gate |

The two native failures remain compacted-size and serialization-size query-pool
creation with `VK_ERROR_UNKNOWN`; their cause is unresolved. The software probes
include clone, RQ render and size query cases. The runner's generic raw status is
`Crash` for their nonzero exits without QPA results; stdout identifies a CTS
harness fatal exit at device creation, not an observed GPU fault. These are not
NotSupported or qualification passes. The AS/shader bridge is the next required
implementation step, rather than hiding this failure by disabling native
consumer discovery solely to obtain skips.

First attempts are retained: a numeric `implementation_version` made the loader
skip the manifest; a POSIX-style relative DLL path produced Windows loader error
87 and a misleading `VK_ERROR_OUT_OF_HOST_MEMORY` at instance creation. The
manifest now uses the required string and Windows-relative path, while runner
snapshots use a fully qualified path. A public-fixture attempt without maintenance4
also remains with its lifetime VUIDs; the valid configuration is explicit now.

Raw directories are under `build-omm`, `build-omm-layer-api`,
`build-omm-query-proceed`, `build-omm-registry`, and `build-omm-cts-evidence`.
The [evidence manifest](omm_layer_api_2026-10-27.json) pins this checkpoint's source,
module, shaders, tests and raw result files.

## Reproduction

Build the standalone or parent opt-in configuration as before. Run component
tests with SDK validation and synchronization validation enabled:

```powershell
ctest --test-dir build-omm -LE loader --output-on-failure
$env:VK_LAYER_PATH = (Resolve-Path build-omm/omm/layers).Path + ';C:/VulkanSDK/1.4.357.0/Bin'
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation;VK_LAYER_VEL_opacity_micromap'
$env:VK_VEL_OMM_EXPERIMENTAL_SOFTWARE = '1'
$env:VK_LAYER_VALIDATE_SYNC = '1'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
ctest --test-dir build-omm -R '^omm_layer$' --output-on-failure
```

Reverse the two instance layer names for the below-order check. The public loader
target skips without the explicit software selector; do not count a skip as a
software result. `tests/run_omm_cts.py --help` documents the external runner.
Use fresh evidence directories. Its native mode is a pass-through control and
its experimental mode currently exposes the admission failure described above.
