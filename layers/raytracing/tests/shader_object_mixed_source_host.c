/* SPDX-License-Identifier: MIT
 * Actual public ShaderObject hooks and rewriter; native ICD/context are spies.
 */
#include "vkrt_layer.h"
#include "vkrt_shader_objects.h"
#include "vkrt_shader_interface.h"
#include "vkrt_rtp.h"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static unsigned native_creates, native_destroys;
static uintptr_t next_shader = 100;
extern int vkrt_mixed_native_words(const uint32_t *, size_t);
bool vkrt_native_backend_eligible(struct vkrt_instance_data *inst, VkPhysicalDevice physical) {
    (void)inst;
    (void)physical;
    return true;
}
VkResult vkrt_as_registry_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    dev->as_registry = (void *)(uintptr_t)1;
    return VK_SUCCESS;
}
VkDeviceAddress vkrt_as_registry_address(struct vkrt_device_data *dev) {
    (void)dev;
    return 4096;
}
VkDescriptorSetLayout vkrt_rtp_native_descriptor_layout(struct vkrt_device_data *dev, VkDescriptorSetLayout layout) {
    (void)dev;
    return layout;
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *info) {
    (void)physical;
    for (VkBaseOutStructure *p = info->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT) {
            VkPhysicalDeviceShaderObjectPropertiesEXT *shader = (void *)p;
            memset(shader->shaderBinaryUUID, 0x53, VK_UUID_SIZE);
            shader->shaderBinaryVersion = 1;
        }
}
static VKAPI_ATTR VkResult VKAPI_CALL native_create(VkDevice device, uint32_t count, const VkShaderCreateInfoEXT *infos,
                                                    const VkAllocationCallbacks *allocator, VkShaderEXT *out) {
    (void)device;
    (void)allocator;
    ++native_creates;
    for (uint32_t i = 0; i < count; ++i) {
        if (infos[i].codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT && !vkrt_mixed_native_words(infos[i].pCode, infos[i].codeSize / 4))
            return VK_ERROR_INVALID_SHADER_NV;
        out[i] = (VkShaderEXT)next_shader++;
    }
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL native_destroy(VkDevice device, VkShaderEXT shader, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)shader;
    (void)allocator;
    ++native_destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL native_binary(VkDevice device, VkShaderEXT shader, size_t *size, void *data) {
    (void)device;
    (void)shader;
    if (data) {
        if (*size < 4) return VK_INCOMPLETE;
        memcpy(data, "NRT!", 4);
    }
    *size = 4;
    return VK_SUCCESS;
}
static uint32_t read32(const uint8_t *bytes, size_t offset) {
    return bytes[offset] | (uint32_t)bytes[offset + 1] << 8 | (uint32_t)bytes[offset + 2] << 16 | (uint32_t)bytes[offset + 3] << 24;
}
int vkrt_mixed_public_shader(const uint32_t *words, size_t count, const char *label) {
    struct vkrt_device_data dev = {0};
    struct vkrt_instance_data inst = {0};
    dev.device = (VkDevice)(uintptr_t)1;
    dev.physical_device = (VkPhysicalDevice)(uintptr_t)2;
    dev.instance_data = &inst;
    dev.virtual_rt = dev.shader_object_backend = dev.app_requested_shader_objects = true;
    dev.as_registry = (void *)(uintptr_t)1;
    dev.vtable.CreateShadersEXT = native_create;
    dev.vtable.DestroyShaderEXT = native_destroy;
    dev.vtable.GetShaderBinaryDataEXT = native_binary;
    inst.vtable.GetPhysicalDeviceProperties2 = properties;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .rayTracingPipeline = VK_TRUE};
    VkPhysicalDeviceShaderObjectFeaturesEXT object = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, .pNext = &rt, .shaderObject = VK_TRUE};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &object};
    if (vkrt_shader_interface_init(&dev, &device) != VK_SUCCESS || vkrt_shader_objects_init(&dev) != VK_SUCCESS) return 1;
    vkrt_map_object(VKRT_HKEY(dev.device), &dev);
    PFN_vkCreateShadersEXT create = (PFN_vkCreateShadersEXT)vkrt_shader_object_proc("vkCreateShadersEXT");
    PFN_vkDestroyShaderEXT destroy = (PFN_vkDestroyShaderEXT)vkrt_shader_object_proc("vkDestroyShaderEXT");
    PFN_vkGetShaderBinaryDataEXT binary = (PFN_vkGetShaderBinaryDataEXT)vkrt_shader_object_proc("vkGetShaderBinaryDataEXT");
    VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                  .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                  .codeSize = count * 4,
                                  .pCode = words,
                                  .pName = "main"};
    VkShaderCreateInfoEXT original_info = info;
    unsigned before = native_creates, destroyed = native_destroys;
    VkShaderEXT shader = VK_NULL_HANDLE;
    VkResult result = create(dev.device, 1, &info, NULL, &shader);
    printf("%s: CreateShadersEXT=%d nativeCalls=%u output=%llu\n", label, (int)result, native_creates - before,
           (unsigned long long)(uintptr_t)shader);
    int failed = result != VK_SUCCESS || !shader;
    if (shader) {
        size_t size = 0;
        if (binary(dev.device, shader, &size, NULL) != VK_SUCCESS) failed = 1;
        uint8_t *bytes = malloc(size);
        if (!bytes || binary(dev.device, shader, &size, bytes) != VK_SUCCESS) failed = 1;
        if (bytes) {
            /* FORMAT3 source/group provenance must describe selected NoRT
             * code; this is the transport contract, independent of ICD bytes. */
            if (size < 224 || read32(bytes, 8) != 3 || read32(bytes, 92) || read32(bytes, 128 + 76))
                failed = 1;
            else {
                info.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
                info.codeSize = size;
                info.pCode = bytes;
                VkShaderEXT imported = VK_NULL_HANDLE;
                if (create(dev.device, 1, &info, NULL, &imported) != VK_SUCCESS || !imported) failed = 1;
                if (imported) destroy(dev.device, imported, NULL);
                /* An independent source/binary batch forces original_group
                 * temporary compilation as well as final opaque forwarding. */
                VkShaderCreateInfoEXT mixed[] = {original_info, info}, saved[2];
                memcpy(saved, mixed, sizeof(saved));
                VkShaderEXT shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
                if (create(dev.device, 2, mixed, NULL, shaders) != VK_SUCCESS || !shaders[0] || !shaders[1] ||
                    memcmp(saved, mixed, sizeof(saved)))
                    failed = 1;
                for (unsigned i = 0; i < 2; ++i)
                    if (shaders[i]) destroy(dev.device, shaders[i], NULL);
            }
            free(bytes);
        }
        destroy(dev.device, shader, NULL);
    }
    if (!failed && (native_creates != before + 4 || native_destroys != destroyed + 5)) failed = 1;
    vkrt_shader_objects_finish(&dev);
    vkrt_shader_interface_finish(&dev);
    vkrt_unmap_object(VKRT_HKEY(dev.device));
    return failed;
}
