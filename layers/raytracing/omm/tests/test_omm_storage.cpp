// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#include "../omm_storage.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace vel::omm;

namespace {
unsigned checks = 0;

#define CHECK(condition)                                                 \
    do {                                                                 \
        ++checks;                                                        \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            return false;                                                \
        }                                                                \
    } while (false)

void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}

void triangle(std::vector<uint8_t>& bytes, size_t offset, uint32_t data, uint32_t level, StorageFormat format) {
    put32(bytes, offset, data);
    bytes[offset + 4] = uint8_t(level);
    bytes[offset + 5] = uint8_t(level >> 8);
    bytes[offset + 6] = uint8_t(format);
    bytes[offset + 7] = 0;
}

bool same_layout(const StorageLayout& a, const StorageLayout& b) {
    return a.triangle_count == b.triangle_count && a.records_offset == b.records_offset && a.data_offset == b.data_offset &&
           a.size == b.size;
}

bool sizes() {
    uint64_t size = 123;
    for (uint32_t level = 0; level <= 31; ++level) {
        const uint64_t states = uint64_t(1) << (2 * level);
        CHECK(storage_data_size(StorageFormat::TwoState, level, &size) == StorageResult::Success);
        CHECK(size == (states + 7) / 8);
        CHECK(storage_data_size(StorageFormat::FourState, level, &size) == StorageResult::Success);
        CHECK(size == (states * 2 + 7) / 8);
    }
    size = 123;
    CHECK(storage_data_size(StorageFormat::TwoState, 32, &size) == StorageResult::Overflow && size == 123);
    CHECK(storage_data_size(StorageFormat(0), 0, &size) == StorageResult::InvalidInput && size == 123);
    CHECK(storage_data_size(StorageFormat::TwoState, 0, nullptr) == StorageResult::InvalidInput);

    StorageLayout empty{};
    CHECK(storage_plan(0, nullptr, nullptr, &empty) == StorageResult::Success);
    CHECK(empty.triangle_count == 0 && empty.records_offset == 64 && empty.data_offset == 64 && empty.size == 64);
    StorageUsage usage[] = {{3, 0, StorageFormat::TwoState}, {2, 2, StorageFormat::FourState}};
    const StorageUsage* pointers[] = {&usage[0], &usage[1]};
    StorageLayout ignored{};
    CHECK(storage_plan(0, usage, pointers, &ignored) == StorageResult::Success && same_layout(ignored, empty));
    StorageLayout direct{}, indirect{};
    CHECK(storage_plan(2, usage, nullptr, &direct) == StorageResult::Success);
    CHECK(storage_plan(2, nullptr, pointers, &indirect) == StorageResult::Success);
    CHECK(same_layout(direct, indirect));
    CHECK(direct.triangle_count == 5 && direct.records_offset == 64 && direct.data_offset == 192 && direct.size == 240);
    auto unchanged = direct;
    CHECK(storage_plan(2, usage, pointers, &direct) == StorageResult::InvalidInput && same_layout(direct, unchanged));
    CHECK(storage_plan(1, nullptr, nullptr, &direct) == StorageResult::InvalidInput && same_layout(direct, unchanged));
    CHECK(storage_plan(1, usage, nullptr, nullptr) == StorageResult::InvalidInput);
    pointers[1] = nullptr;
    CHECK(storage_plan(2, nullptr, pointers, &direct) == StorageResult::InvalidInput && same_layout(direct, unchanged));
    const StorageUsage overflow[] = {{std::numeric_limits<uint64_t>::max(), 0, StorageFormat::TwoState},
                                     {1, 0, StorageFormat::TwoState}};
    CHECK(storage_plan(2, overflow, nullptr, &direct) == StorageResult::Overflow && same_layout(direct, unchanged));
    CHECK(storage_plan(1, overflow, nullptr, &direct) == StorageResult::Overflow && same_layout(direct, unchanged));
    const StorageUsage payload_overflow = {16, 31, StorageFormat::FourState};
    CHECK(storage_plan(1, &payload_overflow, nullptr, &direct) == StorageResult::Overflow && same_layout(direct, unchanged));
    const StorageUsage bad_level = {0, 32, StorageFormat::FourState};
    CHECK(storage_plan(1, &bad_level, nullptr, &direct) == StorageResult::Overflow && same_layout(direct, unchanged));
    return true;
}

