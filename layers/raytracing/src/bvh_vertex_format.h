/* SPDX-License-Identifier: MIT */
#ifndef BVH_VERTEX_FORMAT_H
#define BVH_VERTEX_FORMAT_H
#include <vulkan/vulkan.h>
#include <stdbool.h>

/* Canonical host support predicate shared by discovery and construction.
 * Each case has a decoder in shaders/vk_bvh_helpers.h::load_vertices. */
static inline bool bvh_supported_vertex_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return true;
        default:
            return false;
    }
}
#endif
