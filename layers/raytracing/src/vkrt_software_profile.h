/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VKRT_SOFTWARE_PROFILE_H
#define VKRT_SOFTWARE_PROFILE_H
#include "vkrt_resource_profile.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Software ABI/property values, not a complete implementation qualification.
 * Native RT properties/features are deliberately not inputs. Discovery must
 * still satisfy the independent API, resource, query and shader gates. */
struct vkrt_software_rt_profile {
    struct vkrt_resource_profile resources;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp;
};

/* Copy resource facts and detach internal output-chain links. Input may alias
 * output.resources. NULL input/output leaves the output untouched. */
void vkrt_software_rt_profile_init(const struct vkrt_resource_profile *resources, struct vkrt_software_rt_profile *output);
/* Overwrite every AS/RTP payload field and apply resource limits. Preserve
 * caller headers/links, unknown structures and unrelated native properties.
 * Profile storage may itself contain nodes in the caller's output chain. */
void vkrt_apply_software_rt_profile(const struct vkrt_software_rt_profile *profile, VkPhysicalDeviceProperties2 *properties);

#ifdef __cplusplus
}
#endif
#endif
