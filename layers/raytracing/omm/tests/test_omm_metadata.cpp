// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "../omm_metadata.h"
#include "../omm_storage.h"
#include <cstdio>
#include <cstring>

using namespace vel::omm;
namespace {
unsigned checks = 0;
#define CHECK(value)                                                 \
    do {                                                             \
        ++checks;                                                    \
        if (!(value)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); \
            return false;                                            \
        }                                                            \
    } while (false)
void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}
void put64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}
uint64_t get64(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}
std::vector<uint8_t> canonical() {
    std::vector<uint8_t> triangles(8 * 8), data(8, 0xe4), result;
    for (uint32_t i = 0; i < 8; ++i) {
        put32(triangles, i * 8, i);
        triangles[i * 8 + 4] = 1;
        triangles[i * 8 + 6] = 2;
    }
    if (storage_build({triangles.data(), triangles.size(), 8, 8, data.data(), data.size()}, &result) != StorageResult::Success)
        return {};
    result.resize(result.size() + 17, 0xa5);  // Source capacity deliberately exceeds active size.
    return result;
}
bool planner() {
    uint64_t bytes = 999;
    CHECK(metadata_plan_blas(nullptr, 0, nullptr, 0, &bytes) == MetadataResult::Success && bytes == 64);
    MetadataGeometryPlan plans[] = {
        {kMetadataTriangles, 1, 3, false, 0}, {kMetadataTriangles, 2, 4, true, 113}, {kMetadataAabbs, 1, 2, false, 0}};
    MetadataGeometry geometries[3]{};
    CHECK(metadata_plan_blas(plans, 3, geometries, 3, &bytes) == MetadataResult::Success);
    CHECK(bytes == 416 && geometries[1].association_offset == 256 && geometries[1].association_bytes == 32 &&
          geometries[1].snapshot_offset == 288 && geometries[1].snapshot_capacity == 128);
    const auto saved = geometries[0];
    const auto saved_bytes = bytes;
    plans[2].association = true;
    CHECK(metadata_plan_blas(plans, 3, geometries, 3, &bytes) == MetadataResult::InvalidInput && bytes == saved_bytes &&
          !std::memcmp(&saved, geometries, sizeof(saved)));
    plans[2].association = false;
    plans[1].primitive_count = UINT64_MAX;
    CHECK(metadata_plan_blas(plans, 3, geometries, 3, &bytes) == MetadataResult::Overflow && bytes == saved_bytes);
    CHECK(metadata_plan_blas(plans, 3, geometries, 2, &bytes) == MetadataResult::OutOfBounds);
    return true;
}
bool pack_and_validate() {
    auto snapshot = canonical();
    CHECK(snapshot.size() > 64);
    uint8_t indices[] = {0, 0, 0xcd, 0xcd, 1, 0, 0xcd, 0xcd, 0xfe, 0xff, 0xcd, 0xcd};
    const uint8_t specials[] = {0xff, 0xff, 0xfe, 0xff, 0xfd, 0xff, 0xfc, 0xff};
    MetadataGeometryInput inputs[] = {
        {{kMetadataTriangles, 1, 3, false, 0}, nullptr, 0, 0, 0, 0, nullptr, 0},
        {{kMetadataTriangles, 2, 3, true, snapshot.size()}, indices, sizeof(indices), 4, 16, 2, snapshot.data(), snapshot.size()},
        {{kMetadataTriangles, 1, 4, true, 0}, specials, sizeof(specials), 2, 16, UINT32_MAX, nullptr, 0},
        {{kMetadataAabbs, 1, 2, false, 0}, nullptr, 0, 0, 0, 0, nullptr, 0},
        {{kMetadataTriangles, 0, 2, true, snapshot.size()}, indices, sizeof(indices), 0, 16, 3, snapshot.data(), snapshot.size()},
        {{kMetadataTriangles, 0, 2, true, snapshot.size()}, nullptr, 0, 0, 0, 4, snapshot.data(), snapshot.size()},
    };
    std::vector<uint8_t> body;
    CHECK(metadata_pack_blas(inputs, 6, 0x1234, 99, &body) == MetadataResult::Success);
    MetadataBodyHeader header;
    CHECK(metadata_validate_body(body.data(), body.size(), &header) == MetadataResult::Success && header.record_count == 6 &&
          header.kind == kMetadataBlas && header.build_flags == 0x1234 && header.generation == 99);
    auto association = get64(body, 64 + 64 + 16);
    CHECK(get64(body, size_t(association)) == 2 && get64(body, size_t(association + 8)) == 3 &&
          get64(body, size_t(association + 16)) == metadata_special(1));
    auto special = get64(body, 64 + 128 + 16);
    for (uint32_t state = 0; state < 4; ++state) CHECK(get64(body, size_t(special + state * 8)) == metadata_special(state));
    auto broadcast = get64(body, 64 + 4 * 64 + 16);
    CHECK(get64(body, size_t(broadcast)) == 3 && get64(body, size_t(broadcast + 8)) == 3);
    auto implicit = get64(body, 64 + 5 * 64 + 16);
    CHECK(get64(body, size_t(implicit)) == 4 && get64(body, size_t(implicit + 8)) == 5);
    const auto original = body;
    snapshot.assign(snapshot.size(), 0);
    std::memset(indices, 0, sizeof(indices));
    CHECK(metadata_validate_body(body.data(), body.size()) == MetadataResult::Success && body == original);
    CHECK(metadata_clone_body(body.data(), body.size(), &body) == MetadataResult::Success && body == original);
    for (size_t prefix = 0; prefix < body.size(); ++prefix)
        CHECK(metadata_validate_body(body.data(), prefix) != MetadataResult::Success);
    for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(16), size_t(24), size_t(32), size_t(40), size_t(52),
                          size_t(64), size_t(64 + 4), size_t(64 + 48), size_t(64 + 52), size_t(64 + 56)}) {
        auto changed = original;
        changed[offset] ^= 0x80;
        CHECK(metadata_validate_body(changed.data(), changed.size()) != MetadataResult::Success);
    }
    auto changed = original;
    put64(changed, 64 + 64 + 16, UINT64_MAX - 7);
    CHECK(metadata_validate_body(changed.data(), changed.size()) == MetadataResult::OutOfBounds);
    std::vector<uint8_t> output{1, 2, 3};
    const auto unchanged = output;
    auto source = canonical();
    const uint8_t ordinary[] = {1, 0, 0, 0};
    MetadataGeometryInput carry{{kMetadataTriangles, 1, 1, true, source.size()},
                                ordinary,
                                sizeof(ordinary),
                                4,
                                32,
                                UINT32_MAX,
                                source.data(),
                                source.size()};
    CHECK(metadata_pack_blas(&carry, 1, 0, 0, &output) == MetadataResult::OutOfBounds && output == unchanged);
    carry.plan.snapshot_capacity = 0;
    carry.snapshot = nullptr;
    carry.snapshot_size = 0;
    CHECK(metadata_pack_blas(&carry, 1, 0, 0, &output) == MetadataResult::OutOfBounds && output == unchanged);
    CHECK(metadata_clone_body(changed.data(), changed.size(), &output) != MetadataResult::Success && output == unchanged);
    return true;
}
bool tlas_and_empty() {
    // native_blas_address is an opaque identity, including legal tagged values.
    const MetadataInstance instances[] = {{0x1000, 0x2005, 0x10, 1, 0}, {0, 0, 0, 0, 0}, {0x3000, 0x2005, 0x20, 1, 0}};
    std::vector<uint8_t> body;
    CHECK(metadata_pack_tlas(instances, 3, 7, 8, &body) == MetadataResult::Success && body.size() == 160);
    CHECK(metadata_validate_body(body.data(), body.size()) == MetadataResult::Success);
    auto changed = body;
    put32(changed, 64 + 20, 2);
    CHECK(metadata_validate_body(changed.data(), changed.size()) == MetadataResult::InvalidInput);
    changed = body;
    put64(changed, 64, 1);
    CHECK(metadata_validate_body(changed.data(), changed.size()) == MetadataResult::InvalidInput);
    CHECK(metadata_pack_tlas(nullptr, 0, 0, 0, &body) == MetadataResult::Success && body.size() == 64);
    CHECK(metadata_pack_blas(nullptr, 0, 0, 0, &body) == MetadataResult::Success && body.size() == 64);
    CHECK(metadata_validate_body(body.data(), body.size()) == MetadataResult::Success);
    return true;
}
}  // namespace
int main() {
    if (!planner() || !pack_and_validate() || !tlas_and_empty()) return 1;
    std::printf("OMM metadata reference: %u checks PASS\n", checks);
    return 0;
}
