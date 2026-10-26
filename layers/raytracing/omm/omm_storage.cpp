// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#include "omm_storage.h"

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace vel::omm {
namespace {

constexpr uint8_t kMagic[8] = {'V', 'E', 'L', 'O', 'M', 'M', 0, 0};

bool add(uint64_t a, uint64_t b, uint64_t* output) {
    if (b > std::numeric_limits<uint64_t>::max() - a) return false;
    *output = a + b;
    return true;
}

bool multiply(uint64_t a, uint64_t b, uint64_t* output) {
    if (a && b > std::numeric_limits<uint64_t>::max() / a) return false;
    *output = a * b;
    return true;
}

bool align(uint64_t value, uint64_t alignment, uint64_t* output) {
    uint64_t sum;
    if (!add(value, alignment - 1, &sum)) return false;
    *output = sum & ~(alignment - 1);
    return true;
}

uint32_t read32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

uint64_t read64(const uint8_t* bytes) { return uint64_t(read32(bytes)) | uint64_t(read32(bytes + 4)) << 32; }

void write32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (i * 8));
}

void write64(uint8_t* bytes, uint64_t value) {
    write32(bytes, uint32_t(value));
    write32(bytes + 4, uint32_t(value >> 32));
}

bool zero_bytes(const uint8_t* bytes, uint64_t begin, uint64_t end) {
    for (uint64_t i = begin; i < end; ++i) {
        if (bytes[size_t(i)]) return false;
    }
    return true;
}

// Only levels 0 and 1 may leave a partially filled last byte. Other supported
// levels contain an integral number of bytes of 1-bit or 2-bit states.
uint8_t tail_mask(StorageFormat format, uint32_t level) {
    if (level > 1) return 0xff;
    const uint32_t bits = (format == StorageFormat::TwoState ? 1u : 2u) << (2 * level);
    return bits >= 8 ? uint8_t(0xff) : uint8_t((1u << bits) - 1);
}

StorageResult layout_start(uint64_t count, StorageLayout* output) {
    StorageLayout result{};
    result.triangle_count = count;
    result.records_offset = kStorageHeaderSize;
    uint64_t records_bytes, records_end;
    if (!multiply(count, kStorageRecordSize, &records_bytes) || !add(kStorageHeaderSize, records_bytes, &records_end) ||
        !align(records_end, 16, &result.data_offset)) {
        return StorageResult::Overflow;
    }
    result.size = result.data_offset;
    *output = result;
    return StorageResult::Success;
}

StorageResult append_usage(uint64_t count, StorageFormat format, uint32_t level, uint64_t* size) {
    uint64_t data_size, slot_size, total;
    const auto result = storage_data_size(format, level, &data_size);
    if (result != StorageResult::Success) return result;
    if (!align(data_size, 8, &slot_size) || !multiply(count, slot_size, &total) || !add(*size, total, size)) {
        return StorageResult::Overflow;
    }
    return StorageResult::Success;
}

void input_triangle(const uint8_t* bytes, uint32_t* offset, uint32_t* level, StorageFormat* format) {
    *offset = read32(bytes);
    *level = uint32_t(bytes[4]) | uint32_t(bytes[5]) << 8;
    *format = StorageFormat(uint32_t(bytes[6]) | uint32_t(bytes[7]) << 8);
}

StorageResult allocate_blob(uint64_t size, std::vector<uint8_t>* output) {
    if (size > std::numeric_limits<size_t>::max() || size > output->max_size()) return StorageResult::Overflow;
    try {
        output->resize(size_t(size), 0);
    } catch (const std::bad_alloc&) {
        return StorageResult::OutOfMemory;
    } catch (const std::length_error&) {
        return StorageResult::Overflow;
    }
    return StorageResult::Success;
}

}  // namespace

StorageResult storage_data_size(StorageFormat format, uint32_t subdivision_level, uint64_t* size) {
    if (!size || (format != StorageFormat::TwoState && format != StorageFormat::FourState)) return StorageResult::InvalidInput;
    if (subdivision_level > 31) return StorageResult::Overflow;
    const uint64_t states = uint64_t(1) << (2 * subdivision_level);
    const uint64_t bits = states * (format == StorageFormat::TwoState ? 1u : 2u);
    *size = bits / 8 + (bits % 8 != 0);
    return StorageResult::Success;
}

