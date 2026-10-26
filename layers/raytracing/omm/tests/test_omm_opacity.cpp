// Copyright 2022-2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#include "omm_opacity.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace vel::omm;

namespace {

uint64_t checks = 0;

void check(bool expression, int line) {
    ++checks;
    if (!expression) {
        std::fprintf(stderr, "OMM opacity check failed at line %d\n", line);
        std::exit(1);
    }
}

#define CHECK(expression) check((expression), __LINE__)

void test_encodings() {
    uint64_t size = 99;
    CHECK(encoded_data_size(Format::TwoState, 0, &size) == Status::Success && size == 1);
    CHECK(encoded_data_size(Format::FourState, 0, &size) == Status::Success && size == 1);
    CHECK(encoded_data_size(Format::TwoState, 31, &size) == Status::Success && size == (uint64_t{1} << 59));
    CHECK(encoded_data_size(Format::FourState, 31, &size) == Status::Success && size == (uint64_t{1} << 60));
    size = 99;
    CHECK(encoded_data_size(static_cast<Format>(3), 0, &size) == Status::InvalidArgument && size == 99);
    CHECK(encoded_data_size(Format::TwoState, 32, &size) == Status::InvalidArgument && size == 99);
    CHECK(encoded_data_size(Format::TwoState, 0, nullptr) == Status::InvalidArgument);
    for (const auto format : {Format::TwoState, Format::FourState}) {
        const uint32_t per_byte = format == Format::TwoState ? 8 : 4;
        const uint32_t mask = format == Format::TwoState ? 1 : 3;
        const uint32_t bits = format == Format::TwoState ? 1 : 2;
        // Every byte pattern checks low-bit-first unpacking across every lane.
        for (uint32_t byte = 0; byte < 256; ++byte) {
            const uint8_t data[2] = {static_cast<uint8_t>(byte), static_cast<uint8_t>(~byte)};
            for (uint32_t index = 0; index < 16; ++index) {
                State state = State::Transparent;
                // Level 2 has 16 microtriangles. Use an independent per-bit
                // construction for the complete source representation.
                std::array<uint8_t, 4> full = {data[0], data[1], data[1], data[0]};
                CHECK(decode_state(format, 2, full.data(), full.size(), index, &state) == Status::Success);
                const uint32_t lane = index % per_byte;
                uint32_t expected = 0;
                for (uint32_t bit = 0; bit < bits; ++bit)
                    expected += ((full[index / per_byte] / (1u << (bits * lane + bit))) % 2) << bit;
                CHECK(static_cast<uint32_t>(state) == (expected & mask));
            }
        }
        State state = State::UnknownOpaque;
        const uint8_t data[4] = {};
        CHECK(decode_state(format, 2, data, 4, 16, &state) == Status::OutOfBounds && state == State::UnknownOpaque);
        CHECK(decode_state(format, 2, data, 0, 0, &state) == Status::OutOfBounds && state == State::UnknownOpaque);
        CHECK(decode_state(format, 0, nullptr, 1, 0, &state) == Status::InvalidArgument && state == State::UnknownOpaque);
        CHECK(decode_state(format, 0, data, 4, 0, nullptr) == Status::InvalidArgument);
    }
}

void test_association() {
    for (const uint32_t width : {8u, 16u, 32u}) {
        const uint32_t maximum = width == 32 ? UINT32_MAX : (1u << width) - 1;
        for (uint32_t special = 0; special < 4; ++special) {
            TriangleIndex index;
            CHECK(decode_index(maximum - special, width, UINT32_MAX, &index) == Status::Success);
            CHECK(index.special && static_cast<uint32_t>(index.state) == special);
        }
        TriangleIndex index;
        CHECK(decode_index(maximum - 4, width, UINT32_MAX, &index) == Status::Success);
        CHECK(!index.special && index.triangle == uint64_t{maximum - 4} + UINT32_MAX);
        if (width != 32) CHECK(decode_index(maximum + 1, width, 0, &index) == Status::InvalidArgument);
        // High-bit ordinary values must not be mistaken for signed negatives.
        CHECK(decode_index(1u << (width - 1), width, 5, &index) == Status::Success);
        CHECK(!index.special && index.triangle == uint64_t{1u << (width - 1)} + 5);
    }
    TriangleIndex output;
    output.triangle = 123;
    CHECK(decode_index(0, 64, 0, &output) == Status::InvalidArgument && output.triangle == 123);
    CHECK(decode_index(0, 32, 0, nullptr) == Status::InvalidArgument);

    // Padding is intentionally nonzero; stride is bytes rather than elements.
    const uint8_t indices[] = {0xff, 0xff, 0x43, 0x51, 0x02, 0x00, 0x81, 0x93, 0xfc, 0xff, 0x00, 0x00};
    Association association{IndexType::Uint16, indices, sizeof(indices), 4, 5, 8, true};
    CHECK(resolve_association(association, 0, &output) == Status::Success && output.special && output.state == State::Transparent);
    CHECK(resolve_association(association, 1, &output) == Status::Success && !output.special && output.triangle == 7);
    CHECK(resolve_association(association, 2, &output) == Status::Success && output.special &&
          output.state == State::UnknownOpaque);
    CHECK(resolve_association(association, 3, &output) == Status::OutOfBounds);
    association.has_micromap = false;
    CHECK(resolve_association(association, 0, &output) == Status::Success && output.special);
    CHECK(resolve_association(association, 1, &output) == Status::OutOfBounds);
    association.index_stride = 0;
    CHECK(resolve_association(association, UINT64_MAX, &output) == Status::Success && output.special);
    association.index_stride = 6;
    CHECK(resolve_association(association, UINT64_MAX, &output) == Status::Overflow);
    association.index_stride = 3;
    CHECK(resolve_association(association, 0, &output) == Status::InvalidArgument);
    association.index_stride = uint64_t{1} << 32;
    CHECK(resolve_association(association, 0, &output) == Status::InvalidArgument);
    association.index_stride = 2;
    association.indices = nullptr;
    CHECK(resolve_association(association, 0, &output) == Status::InvalidArgument);

    const uint8_t indices32[] = {0, 0, 0, 0, 0xfe, 0xff, 0xff, 0xff, 0xfa, 0xff, 0xff, 0xff};
    association = {IndexType::Uint32, indices32, sizeof(indices32), 4, 10, 11, true};
    CHECK(resolve_association(association, 0, &output) == Status::Success && output.triangle == 10);
    CHECK(resolve_association(association, 1, &output) == Status::Success && output.special && output.state == State::Opaque);
    CHECK(resolve_association(association, 2, &output) == Status::OutOfBounds);  // Does not wrap base addition.
    association = {IndexType::Implicit, nullptr, 0, 0, 10, 12, true};
    CHECK(resolve_association(association, 0, &output) == Status::Success && output.triangle == 10);
    CHECK(resolve_association(association, 1, &output) == Status::Success && output.triangle == 11);
    CHECK(resolve_association(association, 2, &output) == Status::OutOfBounds);
    CHECK(resolve_association(association, UINT32_MAX, &output) == Status::Success && output.special &&
          output.state == State::Transparent);
    association.has_micromap = false;
    CHECK(resolve_association(association, 0, &output) == Status::OutOfBounds);
    CHECK(resolve_association(association, uint64_t{1} << 32, &output) == Status::OutOfBounds);
    CHECK(resolve_association(association, 0, nullptr) == Status::InvalidArgument);
    association.index_type = static_cast<IndexType>(99);
    CHECK(resolve_association(association, 0, &output) == Status::InvalidArgument);
}

void test_opacity() {
    constexpr uint32_t ray_modes[] = {0, RayOpaque, RayNoOpaque, RayCullOpaque, RayCullNoOpaque};
    constexpr uint32_t instance_modes[] = {0, InstanceForceOpaque, InstanceForceNoOpaque};
    constexpr Decision omm_table[2][4] = {{Decision::Ignore, Decision::Opaque, Decision::NonOpaque, Decision::NonOpaque},
                                          {Decision::Ignore, Decision::Opaque, Decision::Ignore, Decision::Opaque}};
    // Columns: no override / force opaque / force nonopaque; rows: nonopaque,
    // opaque. This is a table of the public Vulkan precedence, independent of the
    // evaluator's flag-branch implementation.
    constexpr Decision instance_table[2][3] = {{Decision::NonOpaque, Decision::Opaque, Decision::NonOpaque},
                                               {Decision::Opaque, Decision::Opaque, Decision::NonOpaque}};
    constexpr Decision ray_table[2][5] = {
        {Decision::NonOpaque, Decision::Opaque, Decision::NonOpaque, Decision::NonOpaque, Decision::Ignore},
        {Decision::Opaque, Decision::Opaque, Decision::NonOpaque, Decision::Ignore, Decision::Opaque}};
    for (uint32_t state = 0; state < 4; ++state)
        for (uint32_t attached = 0; attached < 2; ++attached)
            for (uint32_t disabled = 0; disabled < 2; ++disabled)
                for (uint32_t geometry = 0; geometry < 2; ++geometry)
                    for (uint32_t ray_two = 0; ray_two < 2; ++ray_two)
                        for (uint32_t instance_two = 0; instance_two < 2; ++instance_two)
                            for (uint32_t instance = 0; instance < 3; ++instance)
                                for (uint32_t ray = 0; ray < 5; ++ray) {
                                    const OpacityInput input{attached != 0, static_cast<State>(state),
                                                             ray_modes[ray] | (ray_two ? RayForceTwoState : 0),
                                                             instance_modes[instance] | (instance_two ? InstanceForceTwoState : 0) |
                                                                 (disabled ? InstanceDisableMicromaps : 0),
                                                             geometry ? GeometryOpaque : 0};
                                    Decision base = geometry ? Decision::Opaque : Decision::NonOpaque;
                                    if (attached && !disabled) base = omm_table[ray_two || instance_two][state];
                                    Decision expected = Decision::Ignore;
                                    if (base != Decision::Ignore) {
                                        const Decision after_instance = instance_table[base == Decision::Opaque][instance];
                                        expected = ray_table[after_instance == Decision::Opaque][ray];
                                    }
                                    Decision actual;
                                    CHECK(resolve_opacity(input, &actual) == Status::Success && actual == expected);
                                }
    // Invalid flag combinations are rejected without publishing a decision.
    for (uint32_t bits = 0; bits < 16; ++bits) {
        uint32_t mask = 0;
        constexpr uint32_t flags[] = {RayOpaque, RayNoOpaque, RayCullOpaque, RayCullNoOpaque};
        uint32_t count = 0;
        for (uint32_t i = 0; i < 4; ++i)
            if (bits & (1u << i)) {
                mask |= flags[i];
                ++count;
            }
        Decision actual = Decision::Opaque;
        const OpacityInput input{false, State::Transparent, mask, 0, 0};
        CHECK(resolve_opacity(input, &actual) == (count > 1 ? Status::InvalidArgument : Status::Success));
        if (count > 1) CHECK(actual == Decision::Opaque);
    }
    Decision actual = Decision::Opaque;
    CHECK(resolve_opacity({true, static_cast<State>(4), 0, 0, 0}, &actual) == Status::InvalidArgument);
    CHECK(actual == Decision::Opaque);
    CHECK(resolve_opacity({false, State::Transparent, 0, InstanceForceOpaque | InstanceForceNoOpaque, 0}, &actual) ==
          Status::InvalidArgument);
    CHECK(resolve_opacity({}, nullptr) == Status::InvalidArgument);
    CHECK(resolve_opacity({false, static_cast<State>(UINT32_MAX), 0x80000000u, 0x80000000u, 0}, &actual) == Status::Success &&
          actual == Decision::NonOpaque);
    // A special-only association is active even without a backing object; the
    // adapter supplies has_micromap=true after resolve_association returns it.
    const uint8_t special_only[] = {0xff, 0xff};
    TriangleIndex special;
    CHECK(resolve_association({IndexType::Uint16, special_only, sizeof(special_only), 2, 123, 0, false}, 0, &special) ==
          Status::Success);
    CHECK(special.special);
    CHECK(resolve_opacity({true, special.state, RayOpaque, InstanceForceOpaque, GeometryOpaque}, &actual) == Status::Success &&
          actual == Decision::Ignore);
}

// Independent inverse oracle adapted from the Apache-2.0 Khronos CTS
// calcSubtriangleCentroid (2022 Khronos), pinned source revision
// d18e4c68543103371715845cd914c71e5859963e, ray_tracing/
// vktRayTracingOpacityMicromapTests.cpp. This maps index -> centroid; production
// instead quantizes barycentrics and encodes the curve address.
std::array<float, 2> cts_centroid(uint32_t index, uint32_t level) {
    if (!level) return {1.0f / 3.0f, 1.0f / 3.0f};
    uint32_t d = index;
    d = ((d >> 1) & 0x22222222u) | ((d << 1) & 0x44444444u) | (d & 0x99999999u);
    d = ((d >> 2) & 0x0c0c0c0cu) | ((d << 2) & 0x30303030u) | (d & 0xc3c3c3c3u);
    d = ((d >> 4) & 0x00f000f0u) | ((d << 4) & 0x0f000f00u) | (d & 0xf00ff00fu);
    d = ((d >> 8) & 0x0000ff00u) | ((d << 8) & 0x00ff0000u) | (d & 0xff0000ffu);
    uint32_t f = (d & 0xffffu) | ((d << 16) & ~d);
    f ^= (f >> 1) & 0x7fff7fffu;
    f ^= (f >> 2) & 0x3fff3fffu;
    f ^= (f >> 4) & 0x0fff0fffu;
    f ^= (f >> 8) & 0x00ff00ffu;
    const uint32_t t = (f ^ d) >> 16;
    const uint32_t mask = (1u << level) - 1;
    const uint32_t iu = ((f & ~t) | (d & ~t) | (~d & ~f & t)) & mask;
    const uint32_t iv = ((f >> 16) ^ d) & mask;
    const uint32_t iw = ((~f & ~t) | (d & ~t) | (~d & f & t)) & mask;
    const bool upright = ((iu ^ iv ^ iw) & 1) != 0;
    const float scale = 1.0f / static_cast<float>(1u << level);
    const float fraction = upright ? 1.0f / 3.0f : 2.0f / 3.0f;
    return {(static_cast<float>(iu) + fraction) * scale, (static_cast<float>(iv) + fraction) * scale};
}

void test_bird_curve() {
    // All interiors through level 9 (349525 distinct microtriangles).
    for (uint32_t level = 0; level <= 9; ++level)
        for (uint32_t i = 0; i < (1u << (2 * level)); ++i) {
            const auto centroid = cts_centroid(i, level);
            uint32_t index = UINT32_MAX;
            CHECK(barycentric_index(centroid[0], centroid[1], level, &index) == Status::Success && index == i);
        }
    // Deterministic dispersed interiors across every remaining representable
    // level. Full level16 enumeration would require 2^32 samples.
    uint32_t sample = 0x12345678u;
    for (uint32_t level = 10; level <= 16; ++level) {
        const uint32_t mask = level == 16 ? UINT32_MAX : (1u << (2 * level)) - 1;
        for (uint32_t i = 0; i < 4096; ++i) {
            sample = sample * 1664525u + 1013904223u;
            const uint32_t expected = sample & mask;
            const auto centroid = cts_centroid(expected, level);
            uint32_t index;
            CHECK(barycentric_index(centroid[0], centroid[1], level, &index) == Status::Success && index == expected);
        }
    }
    // Fixed level1 and2 vertices/edges encode the exact quantization tie policy.
    struct Boundary {
        float u, v;
        uint32_t level, expected;
    };
    constexpr Boundary boundaries[] = {{0, 0, 1, 0},    {1, 0, 1, 2},         {0, 1, 1, 3},       {0.5f, 0, 1, 2},
                                       {0, 0.5f, 1, 3}, {0.25f, 0.25f, 1, 1}, {0.5f, 0.5f, 1, 3}, {1, 1, 1, 3}};
    for (const auto& boundary : boundaries) {
        uint32_t index;
        CHECK(barycentric_index(boundary.u, boundary.v, boundary.level, &index) == Status::Success && index == boundary.expected);
    }
    for (uint32_t level = 0; level <= 16; ++level) {
        uint32_t index, clamped;
        CHECK(barycentric_index(-100, -1, level, &index) == Status::Success);
        CHECK(barycentric_index(0, 0, level, &clamped) == Status::Success && index == clamped);
        CHECK(barycentric_index(2, 1.5f, level, &index) == Status::Success);
        CHECK(barycentric_index(1, 1, level, &clamped) == Status::Success && index == clamped);
    }
    uint32_t index = 123;
    CHECK(barycentric_index(0, 0, 17, &index) == Status::InvalidArgument && index == 123);
    CHECK(barycentric_index(std::numeric_limits<float>::quiet_NaN(), 0, 1, &index) == Status::InvalidArgument && index == 123);
    CHECK(barycentric_index(0, std::numeric_limits<float>::infinity(), 1, &index) == Status::InvalidArgument && index == 123);
    CHECK(barycentric_index(0, 0, 1, nullptr) == Status::InvalidArgument);
}

}  // namespace

int main() {
    test_encodings();
    test_association();
    test_opacity();
    test_bird_curve();
    std::printf("OMM opacity core: %llu checks passed\n", static_cast<unsigned long long>(checks));
    return 0;
}