bool build_and_roundtrip() {
    // Deliberately unaligned records and a 13-byte stride, with nonmonotonic and
    // shared input offsets. No native struct cast is permitted by the fixture.
    std::vector<uint8_t> records(1 + 13 * 3, 0xcd);
    triangle(records, 1, 3, 0, StorageFormat::TwoState);
    triangle(records, 14, 1, 1, StorageFormat::FourState);
    triangle(records, 27, 8, 2, StorageFormat::FourState);
    std::vector<uint8_t> data = {0xff, 0xe4, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff, 0x1b, 0xe4, 0x77, 0x99};
    StorageBuildInput input{records.data() + 1, records.size() - 1, 3, 13, data.data(), data.size()};
    std::vector<uint8_t> built{0x42};
    CHECK(storage_build(input, &built) == StorageResult::Success);
    StorageLayout layout;
    CHECK(storage_validate(built.data(), built.size(), &layout) == StorageResult::Success);
    CHECK(layout.triangle_count == 3 && layout.records_offset == 64 && layout.data_offset == 144 && layout.size == 176);
    StoredTriangle value{};
    CHECK(storage_triangle(built.data(), built.size(), 0, &value) == StorageResult::Success);
    CHECK(value.format == StorageFormat::TwoState && value.subdivision_level == 0 && value.data_offset == 144 &&
          value.data_size == 1 && built[144] == 0);
    CHECK(storage_triangle(built.data(), built.size(), 1, &value) == StorageResult::Success);
    CHECK(value.format == StorageFormat::FourState && value.subdivision_level == 1 && value.data_offset == 152 &&
          value.data_size == 1 && built[152] == 0xe4);
    CHECK(storage_triangle(built.data(), built.size(), 2, &value) == StorageResult::Success);
    CHECK(value.subdivision_level == 2 && value.data_offset == 160 && value.data_size == 4 &&
          !std::memcmp(built.data() + 160, data.data() + 8, 4));
    const auto original = built;
    records.assign(records.size(), 0);
    data.assign(data.size(), 0);
    records.clear();
    data.clear();
    CHECK(built == original && storage_validate(built.data(), built.size()) == StorageResult::Success);
    CHECK(storage_clone(built.data(), built.size(), &built) == StorageResult::Success && built == original);
    std::vector<uint8_t> serialized, imported;
    CHECK(storage_serialize(built.data(), built.size(), &serialized) == StorageResult::Success);
    CHECK(storage_deserialize(serialized.data(), serialized.size(), &imported) == StorageResult::Success && imported == built);
    CHECK(storage_triangle(built.data(), built.size(), 3, &value) == StorageResult::InvalidInput);
    CHECK(storage_triangle(built.data(), built.size(), 0, nullptr) == StorageResult::InvalidInput);

    // Reject every truncated prefix and appended garbage, without changing output.
    imported = {0x12, 0x34};
    const auto unchanged = imported;
    for (size_t i = 0; i < built.size(); ++i) {
        CHECK(storage_deserialize(built.data(), i, &imported) != StorageResult::Success && imported == unchanged);
    }
    built.push_back(0);
    CHECK(storage_validate(built.data(), built.size()) == StorageResult::InvalidInput);
    built = original;

    struct Corruption {
        size_t offset;
        uint64_t value;
        unsigned width;
        StorageResult result;
    };
    const Corruption corruptions[] = {
        {0, 'X', 1, StorageResult::Incompatible},         {8, 2, 4, StorageResult::Incompatible},
        {12, 63, 4, StorageResult::InvalidInput},         {16, 160, 8, StorageResult::InvalidInput},
        {24, UINT64_MAX, 8, StorageResult::InvalidInput}, {32, 80, 8, StorageResult::InvalidInput},
        {40, 128, 8, StorageResult::InvalidInput},        {48, 1, 8, StorageResult::InvalidInput},
        {56, 1, 8, StorageResult::InvalidInput},          {64, 3, 4, StorageResult::InvalidInput},
        {68, 32, 4, StorageResult::InvalidInput},         {72, 152, 8, StorageResult::InvalidInput},
        {80, 2, 8, StorageResult::InvalidInput},          {136, 1, 1, StorageResult::InvalidInput},
        {144, 2, 1, StorageResult::InvalidInput},         {145, 1, 1, StorageResult::InvalidInput},
        {164, 1, 1, StorageResult::InvalidInput},         {175, 1, 1, StorageResult::InvalidInput},
    };
    for (const auto& corruption : corruptions) {
        auto changed = original;
        for (unsigned i = 0; i < corruption.width; ++i) changed[corruption.offset + i] = uint8_t(corruption.value >> (8 * i));
        CHECK(storage_validate(changed.data(), changed.size()) == corruption.result);
        CHECK(storage_deserialize(changed.data(), changed.size(), &imported) == corruption.result && imported == unchanged);
    }
    CHECK(storage_validate(nullptr, 0) == StorageResult::InvalidInput);
    CHECK(storage_clone(original.data(), original.size(), nullptr) == StorageResult::InvalidInput);
    return true;
}

