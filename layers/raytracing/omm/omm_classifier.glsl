// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#ifndef VEL_OMM_CLASSIFIER_GLSL
#define VEL_OMM_CLASSIFIER_GLSL

#include "omm_lookup.glsl"
#include "omm_opacity.glsl"

// Runtime wire ABI v1, matched to omm_metadata.h. Scene addresses refer to
// layer-owned 64-byte RootCells. Native AS addresses are never dereferenced.
// Invalid metadata is a debug failure, not permission to treat an unknown AS as
// ordinary geometry. The scalar callback returns Ignore on that failure until
// the public adapter has established a complete, valid provenance contract.
const uint OMM_METADATA_ROOT_MAGIC = 0x434d4d4fu;
const uint OMM_METADATA_BODY_MAGIC = 0x424d4d4fu;
const uint OMM_METADATA_VERSION = 1u;
const uint OMM_METADATA_BLAS = 1u;
const uint OMM_METADATA_TLAS = 2u;

struct OmmMetadataBodyView {
    uint64_t address;
    uint64_t activeBytes;
    uint64_t count;
    uint64_t recordsEnd;
};

uint ommClassifyBody(uint64_t root, uint kind, inout OmmMetadataBodyView view) {
    if (!ommLookupSpanValid(root, 64ul)) return OMM_LOOKUP_INVALID;
    if (ommLookupRead32(root, 0ul) != OMM_METADATA_ROOT_MAGIC || ommLookupRead32(root, 4ul) != OMM_METADATA_VERSION)
        return OMM_LOOKUP_INCOMPATIBLE;
    if (ommLookupRead32(root, 8ul) != kind || ommLookupRead32(root, 12ul) != 1u || ommLookupRead64(root, 40ul) != 0ul ||
        ommLookupRead64(root, 48ul) != 0ul || ommLookupRead64(root, 56ul) != 0ul)
        return OMM_LOOKUP_INVALID;
    uint64_t address = ommLookupRead64(root, 16ul);
    uint64_t capacity = ommLookupRead64(root, 24ul);
    uint64_t generation = ommLookupRead64(root, 32ul);
    if (!ommLookupSpanValid(address, capacity) || capacity < 64ul || (capacity & 15ul) != 0ul) return OMM_LOOKUP_INVALID;
    if (ommLookupRead32(address, 0ul) != OMM_METADATA_BODY_MAGIC || ommLookupRead32(address, 4ul) != OMM_METADATA_VERSION)
        return OMM_LOOKUP_INCOMPATIBLE;
    uint stride = kind == OMM_METADATA_TLAS ? 32u : 64u;
    uint64_t activeBytes = ommLookupRead64(address, 16ul);
    uint64_t bodyCapacity = ommLookupRead64(address, 24ul);
    uint64_t count = ommLookupRead64(address, 32ul);
    if (ommLookupRead32(address, 8ul) != kind || ommLookupRead32(address, 12ul) != stride || bodyCapacity < 64ul ||
        bodyCapacity > capacity || (bodyCapacity & 15ul) != 0ul || ommLookupRead64(address, 40ul) != 64ul ||
        ommLookupRead32(address, 52ul) != 0u || ommLookupRead64(address, 56ul) != generation)
        return OMM_LOOKUP_INVALID;
    if (activeBytes < 64ul || activeBytes > bodyCapacity || (activeBytes & 15ul) != 0ul ||
        count > (activeBytes - 64ul) / uint64_t(stride))
        return OMM_LOOKUP_BOUNDS;
    view = OmmMetadataBodyView(address, activeBytes, count, 64ul + count * uint64_t(stride));
    return OMM_LOOKUP_SUCCESS;
}

bool ommClassifyPayload(OmmMetadataBodyView body, uint64_t offset, uint64_t bytes, uint64_t alignment) {
    return offset >= body.recordsEnd && (offset & (alignment - 1ul)) == 0ul && offset <= body.activeBytes &&
           bytes <= body.activeBytes - offset;
}

