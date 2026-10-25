# Vulkan Extension Layer (VEL)

## Introduction

There are some extensions and features in Vulkan that are not available everywhere due to various reasons. While not available, some extensions are capable of being done as layer and mapping to any Vulkan implementation regardless of it supporting the desired functionality or not.

## Currently implemented extensions

| Layer                               | Extension provided        | Version | File(s)                     | Point of Contact|
|:-----------------------------------:|:-------------------------:|:-------:|:---------------------------:|:--------:|
| VK_LAYER_KHRONOS_timeline_semaphore | VK_KHR_timeline_semaphore | 1       | layers/timeline_semaphore.c | [@llandwerlin-intel](https://github.com/llandwerlin-intel) |
| [VK_LAYER_KHRONOS_synchronization2](docs/synchronization2_layer.md)   | VK_KHR_synchronization2   | 1       | layers/synchronization2.cpp | [@jeremyg-lunarg](https://github.com/jeremyg-lunarg) |
| [VK_LAYER_KHRONOS_shader_object](docs/shader_object_layer.md)      | VK_EXT_shader_object| 1       | layers/shader_object.cpp    | [@daniel-story](https://github.com/daniel-story) |
| [VK_LAYER_KHRONOS_memory_decompression](docs/memory_decompression_layer.md)   | VK_NV_memory_decompression   | 1       | layers/decompression/decompression.cpp | [@vkushwaha-nv](https://github.com/vkushwaha-nv) |

If you find a problem with one of the layers, please file an Issue and tag the Point of Contact listed in the table above.

NOTE: Not all layers are supported on every platform, please read the documentation for the layer for details.

## Experimental layers

| Layer | Status | Point of Contact |
| --- | --- | --- |
| [VK_LAYER_HANYANG_ray_tracing_fallback](docs/ray_tracing_fallback_layer.md) | Executable compute-based KHR RT pipeline subset; not a conformant implementation of the complete extensions | [@hanyangl5](https://github.com/hanyangl5) |
| [VK_LAYER_VKRT_SOFTWARE_RAY_QUERY](vkrt_layer/README.md) | GPU-built BVHIR and SPIR-V software Ray Query lowering; bootstrap implementation requiring native RT capability exposure | [@hanyangl5](https://github.com/hanyangl5) |

The active [software RT implementation goal](docs/ray_tracing_fallback_goal.md) is complete mandatory Vulkan AS, Ray Query, and RT Pipeline functionality on qualifying devices without native RT. The selected route is Mesa build algorithms and build IR, a portable BVH4 runtime shared by RQ/RTP, and typed SPIR-V continuation lowering. See the [baseline audit and feasibility review](docs/ray_tracing_fallback_review.md).

Both executable layers remain restricted development prototypes. The [D3D12 port record](docs/ray_tracing_fallback_port.md) retains source provenance and historical implementation evidence; its earlier exact D3D12 AS-builder target is superseded by the Mesa/BVH4 decision. Default builds remain opt-in.

## Information for Developing or Contributing:

The software Ray Query layer is independently buildable from `vkrt_layer/layer`, or opt-in with `-DBUILD_RAY_QUERY_FALLBACK=ON` in a configured VEL build. It is a separate implementation from the RT pipeline layer, with its own acceleration-structure format. Enable only one of these experimental RT layers at a time. See [its build instructions and limitations](vkrt_layer/README.md).

Please see the [CONTRIBUTING.md](CONTRIBUTING.md) file in this repository for more details.

## How to Build and Run

[BUILD.md](BUILD.md)
Includes directions for building all components and testing them.

## License
This work is released as open source under a Apache-style license from Khronos including a Khronos copyright.

See [LICENSE](LICENSE) for a full list of licenses used in this repository.

The imported Ray Query layer retains its per-file license and provenance notices, including the MIT-licensed Mesa-derived builder sources. See [its source notes](vkrt_layer/README.md#source-provenance).