bool invalid_builds() {
    std::vector<uint8_t> records(8), data(1, 1), output{0xac};
    triangle(records, 0, 0, 0, StorageFormat::TwoState);
    StorageBuildInput input{records.data(), records.size(), 1, 8, data.data(), data.size()};
    const auto unchanged = output;
    auto invalid = input;
    invalid.triangle_stride = 7;
    CHECK(storage_build(invalid, &output) == StorageResult::InvalidInput && output == unchanged);
    invalid = input;
    invalid.triangle_count = UINT64_MAX;
    CHECK(storage_build(invalid, &output) == StorageResult::Overflow && output == unchanged);
    invalid = input;
    invalid.triangles_size = 7;
    CHECK(storage_build(invalid, &output) == StorageResult::InvalidInput && output == unchanged);
    invalid = input;
    invalid.data = nullptr;
    CHECK(storage_build(invalid, &output) == StorageResult::InvalidInput && output == unchanged);
    invalid = input;
    invalid.data_size = 0;
    CHECK(storage_build(invalid, &output) == StorageResult::InvalidInput && output == unchanged);
    put32(records, 0, UINT32_MAX);
    CHECK(storage_build(input, &output) == StorageResult::InvalidInput && output == unchanged);
    put32(records, 0, 0);
    records[6] = 0;
    CHECK(storage_build(input, &output) == StorageResult::InvalidInput && output == unchanged);
    records[6] = 1;
    CHECK(storage_build(input, nullptr) == StorageResult::InvalidInput);

    // A zero-triangle micromap has a valid canonical empty representation.
    CHECK(storage_build({nullptr, 0, 0, 0, nullptr, 0}, &output) == StorageResult::Success);
    CHECK(output.size() == 64 && storage_validate(output.data(), output.size()) == StorageResult::Success);
    return true;
}

bool varied_layouts() {
    uint32_t random = 0x4f4d4d;
    auto next = [&random]() {
        random = random * 1664525u + 1013904223u;
        return random;
    };
    for (uint32_t iteration = 0; iteration < 100; ++iteration) {
        const uint32_t count = next() % 17;
        std::vector<StorageUsage> usages;
        std::vector<uint8_t> records(size_t(count) * 8), data;
        uint64_t slots = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t level = next() % 8;
            const auto format = (next() & 1) ? StorageFormat::TwoState : StorageFormat::FourState;
            // Arithmetic expressed independently of the storage planner.
            const uint64_t bits = (uint64_t(1) << (2 * level)) * uint32_t(format);
            const uint64_t bytes = (bits + 7) / 8;
            triangle(records, size_t(i) * 8, uint32_t(data.size()), level, format);
            for (uint64_t j = 0; j < bytes; ++j) data.push_back(uint8_t(next()));
            usages.push_back({1, level, format});
            slots += ((bytes + 7) / 8) * 8;
        }
        const uint64_t expected_data = ((64 + uint64_t(count) * 24 + 15) / 16) * 16;
        const uint64_t expected_size = ((expected_data + slots + 15) / 16) * 16;
        StorageLayout planned, actual;
        CHECK(storage_plan(count, count ? usages.data() : nullptr, nullptr, &planned) == StorageResult::Success);
        CHECK(planned.data_offset == expected_data && planned.size == expected_size);
        std::vector<uint8_t> built;
        CHECK(storage_build({records.data(), records.size(), count, 8, data.data(), data.size()}, &built) ==
              StorageResult::Success);
        CHECK(storage_validate(built.data(), built.size(), &actual) == StorageResult::Success && same_layout(actual, planned));
    }
    return true;
}
}  // namespace

int main() {
    if (!sizes() || !build_and_roundtrip() || !invalid_builds() || !varied_layouts()) return 1;
    std::printf("OMM storage reference: %u checks passed\n", checks);
    return 0;
}
