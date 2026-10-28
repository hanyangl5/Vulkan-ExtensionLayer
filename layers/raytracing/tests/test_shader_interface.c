/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT */
#include "vkrt_shader_interface.h"
#include "vkrt_layer.h"
#include <limits.h>

#define H(type, n) ((type)(uintptr_t)(n))
#define CHECK(x)                                            \
    do {                                                    \
        ++checks;                                           \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                     \
        }                                                   \
    } while (0)
static unsigned failures, checks;
struct allocations {
    size_t attempts, live, fail_at;
};
struct allocation_header {
    struct allocations *owner;
    uint64_t reserved;
};
static struct allocations source_alloc = {.fail_at = SIZE_MAX}, target_alloc = {.fail_at = SIZE_MAX};
static VKAPI_ATTR void *VKAPI_CALL host_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    struct allocations *a = user;
    CHECK(scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE && alignment <= 16);
    if (a->attempts++ == a->fail_at) return NULL;
    struct allocation_header *p = malloc(sizeof(*p) + size);
    if (!p) return NULL;
    p->owner = a;
    ++a->live;
    return p + 1;
}
static VKAPI_ATTR void VKAPI_CALL host_free(void *user, void *pointer) {
    if (!pointer) return;
    struct allocation_header *p = (struct allocation_header *)pointer - 1;
    CHECK(p->owner == user && p->owner->live);
    --p->owner->live;
    free(p);
}
static const VkAllocationCallbacks source_callbacks = {
    .pUserData = &source_alloc, .pfnAllocation = host_allocate, .pfnFree = host_free};
static const VkAllocationCallbacks target_callbacks = {
    .pUserData = &target_alloc, .pfnAllocation = host_allocate, .pfnFree = host_free};
