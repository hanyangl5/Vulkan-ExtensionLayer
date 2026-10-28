/* SPDX-License-Identifier: MIT */
#include "vkrt_build_dispatch.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                     \
        }                                                   \
    } while (0)

static void check_small(uint32_t groups, uint32_t x, uint32_t y, uint32_t z) {
    struct vkrt_build_dispatch_grid grid = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const struct vkrt_build_dispatch_grid untouched = grid;
    bool ok = vkrt_build_dispatch_grid(groups, x, y, z, &grid);
    bool fits = groups && x && y && z && (uint64_t)x * y * z >= groups;
    CHECK(ok == fits);
    if (!ok) {
        CHECK(!memcmp(&grid, &untouched, sizeof(grid)));
        return;
    }
    CHECK(grid.x && grid.x <= x && grid.y && grid.y <= y && grid.z && grid.z <= z);
    unsigned char seen[1024] = {0};
    uint64_t invocations = 0, padded = 0;
    /* Independent enumeration proves every requested group/invocation appears
     * once, including a non-full final row and a non-full final plane. */
    for (uint32_t k = 0; k < grid.z; ++k)
        for (uint32_t j = 0; j < grid.y; ++j)
            for (uint32_t i = 0; i < grid.x; ++i) {
                uint64_t linear = i + (uint64_t)grid.x * (j + (uint64_t)grid.y * k);
                if (linear < groups) {
                    CHECK(linear < sizeof(seen) && !seen[linear]);
                    seen[linear] = 1;
                    for (unsigned lane = 0; lane < 64; ++lane) {
                        CHECK(linear * 64 + lane == invocations);
                        ++invocations;
                    }
                } else {
                    ++padded;
                }
            }
    CHECK(invocations == (uint64_t)groups * 64);
    CHECK((uint64_t)grid.x * grid.y * grid.z == groups + padded);
    for (uint32_t i = 0; i < groups; ++i) CHECK(seen[i] == 1);
}

int main(void) {
    for (uint32_t x = 0; x <= 7; ++x)
        for (uint32_t y = 0; y <= 5; ++y)
            for (uint32_t z = 0; z <= 4; ++z)
                for (uint32_t groups = 0; groups <= 181; ++groups) check_small(groups, x, y, z);
    CHECK(!vkrt_build_dispatch_grid(1, 1, 1, 1, NULL));
    const uint32_t cases[] = {65534, 65535, 65536, 4194240, 8388608, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct vkrt_build_dispatch_grid grid;
        CHECK(vkrt_build_dispatch_grid(cases[i], 65535, 65535, 65535, &grid));
        CHECK(grid.x <= 65535 && grid.y <= 65535 && grid.z <= 65535);
        CHECK((uint64_t)grid.x * grid.y * grid.z >= cases[i]);
        CHECK(grid.z == 1 || cases[i] > UINT64_C(65535) * 65535);
    }
    struct vkrt_build_dispatch_grid maximum;
    CHECK(vkrt_build_dispatch_grid(UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, &maximum));
    CHECK(maximum.x == UINT32_MAX && maximum.y == 1 && maximum.z == 1);
    printf("Build XYZ dispatch grid: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
