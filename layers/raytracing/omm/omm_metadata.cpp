// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "omm_metadata.h"
#include "omm_opacity.h"
#include "omm_storage.h"

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace vel::omm {
namespace {
bool add(uint64_t a, uint64_t b, uint64_t* result) {
    if (b > UINT64_MAX - a) return false;
    *result = a + b;
    return true;
}
bool mul(uint64_t a, uint64_t b, uint64_t* result) {
    if (a && b > UINT64_MAX / a) return false;
    *result = a * b;
    return true;
}
bool aligned16(uint64_t value, uint64_t* result) {
    if (!add(value, 15, result)) return false;
    *result &= ~uint64_t(15);
    return true;
}
uint32_t get32(const uint8_t* data) {
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}
uint64_t get64(const uint8_t* data) { return get32(data) | uint64_t(get32(data + 4)) << 32; }
void put32(uint8_t* data, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[i] = uint8_t(value >> (8 * i));
}
void put64(uint8_t* data, uint64_t value) {
    put32(data, uint32_t(value));
    put32(data + 4, uint32_t(value >> 32));
}
bool span(uint64_t offset, uint64_t bytes, uint64_t minimum, uint64_t total, uint64_t alignment) {
    return offset >= minimum && !(offset & (alignment - 1)) && offset <= total && bytes <= total - offset;
}
MetadataResult allocate(uint64_t size, std::vector<uint8_t>* output) {
    if (size > SIZE_MAX || size > output->max_size()) return MetadataResult::Overflow;
    try {
        output->resize(size_t(size), 0);
    } catch (const std::bad_alloc&) {
        return MetadataResult::OutOfMemory;
    } catch (const std::length_error&) {
        return MetadataResult::Overflow;
    }
    return MetadataResult::Success;
}
void write_header(uint8_t* bytes, uint32_t kind, uint32_t stride, uint64_t size, uint64_t count, uint32_t flags,
                  uint64_t generation) {
    put32(bytes, kMetadataBodyMagic);
    put32(bytes + 4, kMetadataVersion);
    put32(bytes + 8, kind);
    put32(bytes + 12, stride);
    put64(bytes + 16, size);
    put64(bytes + 24, size);
    put64(bytes + 32, count);
    put64(bytes + 40, 64);
    put32(bytes + 48, flags);
    put32(bytes + 52, 0);
    put64(bytes + 56, generation);
}
void write_geometry(uint8_t* data, const MetadataGeometry& geometry) {
    put32(data, geometry.geometry_type);
    put32(data + 4, geometry.geometry_flags);
    put64(data + 8, geometry.primitive_count);
    put64(data + 16, geometry.association_offset);
    put64(data + 24, geometry.association_bytes);
    put64(data + 32, geometry.snapshot_offset);
    put64(data + 40, geometry.snapshot_capacity);
    put32(data + 48, geometry.association_flags);
    put32(data + 52, 0);
    put64(data + 56, 0);
}
MetadataGeometry read_geometry(const uint8_t* data) {
    return {get32(data),      get32(data + 4),  get64(data + 8),  get64(data + 16), get64(data + 24),
            get64(data + 32), get64(data + 40), get32(data + 48), get32(data + 52), get64(data + 56)};
}
MetadataResult append_geometry(const MetadataGeometryPlan& plan, uint64_t* cursor, MetadataGeometry* output) {
    if ((plan.geometry_type != kMetadataTriangles && plan.geometry_type != kMetadataAabbs) || (plan.geometry_flags & ~3u) ||
        (plan.association && plan.geometry_type != kMetadataTriangles) || (!plan.association && plan.snapshot_capacity))
        return MetadataResult::InvalidInput;
    MetadataGeometry geometry{};
    geometry.geometry_type = plan.geometry_type;
    geometry.geometry_flags = plan.geometry_flags;
    geometry.primitive_count = plan.primitive_count;
    if (plan.association) {
        geometry.association_flags = kMetadataAssociation;
        if (plan.primitive_count) {
            geometry.association_offset = *cursor;
            if (!mul(plan.primitive_count, 8, &geometry.association_bytes) || !add(*cursor, geometry.association_bytes, cursor) ||
                !aligned16(*cursor, cursor))
                return MetadataResult::Overflow;
        }
        if (plan.snapshot_capacity) {
            if (plan.snapshot_capacity < 64) return MetadataResult::InvalidInput;
            geometry.snapshot_offset = *cursor;
            if (!aligned16(plan.snapshot_capacity, &geometry.snapshot_capacity) ||
                !add(*cursor, geometry.snapshot_capacity, cursor))
                return MetadataResult::Overflow;
        }
    }
    if (output) *output = geometry;
    return MetadataResult::Success;
}
MetadataResult source_snapshot(const MetadataGeometryInput& input, uint64_t* active, uint64_t* triangles) {
    *active = *triangles = 0;
    if (!input.plan.snapshot_capacity) return MetadataResult::Success;
    if (!input.snapshot || input.snapshot_size < 64) return MetadataResult::OutOfBounds;
    const uint64_t size = get64(input.snapshot + 16);
    if (size > input.snapshot_size || size > input.plan.snapshot_capacity) return MetadataResult::OutOfBounds;
    const auto status = storage_validate(input.snapshot, size_t(size));
    if (status != StorageResult::Success)
        return status == StorageResult::Incompatible ? MetadataResult::Incompatible : MetadataResult::InvalidInput;
    *active = size;
    *triangles = get64(input.snapshot + 24);
    return MetadataResult::Success;
}
MetadataResult normalize(const MetadataGeometryInput& input, uint64_t primitive, uint64_t triangle_count, uint64_t* output) {
    uint32_t raw = 0;
    if (!input.index_width) {
        if (primitive > UINT32_MAX) return MetadataResult::OutOfBounds;
        raw = uint32_t(primitive);
    } else {
        if ((input.index_width != 16 && input.index_width != 32) || input.index_stride > UINT32_MAX ||
            input.index_stride % (input.index_width / 8))
            return MetadataResult::InvalidInput;
        uint64_t offset;
        const uint32_t width = input.index_width / 8;
        if (!mul(primitive, input.index_stride, &offset)) return MetadataResult::Overflow;
        if (!input.indices || offset > input.indices_size || width > input.indices_size - offset)
            return MetadataResult::OutOfBounds;
        for (uint32_t i = 0; i < width; ++i) raw |= uint32_t(input.indices[size_t(offset) + i]) << (8 * i);
    }
    TriangleIndex index;
    if (decode_index(raw, input.index_width ? input.index_width : 32, input.base_triangle, &index) != Status::Success)
        return MetadataResult::InvalidInput;
    if (index.special)
        *output = metadata_special(uint32_t(index.state));
    else {
        if (!input.plan.snapshot_capacity || index.triangle >= triangle_count) return MetadataResult::OutOfBounds;
        *output = index.triangle;
    }
    return MetadataResult::Success;
}
}  // namespace

MetadataResult metadata_plan_blas(const MetadataGeometryPlan* plans, size_t count, MetadataGeometry* geometries,
                                  size_t geometry_capacity, uint64_t* bytes) {
    if (!bytes || (count && (!plans || !geometries))) return MetadataResult::InvalidInput;
    if (count > geometry_capacity) return MetadataResult::OutOfBounds;
    uint64_t records, start;
    if (!mul(count, sizeof(MetadataGeometry), &records) || !add(64, records, &start)) return MetadataResult::Overflow;
    uint64_t cursor = start;
    for (size_t i = 0; i < count; ++i) {
        const auto result = append_geometry(plans[i], &cursor, nullptr);
        if (result != MetadataResult::Success) return result;
    }
    *bytes = cursor;
    cursor = start;
    for (size_t i = 0; i < count; ++i) append_geometry(plans[i], &cursor, &geometries[i]);
    return MetadataResult::Success;
}

MetadataResult metadata_pack_blas(const MetadataGeometryInput* inputs, size_t count, uint32_t flags, uint64_t generation,
                                  std::vector<uint8_t>* output) {
    if (!output || (count && !inputs)) return MetadataResult::InvalidInput;
    try {
        std::vector<MetadataGeometryPlan> plans;
        std::vector<MetadataGeometry> geometries(count);
        plans.reserve(count);
        for (size_t i = 0; i < count; ++i) plans.push_back(inputs[i].plan);
        uint64_t bytes;
        auto result = metadata_plan_blas(plans.data(), count, geometries.data(), geometries.size(), &bytes);
        if (result != MetadataResult::Success) return result;
        std::vector<uint8_t> built;
        result = allocate(bytes, &built);
        if (result != MetadataResult::Success) return result;
        write_header(built.data(), kMetadataBlas, 64, bytes, count, flags, generation);
        for (size_t i = 0; i < count; ++i) {
            const auto& geometry = geometries[i];
            write_geometry(built.data() + 64 + i * 64, geometry);
            if (!geometry.association_flags) continue;
            uint64_t active, triangle_count;
            result = source_snapshot(inputs[i], &active, &triangle_count);
            if (result != MetadataResult::Success) return result;
            if (active) std::memcpy(built.data() + size_t(geometry.snapshot_offset), inputs[i].snapshot, size_t(active));
            for (uint64_t primitive = 0; primitive < geometry.primitive_count; ++primitive) {
                uint64_t index;
                result = normalize(inputs[i], primitive, triangle_count, &index);
                if (result != MetadataResult::Success) return result;
                put64(built.data() + size_t(geometry.association_offset + primitive * 8), index);
            }
        }
        output->swap(built);
        return MetadataResult::Success;
    } catch (const std::bad_alloc&) {
        return MetadataResult::OutOfMemory;
    } catch (const std::length_error&) {
        return MetadataResult::Overflow;
    }
}

MetadataResult metadata_pack_tlas(const MetadataInstance* instances, size_t count, uint32_t flags, uint64_t generation,
                                  std::vector<uint8_t>* output) {
    if (!output || (count && !instances)) return MetadataResult::InvalidInput;
    uint64_t records, bytes;
    if (!mul(count, sizeof(MetadataInstance), &records) || !add(64, records, &bytes)) return MetadataResult::Overflow;
    std::vector<uint8_t> built;
    auto result = allocate(bytes, &built);
    if (result != MetadataResult::Success) return result;
    write_header(built.data(), kMetadataTlas, 32, bytes, count, flags, generation);
    for (size_t i = 0; i < count; ++i) {
        const auto& instance = instances[i];
        auto* data = built.data() + 64 + i * 32;
        put64(data, instance.blas_root_cell_address);
        put64(data + 8, instance.native_blas_address);
        put32(data + 16, instance.original_flags);
        put32(data + 20, instance.state);
        put64(data + 24, instance.reserved);
    }
    result = metadata_validate_body(built.data(), built.size());
    if (result == MetadataResult::Success) output->swap(built);
    return result;
}

MetadataResult metadata_validate_body(const uint8_t* body, size_t known_span, MetadataBodyHeader* output) {
    if (!body || known_span < 64) return MetadataResult::OutOfBounds;
    const MetadataBodyHeader header{get32(body),      get32(body + 4),  get32(body + 8),  get32(body + 12),
                                    get64(body + 16), get64(body + 24), get64(body + 32), get64(body + 40),
                                    get32(body + 48), get32(body + 52), get64(body + 56)};
    if (header.magic != kMetadataBodyMagic || header.version != kMetadataVersion) return MetadataResult::Incompatible;
    const uint32_t stride = header.kind == kMetadataBlas ? 64 : header.kind == kMetadataTlas ? 32 : 0;
    if (!stride || header.record_stride != stride || header.records_offset != 64 || header.reserved || header.active_bytes < 64 ||
        (header.active_bytes & 15) || header.active_bytes > header.capacity || header.capacity > known_span)
        return MetadataResult::InvalidInput;
    uint64_t records, end;
    if (!mul(header.record_count, stride, &records) || !add(64, records, &end) || end > header.active_bytes)
        return MetadataResult::OutOfBounds;
    for (uint64_t i = 0; i < header.record_count; ++i) {
        const uint8_t* data = body + size_t(64 + i * stride);
        if (header.kind == kMetadataTlas) {
            const auto cell = get64(data), native = get64(data + 8);
            const auto flags = get32(data + 16), state = get32(data + 20);
            if ((flags & ~63u) || (flags & 12u) == 12u || state > 1 || get64(data + 24) ||
                (state == kMetadataInstanceActive ? (!cell || (cell & 15) || !native) : (cell || native)))
                return MetadataResult::InvalidInput;
            continue;
        }
        const auto geometry = read_geometry(data);
        if ((geometry.geometry_type != kMetadataTriangles && geometry.geometry_type != kMetadataAabbs) ||
            (geometry.geometry_flags & ~3u) || (geometry.association_flags & ~kMetadataAssociation) || geometry.reserved ||
            geometry.reserved64)
            return MetadataResult::InvalidInput;
        if (!geometry.association_flags) {
            if (geometry.association_offset || geometry.association_bytes || geometry.snapshot_offset || geometry.snapshot_capacity)
                return MetadataResult::InvalidInput;
            continue;
        }
        uint64_t association_bytes;
        if (geometry.geometry_type != kMetadataTriangles || !mul(geometry.primitive_count, 8, &association_bytes) ||
            association_bytes != geometry.association_bytes ||
            (association_bytes ? !span(geometry.association_offset, association_bytes, end, header.active_bytes, 8)
                               : geometry.association_offset != 0))
            return MetadataResult::OutOfBounds;
        uint64_t triangle_count = 0;
        if (geometry.snapshot_capacity) {
            if (geometry.snapshot_capacity < 64 || (geometry.snapshot_capacity & 15) ||
                !span(geometry.snapshot_offset, geometry.snapshot_capacity, end, header.active_bytes, 16))
                return MetadataResult::OutOfBounds;
            const uint8_t* snapshot = body + size_t(geometry.snapshot_offset);
            const auto active = get64(snapshot + 16);
            if (active > geometry.snapshot_capacity || storage_validate(snapshot, size_t(active)) != StorageResult::Success)
                return MetadataResult::InvalidInput;
            triangle_count = get64(snapshot + 24);
            for (uint64_t j = active; j < geometry.snapshot_capacity; ++j)
                if (snapshot[size_t(j)]) return MetadataResult::InvalidInput;
        } else if (geometry.snapshot_offset)
            return MetadataResult::InvalidInput;
        for (uint64_t primitive = 0; primitive < geometry.primitive_count; ++primitive) {
            const auto index = get64(body + size_t(geometry.association_offset + primitive * 8));
            if (index < UINT64_MAX - 3 && (!geometry.snapshot_capacity || index >= triangle_count))
                return MetadataResult::OutOfBounds;
        }
    }
    if (header.kind == kMetadataTlas && end != header.active_bytes) return MetadataResult::InvalidInput;
    if (output) *output = header;
    return MetadataResult::Success;
}

MetadataResult metadata_clone_body(const uint8_t* body, size_t known_span, std::vector<uint8_t>* output) {
    if (!output) return MetadataResult::InvalidInput;
    MetadataBodyHeader header;
    auto result = metadata_validate_body(body, known_span, &header);
    if (result != MetadataResult::Success) return result;
    std::vector<uint8_t> copied;
    result = allocate(header.capacity, &copied);
    if (result != MetadataResult::Success) return result;
    std::memcpy(copied.data(), body, size_t(header.active_bytes));
    output->swap(copied);
    return MetadataResult::Success;
}
}  // namespace vel::omm