static VkSamplerCreateInfo expected_plain, expected_ycbcr;
static unsigned native_attempts, native_fail_at = UINT_MAX;
static unsigned made_layouts, freed_layouts, made_samplers, freed_samplers, made_conversions, freed_conversions;
static bool expected_translation = true;
static VkResult native_step(void) { return native_attempts++ == native_fail_at ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS; }
static void native_allocator(const VkAllocationCallbacks *a) { CHECK(a && a->pUserData == &target_alloc); }
static VKAPI_ATTR VkResult VKAPI_CALL create_conversion(VkDevice device, const VkSamplerYcbcrConversionCreateInfo *ci,
                                                        const VkAllocationCallbacks *a, VkSamplerYcbcrConversion *out) {
    CHECK(device == H(VkDevice, 2));
    native_allocator(a);
    CHECK(!ci->pNext && ci->format == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM &&
          ci->ycbcrModel == VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601);
    CHECK(ci->ycbcrRange == VK_SAMPLER_YCBCR_RANGE_ITU_FULL && ci->components.r == VK_COMPONENT_SWIZZLE_IDENTITY &&
          ci->components.g == VK_COMPONENT_SWIZZLE_IDENTITY && ci->components.b == VK_COMPONENT_SWIZZLE_IDENTITY &&
          ci->components.a == VK_COMPONENT_SWIZZLE_IDENTITY && ci->xChromaOffset == VK_CHROMA_LOCATION_MIDPOINT &&
          ci->yChromaOffset == VK_CHROMA_LOCATION_MIDPOINT && ci->chromaFilter == VK_FILTER_LINEAR &&
          !ci->forceExplicitReconstruction);
    VkResult result = native_step();
    if (result == VK_SUCCESS) *out = H(VkSamplerYcbcrConversion, UINT64_C(0xcca00000) + ++made_conversions);
    return result;
}
static VKAPI_ATTR void VKAPI_CALL destroy_conversion(VkDevice device, VkSamplerYcbcrConversion value,
                                                     const VkAllocationCallbacks *a) {
    CHECK(device == H(VkDevice, 2) && (uintptr_t)value > UINT64_C(0xcca00000));
    native_allocator(a);
    ++freed_conversions;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_sampler(VkDevice device, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a,
                                                     VkSampler *out) {
    CHECK(device == H(VkDevice, 2));
    native_allocator(a);
    bool ycbcr = false, custom = false, mapping = false, reduction = false;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) {
            ycbcr = true;
            CHECK((uintptr_t)((const VkSamplerYcbcrConversionInfo *)p)->conversion > UINT64_C(0xcca00000));
        } else if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT) {
            custom = true;
            const VkSamplerCustomBorderColorCreateInfoEXT *c = (const void *)p;
            CHECK(c->format == VK_FORMAT_R32G32B32A32_SFLOAT && c->customBorderColor.float32[0] == 0.25f &&
                  c->customBorderColor.float32[1] == -0.5f && c->customBorderColor.float32[2] == 0.75f &&
                  c->customBorderColor.float32[3] == 1.0f);
        } else if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT) {
            mapping = true;
            const VkSamplerBorderColorComponentMappingCreateInfoEXT *m = (const void *)p;
            CHECK(m->components.r == VK_COMPONENT_SWIZZLE_G && m->components.g == VK_COMPONENT_SWIZZLE_R &&
                  m->components.b == VK_COMPONENT_SWIZZLE_B && m->components.a == VK_COMPONENT_SWIZZLE_A && m->srgb);
        } else if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO) {
            reduction = true;
        } else
            CHECK(false);
    }
    const VkSamplerCreateInfo *e = ycbcr ? &expected_ycbcr : &expected_plain;
    CHECK(ci->flags == e->flags && ci->magFilter == e->magFilter && ci->minFilter == e->minFilter &&
          ci->mipmapMode == e->mipmapMode);
    CHECK(ci->addressModeU == e->addressModeU && ci->addressModeV == e->addressModeV && ci->addressModeW == e->addressModeW);
    CHECK(ci->mipLodBias == e->mipLodBias && ci->anisotropyEnable == e->anisotropyEnable && ci->maxAnisotropy == e->maxAnisotropy);
    CHECK(ci->compareEnable == e->compareEnable && ci->compareOp == e->compareOp && ci->minLod == e->minLod &&
          ci->maxLod == e->maxLod);
    CHECK(ci->borderColor == e->borderColor && ci->unnormalizedCoordinates == e->unnormalizedCoordinates);
    CHECK(reduction && (ycbcr ? !custom && !mapping : custom && mapping));
    VkResult result = native_step();
    if (result == VK_SUCCESS) *out = H(VkSampler, UINT64_C(0x55a00000) + ++made_samplers);
    return result;
}
static VKAPI_ATTR void VKAPI_CALL destroy_sampler(VkDevice device, VkSampler value, const VkAllocationCallbacks *a) {
    CHECK(device == H(VkDevice, 2) && (uintptr_t)value > UINT64_C(0x55a00000));
    native_allocator(a);
    ++freed_samplers;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                    const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    CHECK(device == H(VkDevice, 2));
    native_allocator(a);
    CHECK(ci->bindingCount == 5 && ci->flags == VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
    const VkDescriptorSetLayoutBinding *b = ci->pBindings;
    CHECK(b[0].binding == 7 && b[0].descriptorCount == 2 &&
          b[0].descriptorType ==
              (expected_translation ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR));
    CHECK(b[0].stageFlags ==
          (expected_translation ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR));
    CHECK(!b[0].pImmutableSamplers && b[1].binding == 2 && b[2].binding == 3 && b[1].descriptorCount == 1 &&
          b[2].descriptorCount == 1);
    CHECK((uintptr_t)b[1].pImmutableSamplers[0] > UINT64_C(0x55a00000) &&
          (uintptr_t)b[2].pImmutableSamplers[0] > UINT64_C(0x55a00000));
    CHECK(b[3].binding == 99 && b[3].descriptorType == VK_DESCRIPTOR_TYPE_MUTABLE_EXT && b[3].descriptorCount == 1);
    CHECK(b[4].binding == 100 && !b[4].descriptorCount && !b[4].pImmutableSamplers);
    bool flags = false, mutable = false;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            const VkDescriptorSetLayoutBindingFlagsCreateInfo *f = (const void *)p;
            CHECK(f->bindingCount == 5 && f->pBindingFlags[0] == 0 &&
                  f->pBindingFlags[1] == (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT));
            flags = true;
        } else if (p->sType == VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT) {
            const VkMutableDescriptorTypeCreateInfoEXT *m = (const void *)p;
            CHECK(m->mutableDescriptorTypeListCount == 5 && m->pMutableDescriptorTypeLists[3].descriptorTypeCount == 2);
            CHECK(m->pMutableDescriptorTypeLists[3].pDescriptorTypes[0] == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                  m->pMutableDescriptorTypeLists[3].pDescriptorTypes[1] == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            mutable = true;
        } else
            CHECK(false);
    }
    CHECK(flags && mutable);
    VkResult result = native_step();
    if (result == VK_SUCCESS) *out = H(VkDescriptorSetLayout, UINT64_C(0xdd100000) + ++made_layouts);
    return result;
}
static VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice device, VkDescriptorSetLayout value, const VkAllocationCallbacks *a) {
    CHECK(device == H(VkDevice, 2) && (uintptr_t)value > UINT64_C(0xdd100000));
    native_allocator(a);
    ++freed_layouts;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL native_proc(VkDevice device, const char *name) {
    CHECK(device == H(VkDevice, 2));
#define ENTRY(n, fn) \
    if (!strcmp(name, n)) return (PFN_vkVoidFunction)fn
    ENTRY("vkCreateSampler", create_sampler);
    ENTRY("vkDestroySampler", destroy_sampler);
    ENTRY("vkCreateSamplerYcbcrConversionKHR", create_conversion);
    ENTRY("vkDestroySamplerYcbcrConversionKHR", destroy_conversion);
#undef ENTRY
    return NULL; /* Exercise KHR fallback for the conversion commands. */
}
static void check_retired(void) {
    CHECK(!target_alloc.live && made_layouts == freed_layouts && made_samplers == freed_samplers &&
          made_conversions == freed_conversions);
}
static uint64_t read64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static bool contains_handle(const uint8_t *p, size_t size, uint64_t handle) {
    for (size_t i = 0; i + 8 <= size; ++i)
        if (read64(p + i) == handle) return true;
    return false;
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static uint32_t float_word(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    return bits;
}
static void malformed_word(struct vkrt_device_data *target, const void *bytes, size_t size, size_t offset, uint32_t value) {
    uint8_t *copy = malloc(size);
    memcpy(copy, bytes, size);
    write32(copy + offset, value);
    unsigned before = native_attempts;
    struct vkrt_shader_interface *owned = (void *)(uintptr_t)1;
    CHECK(vkrt_shader_interface_validate(copy, size) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
    CHECK(vkrt_shader_interface_materialize(target, copy, size, &owned) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT && !owned &&
          native_attempts == before);
    free(copy);
}
static void malformed_interfaces(struct vkrt_device_data *target, const void *bytes, size_t size) {
    const uint8_t *p = bytes;
    size_t layout = 32 + (size_t)read32(p + 20) * 12;
    size_t first = layout + 24, second = first + 32, sampler = second + 32;
    size_t third = sampler + (size_t)read64(p + sampler), converted = third + 32;
    malformed_word(target, p, size, 4, 1);                        /* Old IF field meanings are not accepted. */
    malformed_word(target, p, size, 24, 16);                      /* Unknown robustness provenance bit. */
    malformed_word(target, p, size, 28, 2);                       /* Push policy is a separate boolean. */
    malformed_word(target, p, size, sampler + 60, float_word(8)); /* minLod > maxLod, 01973. */
    malformed_word(target, p, size, sampler + 64, float_word(-1));
    malformed_word(target, p, size, sampler + 40, 0x7fc00000); /* NaN/infinite bias. */
    malformed_word(target, p, size, sampler + 40, 0x7f800000);
    malformed_word(target, p, size, sampler + 60, 0x7fc00000);
    malformed_word(target, p, size, sampler + 48, 0x7fc00000); /* Enabled anisotropy. */
    malformed_word(target, p, size, sampler + 48, float_word(0.5f));
    malformed_word(target, p, size, second, 7);                                      /* Duplicate ordinary DSL binding. */
    malformed_word(target, p, size, 32 + 12, VK_SHADER_STAGE_COMPUTE_BIT);           /* Duplicate push stage. */
    malformed_word(target, p, size, converted + 28, VK_SAMPLER_ADDRESS_MODE_REPEAT); /* 01646. */
    malformed_word(target, p, size, converted + 44, VK_TRUE);
    malformed_word(target, p, size, converted + 72, VK_TRUE);
    malformed_word(target, p, size, converted + 76, VK_SAMPLER_REDUCTION_MODE_MIN);
    size_t conversion = converted + 128;
    malformed_word(target, p, size, conversion, UINT32_MAX);
    malformed_word(target, p, size, conversion, VK_FORMAT_UNDEFINED);
    malformed_word(target, p, size, conversion, VK_FORMAT_R8G8B8A8_SRGB);
    malformed_word(target, p, size, conversion + 4, UINT32_MAX);
    malformed_word(target, p, size, conversion + 8, UINT32_MAX);
    for (unsigned i = 12; i <= 36; i += 4) malformed_word(target, p, size, conversion + i, UINT32_MAX);
    malformed_word(target, p, size, conversion + 40, 2);
    malformed_word(target, p, size, conversion + 44, 1);
    malformed_word(target, p, size, conversion + 16, VK_COMPONENT_SWIZZLE_R); /* 02581: G */
    malformed_word(target, p, size, conversion + 24, VK_COMPONENT_SWIZZLE_R); /* 02582: A */
    malformed_word(target, p, size, conversion + 12, VK_COMPONENT_SWIZZLE_G); /* 02583: R */
    malformed_word(target, p, size, conversion + 20, VK_COMPONENT_SWIZZLE_G); /* 02584: B */
    malformed_word(target, p, size, conversion + 12, VK_COMPONENT_SWIZZLE_B); /* 02585: only one swapped */
    malformed_word(target, p, size, conversion + 20, VK_COMPONENT_SWIZZLE_R);
    uint8_t *color = malloc(size);
    memcpy(color, bytes, size);
    unsigned conversion_calls = native_attempts;
    for (unsigned i = 0; i < 4; ++i) write32(color + conversion + 12 + 4 * i, VK_COMPONENT_SWIZZLE_R + i);
    CHECK(vkrt_shader_interface_validate(color, size) == VK_SUCCESS); /* Explicit identity is identity. */
    write32(color + conversion + 12, VK_COMPONENT_SWIZZLE_B);
    write32(color + conversion + 20, VK_COMPONENT_SWIZZLE_R);
    write32(color + conversion + 24, VK_COMPONENT_SWIZZLE_ONE);
    CHECK(vkrt_shader_interface_validate(color, size) == VK_SUCCESS); /* Legal paired R/B swap with constant A. */
    write32(color + conversion + 24, VK_COMPONENT_SWIZZLE_ZERO);
    CHECK(vkrt_shader_interface_validate(color, size) == VK_SUCCESS);
    write32(color + conversion, VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(vkrt_shader_interface_validate(color, size) == VK_SUCCESS);
    malformed_word(target, color, size, conversion + 12, VK_COMPONENT_SWIZZLE_ZERO); /* 01655 */
    malformed_word(target, color, size, conversion + 20, VK_COMPONENT_SWIZZLE_ONE);
    write32(color + conversion + 4, VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY);
    write32(color + conversion + 12, VK_COMPONENT_SWIZZLE_ZERO);
    write32(color + conversion + 20, VK_COMPONENT_SWIZZLE_ONE);
    CHECK(vkrt_shader_interface_validate(color, size) == VK_SUCCESS); /* RGB identity allows constant channels. */
    memcpy(color, bytes, size);
    write32(color + conversion, VK_FORMAT_R8G8_UNORM);
    write32(color + conversion + 4, VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY);
    CHECK(vkrt_shader_interface_validate(color, size) == VK_SUCCESS);
    malformed_word(target, color, size, conversion + 4, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_IDENTITY); /* Missing B. */
    write32(color + conversion, VK_FORMAT_R4G4B4A4_UNORM_PACK16);
    write32(color + conversion + 4, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601);
    CHECK(vkrt_shader_interface_validate(color, size) == VK_SUCCESS);
    malformed_word(target, color, size, conversion + 8, VK_SAMPLER_YCBCR_RANGE_ITU_NARROW); /* 02748 */
    write32(color + conversion, VK_FORMAT_A2R10G10B10_UNORM_PACK32);
    write32(color + conversion + 8, VK_SAMPLER_YCBCR_RANGE_ITU_NARROW);
    CHECK(vkrt_shader_interface_validate(color, size) == VK_SUCCESS);             /* 10-bit RGB; alpha precision is ignored. */
    malformed_word(target, color, size, conversion + 12, VK_COMPONENT_SWIZZLE_A); /* Mapped 2-bit alpha. */
    CHECK(native_attempts == conversion_calls);
    free(color);

    /* Start from a valid unnormalized recipe, then violate one condition at a
     * time. This isolates 01072..01077 instead of relying on several errors. */
    uint8_t *unnormalized = malloc(size);
    memcpy(unnormalized, bytes, size);
    write32(unnormalized + sampler + 16, VK_FILTER_NEAREST);
    write32(unnormalized + sampler + 20, VK_FILTER_NEAREST);
    write32(unnormalized + sampler + 24, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    write32(unnormalized + sampler + 28, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    write32(unnormalized + sampler + 32, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    write32(unnormalized + sampler + 44, VK_FALSE);
    write32(unnormalized + sampler + 60, float_word(0));
    write32(unnormalized + sampler + 64, float_word(0));
    write32(unnormalized + sampler + 72, VK_TRUE);
    write32(unnormalized + sampler + 76, VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE);
    CHECK(vkrt_shader_interface_validate(unnormalized, size) == VK_SUCCESS);
    malformed_word(target, unnormalized, size, sampler + 16, VK_FILTER_LINEAR);
    malformed_word(target, unnormalized, size, sampler + 24, VK_SAMPLER_MIPMAP_MODE_LINEAR);
    malformed_word(target, unnormalized, size, sampler + 64, float_word(1));
    malformed_word(target, unnormalized, size, sampler + 28, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    malformed_word(target, unnormalized, size, sampler + 32, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    malformed_word(target, unnormalized, size, sampler + 44, VK_TRUE);
    malformed_word(target, unnormalized, size, sampler + 52, VK_TRUE);
    free(unnormalized);
}
static VKAPI_ATTR void VKAPI_CALL subgroup_properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out) {
    (void)physical;
    VkPhysicalDeviceSubgroupSizeControlProperties *p = out->pNext;
    CHECK(p && p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES);
    p->minSubgroupSize = 4;
    p->maxSubgroupSize = 64;
    p->requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
}
static void source_profiles(struct vkrt_device_data *target, VkDeviceCreateInfo *ci, VkPhysicalDeviceFeatures *core) {
    CHECK(!target->shader_interfaces);
    CHECK(vkrt_shader_interface_init(target, ci) == VK_SUCCESS);
    const uint32_t minimal[] = {0x07230203, 0x00010000, 0, 16, 0};
    uint32_t capability[] = {0x07230203, 0x00010000, 0, 16, 0, (2u << 16) | 17u, 10};
    VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                  .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                  .pCode = minimal,
                                  .codeSize = sizeof(minimal),
                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT};
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    info.pCode = capability;
    info.codeSize = sizeof(capability);
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT); /* Unused Float64 peer. */
    capability[6] = 2;
    CHECK(vkrt_shader_interface_source_supported(target, &info) ==
          VK_ERROR_FEATURE_NOT_PRESENT); /* Geometry cap in a compute entry. */
    capability[6] = 3;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    capability[6] = 11;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    target->shader_object_backend = true;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS); /* Actually enabled private int64. */
    capability[6] = 22;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    target->shader_object_backend = false;
    info.pCode = minimal;
    info.codeSize = sizeof(minimal);
    info.flags = VK_SHADER_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.flags = 0;
    info.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.stage = VK_SHADER_STAGE_VERTEX_BIT;
    info.nextStage = VK_SHADER_STAGE_GEOMETRY_BIT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.nextStage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_shader_interface_finish(target);

    VkPhysicalDeviceSubgroupSizeControlFeatures subgroup = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,
        .pNext = (void *)ci->pNext,
        .subgroupSizeControl = VK_TRUE,
        .computeFullSubgroups = VK_TRUE};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
                                                  .pNext = &subgroup,
                                                  .taskShader = VK_TRUE,
                                                  .meshShader = VK_TRUE};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR rate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
        .pNext = &mesh,
        .attachmentFragmentShadingRate = VK_TRUE};
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT density = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT,
        .pNext = &rate,
        .fragmentDensityMap = VK_TRUE};
    VkDeviceCreateInfo enabled = *ci;
    enabled.pNext = &density;
    struct vkrt_instance_data instance = {.vtable = {.GetPhysicalDeviceProperties2 = subgroup_properties}};
    struct vkrt_instance_data *previous_instance = target->instance_data;
    target->instance_data = &instance;
    core->geometryShader = core->tessellationShader = core->shaderFloat64 = VK_TRUE;
    CHECK(vkrt_shader_interface_init(target, &enabled) == VK_SUCCESS);
    capability[6] = 10;
    info.pCode = capability;
    info.codeSize = sizeof(capability);
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    capability[6] = 2;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    info.pCode = minimal;
    info.codeSize = sizeof(minimal);
    info.flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
    info.stage = VK_SHADER_STAGE_VERTEX_BIT;
    info.nextStage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    info.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    info.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    info.nextStage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    info.stage = VK_SHADER_STAGE_TASK_BIT_EXT;
    info.nextStage = VK_SHADER_STAGE_MESH_BIT_EXT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    info.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    info.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
    info.flags |= VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    info.nextStage = 0;
    info.flags =
        VK_SHADER_CREATE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_EXT | VK_SHADER_CREATE_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.flags = VK_SHADER_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT | VK_SHADER_CREATE_DISPATCH_BASE_BIT_EXT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    info.flags = VK_SHADER_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 16};
    info.flags = 0;
    info.pNext = &required;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_SUCCESS);
    required.requiredSubgroupSize = 3;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    required.requiredSubgroupSize = 128;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.flags = VK_SHADER_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT;
    required.requiredSubgroupSize = 16;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    info.flags = 0;
    info.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    info.nextStage = 0;
    CHECK(vkrt_shader_interface_source_supported(target, &info) == VK_ERROR_FEATURE_NOT_PRESENT);
    vkrt_shader_interface_finish(target);
    core->geometryShader = core->tessellationShader = core->shaderFloat64 = VK_FALSE;
    target->instance_data = previous_instance;
}
static uint32_t physical_api = VK_API_VERSION_1_2;
static VKAPI_ATTR void VKAPI_CALL physical_properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *properties) {
    (void)physical;
    memset(properties, 0, sizeof(*properties));
    properties->apiVersion = physical_api;
    properties->limits.maxSamplerAnisotropy = 16;
    properties->limits.maxSamplerLodBias = 2;
}
static VkResult capability_result(struct vkrt_device_data *dev, uint32_t cap) {
    uint32_t code[] = {0x07230203, 0x00010300, 0, 2, 0, (2u << 16) | 17, cap};
    VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                  .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                  .codeSize = sizeof(code),
                                  .pCode = code};
    return vkrt_shader_interface_source_supported(dev, &info);
}
static VkResult local_size_id_result(struct vkrt_device_data *dev) {
    uint32_t code[] = {0x07230203, 0x00010300, 0, 5, 0, (6u << 16) | 331, 1, 38, 2, 3, 4};
    VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                  .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                  .codeSize = sizeof(code),
                                  .pCode = code};
    return vkrt_shader_interface_source_supported(dev, &info);
}
static void capability_profiles(struct vkrt_device_data *target) {
    /* These deliberately contain only metadata instructions: source_supported
     * qualifies enabled features, while the native compiler validates SPIR-V. */
    const struct {
        uint32_t cap;
        bool backend, alias;
    } caps[] = {{9, false, true},    {12, false, true},   {39, true, true},    {55, false, true},   {56, false, true},
                {69, false, false},  {70, false, false},  {4427, false, true}, {4433, true, true},  {4434, false, true},
                {4435, false, true}, {4436, false, true}, {4439, false, true}, {4441, true, true},  {4442, false, true},
                {4448, true, true},  {4449, false, true}, {4450, false, true}, {5302, false, true}, {5303, false, true},
                {5304, false, true}, {5305, false, true}, {5306, false, true}, {5307, false, true}, {5308, true, true},
                {5309, false, true}, {5310, false, true}, {5311, false, true}, {5312, false, true}, {5345, true, true},
                {5346, true, true},  {5347, true, true},  {5379, false, true}, {6016, false, true}, {6017, false, true},
                {6018, false, true}, {6019, false, true}};
    struct vkrt_instance_data instance = {.vtable = {.GetPhysicalDeviceProperties = physical_properties}};
    struct vkrt_instance_data *previous = target->instance_data;
    target->instance_data = &instance;
    VkPhysicalDeviceFeatures core = {0};
    VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pEnabledFeatures = &core};
    unsigned before = native_attempts;
    CHECK(vkrt_shader_interface_init(target, &ci) == VK_SUCCESS);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(caps); ++i)
        CHECK(capability_result(target, caps[i].cap) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(local_size_id_result(target) == VK_ERROR_FEATURE_NOT_PRESENT);
    target->shader_object_backend = true;
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(caps); ++i)
        CHECK(capability_result(target, caps[i].cap) == (caps[i].backend ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT));
    target->query_backend_enabled = true;
    CHECK(capability_result(target, 12) == VK_SUCCESS);
    target->query_backend_enabled = target->shader_object_backend = false;
    vkrt_shader_interface_finish(target);

    VkPhysicalDeviceVulkan11Features v11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE,
                                            .uniformAndStorageBuffer16BitAccess = VK_TRUE,
                                            .storagePushConstant16 = VK_TRUE,
                                            .storageInputOutput16 = VK_TRUE,
                                            .multiview = VK_TRUE,
                                            .variablePointersStorageBuffer = VK_TRUE,
                                            .variablePointers = VK_TRUE,
                                            .shaderDrawParameters = VK_TRUE};
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &v11,
                                            .storageBuffer8BitAccess = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE,
                                            .storagePushConstant8 = VK_TRUE,
                                            .shaderBufferInt64Atomics = VK_TRUE,
                                            .shaderSharedInt64Atomics = VK_TRUE,
                                            .shaderFloat16 = VK_TRUE,
                                            .shaderInt8 = VK_TRUE,
                                            .shaderInputAttachmentArrayDynamicIndexing = VK_TRUE,
                                            .shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE,
                                            .shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE,
                                            .shaderUniformBufferArrayNonUniformIndexing = VK_TRUE,
                                            .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
                                            .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE,
                                            .shaderStorageImageArrayNonUniformIndexing = VK_TRUE,
                                            .shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE,
                                            .shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE,
                                            .shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE,
                                            .runtimeDescriptorArray = VK_TRUE,
                                            .shaderOutputViewportIndex = VK_TRUE,
                                            .shaderOutputLayer = VK_TRUE,
                                            .bufferDeviceAddress = VK_TRUE,
                                            .vulkanMemoryModel = VK_TRUE,
                                            .vulkanMemoryModelDeviceScope = VK_TRUE};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = &v12,
                                            .shaderDemoteToHelperInvocation = VK_TRUE,
                                            .shaderIntegerDotProduct = VK_TRUE,
                                            .maintenance4 = VK_TRUE};
    core.shaderStorageImageReadWithoutFormat = core.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    ci.pNext = &v13;
    CHECK(vkrt_shader_interface_init(target, &ci) == VK_SUCCESS);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(caps); ++i) CHECK(capability_result(target, caps[i].cap) == VK_SUCCESS);
    CHECK(local_size_id_result(target) == VK_SUCCESS);
    vkrt_shader_interface_finish(target);
    VkBool32 *permissions[] = {&v12.shaderFloat16,
                               &v12.shaderBufferInt64Atomics,
                               &v12.shaderInt8,
                               &core.shaderStorageImageReadWithoutFormat,
                               &core.shaderStorageImageWriteWithoutFormat,
                               &v12.shaderOutputLayer,
                               &v12.shaderOutputViewportIndex,
                               &v11.shaderDrawParameters,
                               &v11.storageBuffer16BitAccess,
                               &v11.uniformAndStorageBuffer16BitAccess,
                               &v11.storagePushConstant16,
                               &v11.storageInputOutput16,
                               &v11.multiview,
                               &v11.variablePointersStorageBuffer,
                               &v11.variablePointers,
                               &v12.storageBuffer8BitAccess,
                               &v12.uniformAndStorageBuffer8BitAccess,
                               &v12.storagePushConstant8,
                               &v12.runtimeDescriptorArray,
                               &v12.shaderInputAttachmentArrayDynamicIndexing,
                               &v12.shaderUniformTexelBufferArrayDynamicIndexing,
                               &v12.shaderStorageTexelBufferArrayDynamicIndexing,
                               &v12.shaderUniformBufferArrayNonUniformIndexing,
                               &v12.shaderSampledImageArrayNonUniformIndexing,
                               &v12.shaderStorageBufferArrayNonUniformIndexing,
                               &v12.shaderStorageImageArrayNonUniformIndexing,
                               &v12.shaderInputAttachmentArrayNonUniformIndexing,
                               &v12.shaderUniformTexelBufferArrayNonUniformIndexing,
                               &v12.shaderStorageTexelBufferArrayNonUniformIndexing,
                               &v12.vulkanMemoryModel,
                               &v12.vulkanMemoryModelDeviceScope,
                               &v12.bufferDeviceAddress,
                               &v13.shaderDemoteToHelperInvocation,
                               &v13.shaderIntegerDotProduct,
                               &v13.shaderIntegerDotProduct,
                               &v13.shaderIntegerDotProduct,
                               &v13.shaderIntegerDotProduct};
    _Static_assert(VKRT_ARRAY_SIZE(permissions) == VKRT_ARRAY_SIZE(caps), "capability permission table");
    v12.shaderSharedInt64Atomics = VK_FALSE;
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(caps); ++i) {
        *permissions[i] = VK_FALSE;
        CHECK(vkrt_shader_interface_init(target, &ci) == VK_SUCCESS);
        CHECK(capability_result(target, caps[i].cap) == VK_ERROR_FEATURE_NOT_PRESENT);
        vkrt_shader_interface_finish(target);
        *permissions[i] = VK_TRUE;
    }

    VkPhysicalDevice16BitStorageFeatures s16 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
                                                .storageBuffer16BitAccess = VK_TRUE,
                                                .uniformAndStorageBuffer16BitAccess = VK_TRUE,
                                                .storagePushConstant16 = VK_TRUE,
                                                .storageInputOutput16 = VK_TRUE};
    VkPhysicalDevice8BitStorageFeatures s8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
                                              .storageBuffer8BitAccess = VK_TRUE,
                                              .uniformAndStorageBuffer8BitAccess = VK_TRUE,
                                              .storagePushConstant8 = VK_TRUE};
    VkPhysicalDeviceVariablePointersFeatures pointers = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
                                                         .variablePointersStorageBuffer = VK_TRUE,
                                                         .variablePointers = VK_TRUE};
    VkPhysicalDeviceMultiviewFeatures views = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, .multiview = VK_TRUE};
    VkPhysicalDeviceShaderDrawParametersFeatures draw = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
                                                         .shaderDrawParameters = VK_TRUE};
    VkPhysicalDeviceShaderFloat16Int8Features arithmetic = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, .shaderFloat16 = VK_TRUE, .shaderInt8 = VK_TRUE};
    VkPhysicalDeviceShaderAtomicInt64Features atomics = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES,
                                                         .shaderBufferInt64Atomics = VK_TRUE,
                                                         .shaderSharedInt64Atomics = VK_TRUE};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
                                                           .shaderInputAttachmentArrayDynamicIndexing = VK_TRUE,
                                                           .shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE,
                                                           .shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE,
                                                           .shaderUniformBufferArrayNonUniformIndexing = VK_TRUE,
                                                           .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
                                                           .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE,
                                                           .shaderStorageImageArrayNonUniformIndexing = VK_TRUE,
                                                           .shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE,
                                                           .shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE,
                                                           .shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE,
                                                           .runtimeDescriptorArray = VK_TRUE};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
                                                       .bufferDeviceAddress = VK_TRUE};
    VkPhysicalDeviceVulkanMemoryModelFeatures memory = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
                                                        .vulkanMemoryModel = VK_TRUE,
                                                        .vulkanMemoryModelDeviceScope = VK_TRUE};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
        .shaderDemoteToHelperInvocation = VK_TRUE};
    VkPhysicalDeviceShaderIntegerDotProductFeatures dot = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, .shaderIntegerDotProduct = VK_TRUE};
    VkPhysicalDeviceMaintenance4Features maintenance = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,
                                                        .maintenance4 = VK_TRUE};
    VkBaseOutStructure *aliases[] = {(void *)&s16,        (void *)&s8,      (void *)&pointers,   (void *)&views, (void *)&draw,
                                     (void *)&arithmetic, (void *)&atomics, (void *)&indexing,   (void *)&bda,   (void *)&memory,
                                     (void *)&demote,     (void *)&dot,     (void *)&maintenance};
    for (unsigned i = 0; i + 1 < VKRT_ARRAY_SIZE(aliases); ++i) aliases[i]->pNext = aliases[i + 1];
    ci.pNext = aliases[0];
    CHECK(vkrt_shader_interface_init(target, &ci) == VK_SUCCESS);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(caps); ++i)
        CHECK(capability_result(target, caps[i].cap) == (caps[i].alias ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT));
    CHECK(local_size_id_result(target) == VK_SUCCESS);
    vkrt_shader_interface_finish(target);

    /* Each registry alternative independently grants the capability. Buffer
     * atomics do not imply shared/image operation permission (checked natively). */
    atomics.pNext = NULL;
    ci.pNext = &atomics;
    atomics.shaderSharedInt64Atomics = VK_FALSE;
    CHECK(vkrt_shader_interface_init(target, &ci) == VK_SUCCESS);
    CHECK(capability_result(target, 12) == VK_SUCCESS);
    vkrt_shader_interface_finish(target);
    atomics.shaderBufferInt64Atomics = VK_FALSE;
    atomics.shaderSharedInt64Atomics = VK_TRUE;
    CHECK(vkrt_shader_interface_init(target, &ci) == VK_SUCCESS);
    CHECK(capability_result(target, 12) == VK_SUCCESS);
    vkrt_shader_interface_finish(target);
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT image = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, .shaderImageInt64Atomics = VK_TRUE};
    ci.pNext = &image;
    CHECK(vkrt_shader_interface_init(target, &ci) == VK_SUCCESS);
    CHECK(capability_result(target, 12) == VK_SUCCESS);
    vkrt_shader_interface_finish(target);

    core = (VkPhysicalDeviceFeatures){0};
    ci.pNext = NULL;
    physical_api = VK_API_VERSION_1_3;
    CHECK(vkrt_shader_interface_init(target, &ci) == VK_SUCCESS);
    CHECK(capability_result(target, 55) == VK_SUCCESS && capability_result(target, 56) == VK_SUCCESS);
    CHECK(local_size_id_result(target) == VK_ERROR_FEATURE_NOT_PRESENT); /* Version never enables maintenance4. */
    vkrt_shader_interface_finish(target);
    physical_api = VK_API_VERSION_1_2;
    const char *extensions[] = {"VK_KHR_format_feature_flags2", "VK_AMD_gpu_shader_half_float", "VK_KHR_shader_draw_parameters"};
    ci.enabledExtensionCount = VKRT_ARRAY_SIZE(extensions);
    ci.ppEnabledExtensionNames = extensions;
    CHECK(vkrt_shader_interface_init(target, &ci) == VK_SUCCESS);
    CHECK(capability_result(target, 55) == VK_SUCCESS && capability_result(target, 56) == VK_SUCCESS);
    CHECK(capability_result(target, 9) == VK_SUCCESS && capability_result(target, 4427) == VK_SUCCESS);
    vkrt_shader_interface_finish(target);
    CHECK(native_attempts == before);
    target->instance_data = previous;
}
int main(void) {
    struct vkrt_device_data ordinary = {0};
    CHECK(vkrt_shader_interface_init(&ordinary, (void *)(uintptr_t)1) == VK_SUCCESS && !ordinary.shader_interfaces);
    vkrt_shader_interface_lock(&ordinary);
    CHECK(vkrt_shader_interface_track_sampler_locked(&ordinary, H(VkSampler, 1), (void *)(uintptr_t)1) == VK_SUCCESS);
    CHECK(vkrt_shader_interface_track_layout_locked(&ordinary, H(VkDescriptorSetLayout, 1), (void *)(uintptr_t)1) == VK_SUCCESS);
    vkrt_shader_interface_unlock(&ordinary);
    vkrt_shader_interface_finish(&ordinary);
    VkPhysicalDeviceCustomBorderColorFeaturesEXT custom_feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE};
    VkPhysicalDeviceBorderColorSwizzleFeaturesEXT swizzle_feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT,
        .pNext = &custom_feature,
        .borderColorSwizzle = VK_TRUE};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT,
        .pNext = &swizzle_feature,
        .mutableDescriptorType = VK_TRUE};
    VkPhysicalDeviceVulkan11Features v11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .pNext = &mutable_feature,
                                            .samplerYcbcrConversion = VK_TRUE};
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &v11,
                                            .samplerFilterMinmax = VK_TRUE,
                                            .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
                                            .descriptorBindingPartiallyBound = VK_TRUE};
    VkPhysicalDeviceFeatures core = {.samplerAnisotropy = VK_TRUE, .robustBufferAccess = VK_TRUE};
    VkPhysicalDeviceImageRobustnessFeatures image_robustness = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, .pNext = &v12, .robustImageAccess = VK_TRUE};
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                                                          .pNext = &image_robustness,
                                                          .robustBufferAccess2 = VK_TRUE,
                                                          .robustImageAccess2 = VK_TRUE};
    VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &robustness2, .pEnabledFeatures = &core};
    struct vkrt_instance_data instance = {.has_allocator = true, .allocator = source_callbacks};
    struct vkrt_device_data source = {
        .device = H(VkDevice, 1), .instance_data = &instance, .virtual_rt = true, .app_requested_shader_objects = true};
    struct vkrt_device_data target = {.device = H(VkDevice, 2),
                                      .app_requested_shader_objects = true,
                                      .queue_families_have_allocator = true,
                                      .queue_families_allocator = target_callbacks,
                                      .vtable = {.GetDeviceProcAddr = native_proc,
                                                 .CreateDescriptorSetLayout = create_layout,
                                                 .DestroyDescriptorSetLayout = destroy_layout}};
    CHECK(vkrt_shader_interface_init(&source, &ci) == VK_SUCCESS);
    CHECK(vkrt_shader_interface_init(&target, &ci) == VK_SUCCESS);
    size_t target_state_live = target_alloc.live;
    VkSamplerYcbcrConversion conversion = H(VkSamplerYcbcrConversion, UINT64_C(0x1234abcd98760001));
    VkSampler plain = H(VkSampler, UINT64_C(0x1234abcd98760002)), converted = H(VkSampler, UINT64_C(0x1234abcd98760003));
    VkDescriptorSetLayout layout = H(VkDescriptorSetLayout, UINT64_C(0x1234abcd98760004));
    VkSamplerYcbcrConversionCreateInfo conversion_ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
                                                        .format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM,
                                                        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
                                                        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
                                                        .xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
                                                        .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
                                                        .chromaFilter = VK_FILTER_LINEAR};
    VkSamplerReductionModeCreateInfo reduction = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
                                                  .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN};
    VkSamplerCustomBorderColorCreateInfoEXT custom = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                                                      .pNext = &reduction,
                                                      .customBorderColor.float32 = {0.25f, -0.5f, 0.75f, 1.0f},
                                                      .format = VK_FORMAT_R32G32B32A32_SFLOAT};
    VkSamplerBorderColorComponentMappingCreateInfoEXT mapping = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT,
        .pNext = &custom,
        .components = {VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A},
        .srgb = VK_TRUE};
    expected_plain = (VkSamplerCreateInfo){.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                           .pNext = &mapping,
                                           .magFilter = VK_FILTER_LINEAR,
                                           .minFilter = VK_FILTER_NEAREST,
                                           .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                           .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                           .addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                           .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                           .mipLodBias = -0.25f,
                                           .anisotropyEnable = VK_TRUE,
                                           .maxAnisotropy = 4,
                                           .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
                                           .minLod = 0,
                                           .maxLod = 7,
                                           .borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT};
    VkSamplerReductionModeCreateInfo ycbcr_reduction = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    VkSamplerYcbcrConversionInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, .pNext = &ycbcr_reduction, .conversion = conversion};
    expected_ycbcr = (VkSamplerCreateInfo){.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                           .pNext = &conversion_info,
                                           .magFilter = VK_FILTER_LINEAR,
                                           .minFilter = VK_FILTER_LINEAR,
                                           .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                           .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                           .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                           .maxAnisotropy = 1,
                                           .compareOp = VK_COMPARE_OP_ALWAYS};
    VkDescriptorSetLayoutBinding bindings[] = {
        {7, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR,
         (void *)(uintptr_t)1},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &plain},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &converted},
        {99, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, 1, VK_SHADER_STAGE_COMPUTE_BIT, (void *)(uintptr_t)1},
        {100, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 0, 0, (void *)(uintptr_t)1}};
    VkDescriptorBindingFlags binding_flags[] = {
        0, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, 0, 0};
    VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    VkMutableDescriptorTypeListEXT lists[5] = {{UINT32_MAX, (void *)(uintptr_t)1},
                                               {UINT32_MAX, (void *)(uintptr_t)1},
                                               {UINT32_MAX, (void *)(uintptr_t)1},
                                               {2, types},
                                               {UINT32_MAX, (void *)(uintptr_t)1}};
    VkMutableDescriptorTypeCreateInfoEXT mutable = {.sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
                                                    .mutableDescriptorTypeListCount = 5,
                                                    .pMutableDescriptorTypeLists = lists};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .pNext = &mutable,
        .bindingCount = 5,
        .pBindingFlags = binding_flags};
    VkDescriptorSetLayoutCreateInfo layout_ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .pNext = &flag_info,
                                                 .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                                 .bindingCount = 5,
                                                 .pBindings = bindings};
    vkrt_shader_interface_lock(&source);
    CHECK(vkrt_shader_interface_track_conversion_locked(&source, conversion, &conversion_ci) == VK_SUCCESS);
    CHECK(vkrt_shader_interface_track_sampler_locked(&source, plain, &expected_plain) == VK_SUCCESS);
    CHECK(vkrt_shader_interface_track_sampler_locked(&source, plain, &expected_plain) == VK_SUCCESS);
    CHECK(vkrt_shader_interface_track_sampler_locked(&source, converted, &expected_ycbcr) == VK_SUCCESS);
    vkrt_shader_interface_forget_conversion_locked(&source, conversion);
    vkrt_shader_interface_forget_sampler_locked(&source, plain);
    CHECK(vkrt_shader_interface_track_layout_locked(&source, layout, &layout_ci) == VK_SUCCESS);
    CHECK(vkrt_shader_interface_track_layout_locked(&source, layout, &layout_ci) == VK_SUCCESS);
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    CHECK(vkrt_shader_interface_track_layout_locked(&source, layout, &layout_ci) ==
          VK_SUCCESS); /* Native-equivalent handle alias. */
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkrt_shader_interface_forget_sampler_locked(&source, plain);
    vkrt_shader_interface_forget_sampler_locked(&source, converted);
    vkrt_shader_interface_forget_layout_locked(&source, layout);
    vkrt_shader_interface_unlock(&source);
    VkDescriptorSetLayout sets[] = {layout, layout};
    VkPushConstantRange push[] = {{VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR, 12, 20},
                                  {VK_SHADER_STAGE_MISS_BIT_KHR, 32, 16}};
    VkShaderCreateInfoEXT shader = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                    .setLayoutCount = 2,
                                    .pSetLayouts = sets,
                                    .pushConstantRangeCount = 2,
                                    .pPushConstantRanges = push};
    void *bytes = NULL;
    size_t size = 0;
    CHECK(vkrt_shader_interface_capture(&source, &shader, &bytes, &size) == VK_SUCCESS);
    CHECK(size > 32 && !memcmp(bytes, "VKFI", 4) && read32((const uint8_t *)bytes + 4) == 2 &&
          read32((const uint8_t *)bytes + 24) == 15 && read32((const uint8_t *)bytes + 28) == 1 &&
          vkrt_shader_interface_validate(bytes, size) == VK_SUCCESS);
    CHECK(!contains_handle(bytes, size, (uintptr_t)plain) && !contains_handle(bytes, size, (uintptr_t)converted) &&
          !contains_handle(bytes, size, (uintptr_t)conversion) && !contains_handle(bytes, size, (uintptr_t)layout));
    size_t baseline = source_alloc.live;
    source_alloc.fail_at = source_alloc.attempts;
    void *failed = (void *)(uintptr_t)1;
    size_t failed_size = 99;
    CHECK(vkrt_shader_interface_capture(&source, &shader, &failed, &failed_size) == VK_ERROR_OUT_OF_HOST_MEMORY && !failed &&
          !failed_size && source_alloc.live == baseline);
    source_alloc.fail_at = SIZE_MAX;
    vkrt_shader_interface_lock(&source);
    vkrt_shader_interface_forget_layout_locked(&source, layout);
    vkrt_shader_interface_forget_layout_locked(&source, layout);
    vkrt_shader_interface_unlock(&source);
    struct vkrt_shader_interface *owned = NULL;
    CHECK(vkrt_shader_interface_materialize(&target, bytes, size, &owned) == VK_SUCCESS);
    VkShaderCreateInfoEXT materialized = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                          .flags = VK_SHADER_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT};
    vkrt_shader_interface_apply(owned, &materialized);
    CHECK(materialized.setLayoutCount == 2 && materialized.pSetLayouts[0] != layout && materialized.pushConstantRangeCount == 1 &&
          materialized.pPushConstantRanges[0].stageFlags == VK_SHADER_STAGE_COMPUTE_BIT &&
          materialized.pPushConstantRanges[0].offset == 12 && materialized.pPushConstantRanges[0].size == 20 &&
          materialized.flags == VK_SHADER_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT && !target.virtual_rt);
    vkrt_shader_interface_destroy(&target, owned);
    CHECK(target_alloc.live == target_state_live);
    /* A reused native handle receives its new definition without changing an
     * already captured packet. Unknown optional data remains explicitly unknown. */
    VkDescriptorSetLayoutBinding reused_binding = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo reused_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &reused_binding};
    VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    VkDescriptorSetLayoutCreateInfo unknown_ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .pNext = &unknown,
                                                  .bindingCount = 0,
                                                  .pBindings = (void *)(uintptr_t)1};
    VkDescriptorSetLayout unknown_layout = H(VkDescriptorSetLayout, 0x1234567);
    vkrt_shader_interface_lock(&source);
    CHECK(vkrt_shader_interface_track_layout_locked(&source, layout, &reused_ci) == VK_SUCCESS);
    CHECK(vkrt_shader_interface_track_layout_locked(&source, unknown_layout, &unknown_ci) == VK_SUCCESS);
    vkrt_shader_interface_unlock(&source);
    VkShaderCreateInfoEXT changed_shader = {
        .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .setLayoutCount = 1, .pSetLayouts = &unknown_layout};
    void *changed = (void *)(uintptr_t)1;
    size_t changed_size = 99;
    CHECK(vkrt_shader_interface_capture(&source, &changed_shader, &changed, &changed_size) == VK_ERROR_FEATURE_NOT_PRESENT &&
          !changed && !changed_size);
    changed_shader.pSetLayouts = &layout;
    CHECK(vkrt_shader_interface_capture(&source, &changed_shader, &changed, &changed_size) == VK_SUCCESS && changed_size < size);
    vkrt_shader_interface_free_bytes(&source, changed);
    CHECK(vkrt_shader_interface_materialize(&target, bytes, size, &owned) == VK_SUCCESS);
    vkrt_shader_interface_destroy(&target, owned);
    vkrt_shader_interface_lock(&source);
    vkrt_shader_interface_forget_layout_locked(&source, layout);
    vkrt_shader_interface_forget_layout_locked(&source, unknown_layout);
    vkrt_shader_interface_unlock(&source);
    for (size_t budget = 0; budget < 35; ++budget) {
        target_alloc.fail_at = target_alloc.attempts + budget;
        VkResult result = vkrt_shader_interface_materialize(&target, bytes, size, &owned);
        CHECK(result == VK_SUCCESS || result == VK_ERROR_OUT_OF_HOST_MEMORY);
        if (result == VK_SUCCESS) vkrt_shader_interface_destroy(&target, owned);
        CHECK(target_alloc.live == target_state_live && made_layouts == freed_layouts && made_samplers == freed_samplers &&
              made_conversions == freed_conversions);
        target_alloc.fail_at = SIZE_MAX;
    }
    for (unsigned failure = 0; failure < 8; ++failure) {
        native_fail_at = native_attempts + failure;
        CHECK(vkrt_shader_interface_materialize(&target, bytes, size, &owned) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        CHECK(!owned && target_alloc.live == target_state_live && made_layouts == freed_layouts &&
              made_samplers == freed_samplers && made_conversions == freed_conversions);
    }
    native_fail_at = UINT_MAX;
    uint8_t *corrupt = malloc(size);
    memcpy(corrupt, bytes, size);
    corrupt[size - 1] = 1;
    unsigned before = native_attempts;
    CHECK(vkrt_shader_interface_materialize(&target, corrupt, size, &owned) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT && !owned &&
          native_attempts == before);
    for (size_t end = 0; end < size; ++end)
        CHECK(vkrt_shader_interface_validate(bytes, end) == VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
    free(corrupt);
    malformed_interfaces(&target, bytes, size);
    struct vkrt_device_data ordinary_profile = {.app_requested_shader_objects = true, .instance_data = &instance};
    CHECK(vkrt_shader_interface_init(&ordinary_profile, NULL) == VK_SUCCESS);
    VkShaderCreateInfoEXT empty_shader = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                          .pSetLayouts = (void *)(uintptr_t)1,
                                          .pPushConstantRanges = (void *)(uintptr_t)1};
    void *empty_bytes = NULL;
    size_t empty_size = 0;
    CHECK(vkrt_shader_interface_capture(&ordinary_profile, &empty_shader, &empty_bytes, &empty_size) == VK_SUCCESS);
    CHECK(empty_size == 32 && ((const uint8_t *)empty_bytes)[24] == 0 && ((const uint8_t *)empty_bytes)[28] == 0);
    before = native_attempts;
    target.virtual_rt = true;
    CHECK(vkrt_shader_interface_materialize(&target, empty_bytes, empty_size, &owned) == VK_SUCCESS && native_attempts == before);
    vkrt_shader_interface_apply(owned, &empty_shader);
    CHECK(!empty_shader.setLayoutCount && !empty_shader.pushConstantRangeCount && !empty_shader.pSetLayouts &&
          !empty_shader.pPushConstantRanges);
    vkrt_shader_interface_destroy(&target, owned);
    target.virtual_rt = false;
    vkrt_shader_interface_free_bytes(&ordinary_profile, empty_bytes);
    vkrt_shader_interface_finish(&ordinary_profile);
    vkrt_shader_interface_finish(&target);
    VkBool32 *requirements[] = {&core.robustBufferAccess,
                                &robustness2.robustBufferAccess2,
                                &image_robustness.robustImageAccess,
                                &robustness2.robustImageAccess2,
                                &v12.descriptorBindingSampledImageUpdateAfterBind,
                                &v12.descriptorBindingPartiallyBound,
                                &core.samplerAnisotropy,
                                &v11.samplerYcbcrConversion,
                                &v12.samplerFilterMinmax,
                                &custom_feature.customBorderColors,
                                &swizzle_feature.borderColorSwizzle,
                                &mutable_feature.mutableDescriptorType};
    for (unsigned i = 0; i < sizeof(requirements) / sizeof(requirements[0]); ++i) {
        *requirements[i] = VK_FALSE;
        CHECK(vkrt_shader_interface_init(&target, &ci) == VK_SUCCESS);
        before = native_attempts;
        CHECK(vkrt_shader_interface_materialize(&target, bytes, size, &owned) == VK_ERROR_FEATURE_NOT_PRESENT && !owned &&
              native_attempts == before);
        vkrt_shader_interface_finish(&target);
        *requirements[i] = VK_TRUE;
    }
    source_profiles(&target, &ci, &core);
    capability_profiles(&target);
    vkrt_shader_interface_free_bytes(&source, bytes);
    vkrt_shader_interface_finish(&source);
    CHECK(!source_alloc.live);
    check_retired();
    if (!failures)
        printf(
            "Shader interfaces: PASS (handle-free LE recipes, sampler/conversion snapshots, native aliases, cross-profile "
            "translation, allocator/native rollback, source-capability aliases/LocalSizeId, preflight; %u checks)\n",
            checks);
    return failures ? 1 : 0;
}