StorageResult storage_plan(uint32_t usage_count, const StorageUsage* usages, const StorageUsage* const* usage_pointers,
                           StorageLayout* layout) {
    if (!layout || (usage_count && bool(usages) == bool(usage_pointers))) return StorageResult::InvalidInput;
    uint64_t triangle_count = 0;
    for (uint32_t i = 0; i < usage_count; ++i) {
        const StorageUsage* usage = usages ? &usages[i] : usage_pointers[i];
        if (!usage) return StorageResult::InvalidInput;
        uint64_t ignored;
        const auto result = storage_data_size(usage->format, usage->subdivision_level, &ignored);
        if (result != StorageResult::Success) return result;
        if (!add(triangle_count, usage->count, &triangle_count)) return StorageResult::Overflow;
    }
    StorageLayout result;
    auto status = layout_start(triangle_count, &result);
    if (status != StorageResult::Success) return status;
    for (uint32_t i = 0; i < usage_count; ++i) {
        const StorageUsage& usage = usages ? usages[i] : *usage_pointers[i];
        status = append_usage(usage.count, usage.format, usage.subdivision_level, &result.size);
        if (status != StorageResult::Success) return status;
    }
    if (!align(result.size, 16, &result.size)) return StorageResult::Overflow;
    *layout = result;
    return StorageResult::Success;
}

StorageResult storage_build(const StorageBuildInput& input, std::vector<uint8_t>* output) {
    if (!output || (input.triangles_size && !input.triangles) || (input.data_size && !input.data)) {
        return StorageResult::InvalidInput;
    }
    if (input.triangle_count) {
        uint64_t last_offset, required;
        if (!input.triangles || input.triangle_stride < 8) return StorageResult::InvalidInput;
        if (!multiply(input.triangle_count - 1, input.triangle_stride, &last_offset) || !add(last_offset, 8, &required)) {
            return StorageResult::Overflow;
        }
        if (required > input.triangles_size) return StorageResult::InvalidInput;
    }
    StorageLayout layout;
    auto result = layout_start(input.triangle_count, &layout);
    if (result != StorageResult::Success) return result;
    for (uint64_t i = 0; i < input.triangle_count; ++i) {
        uint32_t offset, level;
        StorageFormat format;
        input_triangle(input.triangles + size_t(i * input.triangle_stride), &offset, &level, &format);
        uint64_t bytes;
        result = storage_data_size(format, level, &bytes);
        if (result != StorageResult::Success) return result;
        if (offset > input.data_size || bytes > input.data_size - offset) return StorageResult::InvalidInput;
        result = append_usage(1, format, level, &layout.size);
        if (result != StorageResult::Success) return result;
    }
    if (!align(layout.size, 16, &layout.size)) return StorageResult::Overflow;
    std::vector<uint8_t> built;
    result = allocate_blob(layout.size, &built);
    if (result != StorageResult::Success) return result;
    std::memcpy(built.data(), kMagic, sizeof(kMagic));
    write32(built.data() + 8, kStorageVersion);
    write32(built.data() + 12, uint32_t(kStorageHeaderSize));
    write64(built.data() + 16, layout.size);
    write64(built.data() + 24, input.triangle_count);
    write64(built.data() + 32, layout.records_offset);
    write64(built.data() + 40, layout.data_offset);
    uint64_t destination = layout.data_offset;
    for (uint64_t i = 0; i < input.triangle_count; ++i) {
        uint32_t offset, level;
        StorageFormat format;
        input_triangle(input.triangles + size_t(i * input.triangle_stride), &offset, &level, &format);
        uint64_t bytes = 0, slot_size = 0;
        storage_data_size(format, level, &bytes);
        align(bytes, 8, &slot_size);
        uint8_t* record = built.data() + size_t(kStorageHeaderSize + i * kStorageRecordSize);
        write32(record, uint32_t(format));
        write32(record + 4, level);
        write64(record + 8, destination);
        write64(record + 16, bytes);
        std::memcpy(built.data() + size_t(destination), input.data + offset, size_t(bytes));
        built[size_t(destination + bytes - 1)] &= tail_mask(format, level);
        destination += slot_size;
    }
    output->swap(built);
    return StorageResult::Success;
}