uint ommClassifyChecked(uvec2 scene, uint instance, uint geometry, uint primitive, vec2 bary, bool triangle, uint logicalFlags,
                        inout uint decision) {
    uint opacityFlags = logicalFlags & (0x1u | 0x2u | 0x40u | 0x80u);
    if (opacityFlags != 0u && (opacityFlags & (opacityFlags - 1u)) != 0u) return OMM_LOOKUP_INVALID;
    uint64_t sceneRoot = uint64_t(scene.x) | (uint64_t(scene.y) << 32u);
    OmmMetadataBodyView tlas;
    uint status = ommClassifyBody(sceneRoot, OMM_METADATA_TLAS, tlas);
    if (status != OMM_LOOKUP_SUCCESS) return status;
    if (uint64_t(instance) >= tlas.count) return OMM_LOOKUP_BOUNDS;
    uint64_t record = 64ul + uint64_t(instance) * 32ul;
    uint64_t blasRoot = ommLookupRead64(tlas.address, record);
    uint64_t nativeBlas = ommLookupRead64(tlas.address, record + 8ul);
    uint instanceFlags = ommLookupRead32(tlas.address, record + 16ul);
    uint instanceState = ommLookupRead32(tlas.address, record + 20ul);
    if (instanceState > 1u || ommLookupRead64(tlas.address, record + 24ul) != 0ul || (instanceFlags & ~0x3fu) != 0u ||
        (instanceFlags & 0xcu) == 0xcu)
        return OMM_LOOKUP_INVALID;
    if (instanceState == 0u) {
        decision = 0u;
        return OMM_LOOKUP_SUCCESS;
    }
    if (nativeBlas == 0ul) return OMM_LOOKUP_INVALID;
    OmmMetadataBodyView blas;
    status = ommClassifyBody(blasRoot, OMM_METADATA_BLAS, blas);
    if (status != OMM_LOOKUP_SUCCESS) return status;
    if (uint64_t(geometry) >= blas.count) return OMM_LOOKUP_BOUNDS;
    record = 64ul + uint64_t(geometry) * 64ul;
    uint geometryType = ommLookupRead32(blas.address, record);
    uint geometryFlags = ommLookupRead32(blas.address, record + 4ul);
    uint64_t primitiveCount = ommLookupRead64(blas.address, record + 8ul);
    uint64_t associationOffset = ommLookupRead64(blas.address, record + 16ul);
    uint64_t associationBytes = ommLookupRead64(blas.address, record + 24ul);
    uint64_t snapshotOffset = ommLookupRead64(blas.address, record + 32ul);
    uint64_t snapshotCapacity = ommLookupRead64(blas.address, record + 40ul);
    uint associationFlags = ommLookupRead32(blas.address, record + 48ul);
    if (geometryType != (triangle ? 0u : 1u) || (geometryFlags & ~3u) != 0u || (associationFlags & ~1u) != 0u ||
        ommLookupRead32(blas.address, record + 52ul) != 0u || ommLookupRead64(blas.address, record + 56ul) != 0ul)
        return OMM_LOOKUP_INVALID;
    if (uint64_t(primitive) >= primitiveCount) return OMM_LOOKUP_BOUNDS;
    bool associated = (associationFlags & 1u) != 0u;
    if (!triangle && associated) return OMM_LOOKUP_INVALID;
    uint state = 0u;
    if (associated && (instanceFlags & 0x20u) == 0u) {
        if (primitiveCount > (~0ul >> 3u) || associationBytes != primitiveCount * 8ul ||
            !ommClassifyPayload(blas, associationOffset, associationBytes, 8ul))
            return OMM_LOOKUP_BOUNDS;
        uint64_t index = ommLookupRead64(blas.address, associationOffset + uint64_t(primitive) * 8ul);
        if (index >= ~0ul - 3ul) {
            state = uint(~0ul - index);
        } else {
            if (snapshotCapacity < 64ul || !ommClassifyPayload(blas, snapshotOffset, snapshotCapacity, 16ul))
                return OMM_LOOKUP_BOUNDS;
            uint64_t snapshotAddress = blas.address + snapshotOffset;
            uint64_t snapshotBytes = ommLookupRead64(snapshotAddress, 16ul);
            if (snapshotBytes > snapshotCapacity) return OMM_LOOKUP_BOUNDS;
            uint microtriangle = 0u;
            status = ommLookupTriangle(OmmStorageSpan(snapshotAddress, snapshotBytes), index, bary, state, microtriangle);
            if (status != OMM_LOOKUP_SUCCESS) return status;
        }
    }
    decision = ommResolveOpacity(state, associated, logicalFlags, instanceFlags, geometryFlags);
    return OMM_LOOKUP_SUCCESS;
}

uint ommClassify(uvec2 scene, uint instance, uint geometry, uint primitive, vec2 bary, bool triangle, uint logicalFlags) {
    uint decision = 0u;
    uint status = ommClassifyChecked(scene, instance, geometry, primitive, bary, triangle, logicalFlags, decision);
    return status == OMM_LOOKUP_SUCCESS ? decision : 0u;
}

#endif
