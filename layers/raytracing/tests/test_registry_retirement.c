/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
static void *registry_calloc(size_t count, size_t size);
#define calloc registry_calloc
#include "vkrt_common.h"
#undef calloc

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
static unsigned failures;
static bool reject_allocations;
static unsigned allocation_attempts;
static void *registry_calloc(size_t count, size_t size) {
    ++allocation_attempts;
    return reject_allocations ? NULL : calloc(count, size);
}
static void check(bool condition) {
    if (!condition) ++failures;
}
int main(void) {
    unsigned first_owner, second_owner;
    uint64_t keys[96];
    reject_allocations = true;
    check(!vkrt_map_object(1, &first_owner) && !vkrt_find_object(1) && !vkrt_global.objects.count);
    check(!vkrt_map_object(2, NULL) && !vkrt_find_object(2) && !vkrt_global.objects.count);
    reject_allocations = false;
    vkrt_global_ensure();
    while (vkrt_global.objects.capacity < 256)
        if (!vkrt_map_grow(&vkrt_global.objects)) return 2;
    /* A collision cluster crossing slot255/0 forces deletion to move aliases
     * behind the scanner. Preserve another live owner's interleaved entries. */
    uint64_t candidate = 1;
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(keys); ++i) {
        while ((vkrt_hash64(candidate) & 255) != 255) ++candidate;
        keys[i] = candidate++;
        vkrt_map_object(keys[i], i % 3 ? &first_owner : &second_owner);
    }
    check(vkrt_global.objects.count == VKRT_ARRAY_SIZE(keys));
    vkrt_unmap_owner_objects(NULL);
    check(vkrt_global.objects.count == VKRT_ARRAY_SIZE(keys));
    vkrt_unmap_owner_objects(&first_owner);
    check(vkrt_global.objects.count == 32);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(keys); ++i) check(vkrt_find_object(keys[i]) == (i % 3 ? NULL : &second_owner));
    /* Cleanup is idempotent and must not consume another owner's replacement. */
    vkrt_map_object(keys[1], &second_owner);
    vkrt_unmap_owner_objects(&first_owner);
    check(vkrt_find_object(keys[1]) == &second_owner && vkrt_global.objects.count == 33);
    vkrt_unmap_owner_objects(&second_owner);
    check(vkrt_global.objects.count == 0);
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(keys); ++i) check(!vkrt_find_object(keys[i]));
    /* A successful device pre-acquires its queue aliases. Repeated void
     * retrieval must not allocate when concurrent objects fill the table. */
    for (uint64_t i = 1; i <= 192; ++i) check(vkrt_map_object(i, &first_owner));
    check(vkrt_global.objects.capacity == 256 && vkrt_global.objects.count == 192);
    reject_allocations = true;
    unsigned previous_attempts = allocation_attempts;
    check(vkrt_map_object(1, &second_owner) && vkrt_find_object(1) == &second_owner);
    check(allocation_attempts == previous_attempts);
    check(!vkrt_map_object(1000000, &first_owner) && !vkrt_find_object(1000000));
    check(vkrt_global.objects.count == 192 && allocation_attempts == previous_attempts + 1);
    vkrt_unmap_owner_objects(&first_owner);
    check(vkrt_find_object(1) == &second_owner && vkrt_global.objects.count == 1);
    vkrt_unmap_owner_objects(&second_owner);
    check(!vkrt_global.objects.count);
    printf("Registry owner retirement: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