StorageResult storage_validate(const uint8_t* bytes, size_t size, StorageLayout* layout) {
    if (!bytes || size < kStorageHeaderSize) return StorageResult::InvalidInput;
    if (std::memcmp(bytes, kMagic, sizeof(kMagic)) || read32(bytes + 8) != kStorageVersion) return StorageResult::Incompatible;
    if (read32(bytes + 12) != kStorageHeaderSize || read64(bytes + 16) != size || read64(bytes + 48) || read64(bytes + 56)) {
        return StorageResult::InvalidInput;
    }
    StorageLayout expected;
    auto result = layout_start(read64(bytes + 24), &expected);
    if (result != StorageResult::Success) return StorageResult::InvalidInput;
    if (read64(bytes + 32) != expected.records_offset || read64(bytes + 40) != expected.data_offset ||
        expected.data_offset > size ||
        !zero_bytes(bytes, kStorageHeaderSize + expected.triangle_count * kStorageRecordSize, expected.data_offset)) {
        return StorageResult::InvalidInput;
    }
    for (uint64_t i = 0; i < expected.triangle_count; ++i) {
        const uint8_t* record = bytes + size_t(expected.records_offset + i * kStorageRecordSize);
        const auto format = StorageFormat(read32(record));
        const uint32_t level = read32(record + 4);
        uint64_t data_size = 0, slot_size = 0;
        result = storage_data_size(format, level, &data_size);
        if (result != StorageResult::Success || read64(record + 8) != expected.size || read64(record + 16) != data_size ||
            !align(data_size, 8, &slot_size) || slot_size > size - expected.size) {
            return StorageResult::InvalidInput;
        }
        if ((bytes[size_t(expected.size + data_size - 1)] & uint8_t(~tail_mask(format, level))) ||
            !zero_bytes(bytes, expected.size + data_size, expected.size + slot_size)) {
            return StorageResult::InvalidInput;
        }
        expected.size += slot_size;
    }
    const uint64_t data_end = expected.size;
    if (!align(expected.size, 16, &expected.size) || expected.size != size || !zero_bytes(bytes, data_end, size)) {
        return StorageResult::InvalidInput;
    }
    if (layout) *layout = expected;
    return StorageResult::Success;
}

StorageResult storage_triangle(const uint8_t* bytes, size_t size, uint64_t triangle_index, StoredTriangle* triangle) {
    if (!triangle) return StorageResult::InvalidInput;
    StorageLayout layout;
    const auto result = storage_validate(bytes, size, &layout);
    if (result != StorageResult::Success) return result;
    if (triangle_index >= layout.triangle_count) return StorageResult::InvalidInput;
    const uint8_t* record = bytes + size_t(layout.records_offset + triangle_index * kStorageRecordSize);
    *triangle = {StorageFormat(read32(record)), read32(record + 4), read64(record + 8), read64(record + 16)};
    return StorageResult::Success;
}

StorageResult storage_clone(const uint8_t* bytes, size_t size, std::vector<uint8_t>* output) {
    if (!output) return StorageResult::InvalidInput;
    const auto result = storage_validate(bytes, size);
    if (result != StorageResult::Success) return result;
    std::vector<uint8_t> cloned;
    const auto allocation = allocate_blob(size, &cloned);
    if (allocation != StorageResult::Success) return allocation;
    std::memcpy(cloned.data(), bytes, size);
    output->swap(cloned);
    return StorageResult::Success;
}

StorageResult storage_serialize(const uint8_t* bytes, size_t size, std::vector<uint8_t>* output) {
    return storage_clone(bytes, size, output);
}

StorageResult storage_deserialize(const uint8_t* bytes, size_t size, std::vector<uint8_t>* output) {
    return storage_clone(bytes, size, output);
}

}  // namespace vel::omm
