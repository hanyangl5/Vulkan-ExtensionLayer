// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
layout(buffer_reference, std430, buffer_reference_align = 4) coherent buffer SourceData { uint v[]; };
struct SourceRecord { SourceData a; SourceData b; uint index; uint pad; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer SourceRecords { SourceRecord rec[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer SourceOutput { uint v[]; };
struct PointerPack { SourceData refs[2]; uint tag; uint pad; };
struct Transfer { PointerPack groups[2]; SourceData chosen; uint index; uint total; uint depth; uint pad; };
layout(push_constant, std430) uniform SourcePush {
    uint64_t records;
    uint64_t outputAddress;
    uint seed;
    uint pad0;
    uint pad1;
    uint pad2;
} sourcePush;
uint sourceInvocation() {
    return gl_LaunchIDEXT.x + gl_LaunchSizeEXT.x * (gl_LaunchIDEXT.y + gl_LaunchSizeEXT.y * gl_LaunchIDEXT.z);
}
