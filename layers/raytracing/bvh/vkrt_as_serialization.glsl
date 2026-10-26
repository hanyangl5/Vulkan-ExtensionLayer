/* SPDX-License-Identifier: MIT */
#ifndef VKRT_AS_SERIALIZATION_GLSL
#define VKRT_AS_SERIALIZATION_GLSL
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#define VKRT_AS_SERIAL_HEADER_SIZE 56ul
#define VKRT_AS_SERIAL_HANDLES_OFFSET 56ul
uint64_t vkrt_as_serialization_payload(uint instance_count) {
    return (VKRT_AS_SERIAL_HEADER_SIZE + 8ul * uint64_t(instance_count) + 15ul) & ~15ul;
}
uint64_t vkrt_as_serialization_size(uint64_t physical_size, uint instance_count) {
    uint64_t payload = vkrt_as_serialization_payload(instance_count);
    return physical_size <= 0xfffffffffffffffful - payload ? payload + physical_size : 0ul;
}
#endif
