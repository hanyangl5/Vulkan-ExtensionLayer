// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
layout(buffer_reference, std430, buffer_reference_align = 4) buffer SourceData { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer SourceWideData { uvec2 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer SourcePointerPair { SourceWideData pair[2]; };
struct SourceRecord { SourceData a; SourceData b; uint index; uint pad; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer SourceRecords { SourceRecord rec[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer SourceOutput { uint v[]; };
layout(push_constant, std430) uniform SourcePush {
    uint64_t records;
    uint64_t outputAddress;
    uint seed;
    uint pad0;
    uint pad1;
    uint pad2;
} sourcePush;
