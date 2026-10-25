/* SPDX-License-Identifier: MIT
 *
 * vkrt_common.h - shared types for the VKRT software ray-query layer.
 *
 * Self-contained by design: this layer must build and run without linking
 * Mesa (see vkrt_layer/rayquery_software_rt_layer_design.md). Utilities that
 * Mesa layers take from idep_mesautil are reimplemented here in miniature.
 */

#ifndef VKRT_COMMON_H
#define VKRT_COMMON_H

/* fopen and friends; this layer is not a security boundary. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

#if defined(_WIN32)
#define VKRT_EXPORT __declspec(dllexport)
#define VKRT_CALL VKAPI_CALL
#define VKRT_ALIGN __declspec(align(64))
#define VKRT_THREAD_LOCAL __declspec(thread)
#else
#define VKRT_EXPORT __attribute__((visibility("default")))
#define VKRT_CALL VKAPI_CALL
#define VKRT_ALIGN __attribute__((aligned(64)))
#define VKRT_THREAD_LOCAL __thread
#endif

#ifndef VKRT_ARRAY_SIZE
#define VKRT_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Walk a const pNext chain. Self-contained stand-in for Mesa's
 * vk_foreach_struct_const so we do not need vk_util.h. */
#ifndef vk_foreach_struct_const
#define vk_foreach_struct_const(iter, list)                                    \
   for (const VkBaseOutStructure *iter = (const VkBaseOutStructure *)(list);   \
        iter != NULL;                                                          \
        iter = (const VkBaseOutStructure *)iter->pNext)
#endif

#define VKRT_LOG_ERR(fmt, ...) fprintf(stderr, "VKRT ERROR: " fmt "\n", ##__VA_ARGS__)
#define VKRT_LOG_WARN(fmt, ...) fprintf(stderr, "VKRT WARNING: " fmt "\n", ##__VA_ARGS__)

#if defined(VKRT_DEBUG) || !defined(NDEBUG)
#define VKRT_LOG_DBG(fmt, ...) fprintf(stderr, "VKRT DEBUG: " fmt "\n", ##__VA_ARGS__)
#else
#define VKRT_LOG_DBG(fmt, ...) (void)0
#endif

/* ------------------------------------------------------------------ */
/* Minimal open-addressing map: uint64_t key -> pointer.               */
/* Used to attach layer data to VkInstance / VkDevice / Vk* handles.   */
/* ------------------------------------------------------------------ */

struct vkrt_map_entry {
   uint64_t key;
   void    *value;
};

struct vkrt_map {
   struct vkrt_map_entry *entries;
   size_t                 capacity; /* power of two */
   size_t                 count;
};

static inline void
vkrt_map_init(struct vkrt_map *m)
{
   m->entries  = NULL;
   m->capacity = 0;
   m->count    = 0;
}

static inline void
vkrt_map_destroy(struct vkrt_map *m)
{
   free(m->entries);
   m->entries  = NULL;
   m->capacity = 0;
   m->count    = 0;
}

static inline uint64_t
vkrt_hash64(uint64_t k)
{
   /* splitmix64 finalizer -- good avalanche for pointer-ish keys. */
   k += 0x9e3779b97f4a7c15ull;
   k = (k ^ (k >> 30)) * 0xbf58476d1ce4e5b9ull;
   k = (k ^ (k >> 27)) * 0x94d049bb133111ebull;
   return k ^ (k >> 31);
}

static inline bool
vkrt_map_grow(struct vkrt_map *m)
{
   size_t new_cap = m->capacity ? m->capacity * 2 : 64;
   struct vkrt_map_entry *ne =
      (struct vkrt_map_entry *)calloc(new_cap, sizeof(*ne));
   if (!ne)
      return false;

   for (size_t i = 0; i < m->capacity; i++) {
      if (!m->entries[i].value)
         continue;
      size_t idx = (size_t)vkrt_hash64(m->entries[i].key) & (new_cap - 1);
      while (ne[idx].value)
         idx = (idx + 1) & (new_cap - 1);
      ne[idx] = m->entries[i];
   }

   free(m->entries);
   m->entries  = ne;
   m->capacity = new_cap;
   return true;
}

/* Returns previous value for the key, or NULL if absent. */
static inline void *
vkrt_map_insert(struct vkrt_map *m, uint64_t key, void *value)
{
   if (m->count * 4 >= m->capacity * 3) {
      if (!vkrt_map_grow(m))
         return NULL;
   }

   size_t idx = (size_t)vkrt_hash64(key) & (m->capacity - 1);
   while (m->entries[idx].value) {
      if (m->entries[idx].key == key) {
         void *prev        = m->entries[idx].value;
         m->entries[idx].value = value;
         return prev;
      }
      idx = (idx + 1) & (m->capacity - 1);
   }

   m->entries[idx].key   = key;
   m->entries[idx].value = value;
   m->count++;
   return NULL;
}

static inline void *
vkrt_map_lookup(struct vkrt_map *m, uint64_t key)
{
   if (!m->capacity)
      return NULL;

   size_t idx = (size_t)vkrt_hash64(key) & (m->capacity - 1);
   size_t probes = 0;
   while (m->entries[idx].value && probes < m->capacity) {
      if (m->entries[idx].key == key)
         return m->entries[idx].value;
      idx = (idx + 1) & (m->capacity - 1);
      probes++;
   }
   return NULL;
}

static inline void *
vkrt_map_remove(struct vkrt_map *m, uint64_t key)
{
   if (!m->capacity)
      return NULL;

   size_t idx = (size_t)vkrt_hash64(key) & (m->capacity - 1);
   size_t probes = 0;
   while (m->entries[idx].value && probes < m->capacity) {
      if (m->entries[idx].key == key) {
         void *prev = m->entries[idx].value;
         /* Robin-hood-free tombstone: shift the cluster down. */
         m->entries[idx].key   = 0;
         m->entries[idx].value = NULL;
         m->count--;

         size_t next = (idx + 1) & (m->capacity - 1);
         while (m->entries[next].value) {
            struct vkrt_map_entry e = m->entries[next];
            m->entries[next].key   = 0;
            m->entries[next].value = NULL;
            m->count--;
            vkrt_map_insert(m, e.key, e.value);
            next = (next + 1) & (m->capacity - 1);
         }
         return prev;
      }
      idx = (idx + 1) & (m->capacity - 1);
      probes++;
   }
   return NULL;
}

/* ------------------------------------------------------------------ */
/* Simple non-recursive mutex (SRWLOCK on Windows, futex-free else).   */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK vkrt_mutex_t;
static inline void vkrt_mutex_init(vkrt_mutex_t *m)   { InitializeSRWLock(m); }
static inline void vkrt_mutex_lock(vkrt_mutex_t *m)   { AcquireSRWLockExclusive(m); }
static inline void vkrt_mutex_unlock(vkrt_mutex_t *m) { ReleaseSRWLockExclusive(m); }
#else
#include <pthread.h>
typedef pthread_mutex_t vkrt_mutex_t;
static inline void vkrt_mutex_init(vkrt_mutex_t *m)   { pthread_mutex_init(m, NULL); }
static inline void vkrt_mutex_lock(vkrt_mutex_t *m)   { pthread_mutex_lock(m); }
static inline void vkrt_mutex_unlock(vkrt_mutex_t *m) { pthread_mutex_unlock(m); }
#endif

/* ------------------------------------------------------------------ */
/* Global object map (mirrors vram_report_limit_layer's HKEY scheme).  */
/* ------------------------------------------------------------------ */

#define VKRT_HKEY(obj) ((uint64_t)(uintptr_t)(obj))

struct vkrt_global {
   struct vkrt_map  objects;
   vkrt_mutex_t     objects_lock;
#if defined(_WIN32)
   INIT_ONCE initialization_once;
#else
   pthread_once_t initialization_once;
#endif
};

/* The once state belongs to the registry, not to each translation unit that
 * includes these helpers. In particular, pthread_once_t must have its platform
 * initializer rather than relying on an all-zero representation. */
#if defined(_WIN32)
#define VKRT_GLOBAL_INITIALIZER {.initialization_once = INIT_ONCE_STATIC_INIT}
#else
#define VKRT_GLOBAL_INITIALIZER {.initialization_once = PTHREAD_ONCE_INIT}
#endif

extern struct vkrt_global vkrt_global;

#if defined(_WIN32)
static inline BOOL CALLBACK vkrt_global_initialize(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    vkrt_map_init(&vkrt_global.objects);
    vkrt_mutex_init(&vkrt_global.objects_lock);
    return TRUE;
}
#else
static inline void vkrt_global_initialize(void) {
    /* Map initialization does not allocate. A mutex initialization failure
     * cannot be reported by the void registry API; never publish a registry
     * whose lock is unusable. No live registry is destroyed or reinitialized. */
    vkrt_map_init(&vkrt_global.objects);
    int error = pthread_mutex_init(&vkrt_global.objects_lock, NULL);
    if (error) {
        VKRT_LOG_ERR("object registry mutex initialization failed: %d", error);
        abort();
    }
}
#endif

static inline void
vkrt_global_ensure(void)
{
#if defined(_WIN32)
    if (!InitOnceExecuteOnce(&vkrt_global.initialization_once, vkrt_global_initialize, NULL, NULL)) {
        VKRT_LOG_ERR("object registry initialization failed: %lu", (unsigned long)GetLastError());
        abort();
    }
#else
    int error = pthread_once(&vkrt_global.initialization_once, vkrt_global_initialize);
    if (error) {
        VKRT_LOG_ERR("object registry initialization failed: %d", error);
        abort();
    }
#endif
}

static inline void
vkrt_map_object(uint64_t key, void *value)
{
   vkrt_global_ensure();
   vkrt_mutex_lock(&vkrt_global.objects_lock);
   vkrt_map_insert(&vkrt_global.objects, key, value);
   vkrt_mutex_unlock(&vkrt_global.objects_lock);
}

static inline void *
vkrt_find_object(uint64_t key)
{
   vkrt_global_ensure();
   vkrt_mutex_lock(&vkrt_global.objects_lock);
   void *v = vkrt_map_lookup(&vkrt_global.objects, key);
   vkrt_mutex_unlock(&vkrt_global.objects_lock);
   return v;
}

static inline void *
vkrt_unmap_object(uint64_t key)
{
   vkrt_global_ensure();
   vkrt_mutex_lock(&vkrt_global.objects_lock);
   void *v = vkrt_map_remove(&vkrt_global.objects, key);
   vkrt_mutex_unlock(&vkrt_global.objects_lock);
   return v;
}

#define VKRT_FIND(type, obj) ((type *)vkrt_find_object(VKRT_HKEY(obj)))

/* Note on exporting vkNegotiateLoaderLayerInterfaceVersion:
 *
 * <vulkan/vk_layer.h> declares it as `VKAPI_ATTR VkResult VKAPI_CALL ...`.
 * VKAPI_ATTR expands to nothing on Windows, so the declaration does not
 * export anything. Adding __declspec(dllexport) to our definition trips
 * -Wdll-attribute-on-redeclaration because the prototype already exists.
 *
 * We therefore export it from a .def file (vkrt_layer.def) instead, which
 * is the standard Windows mechanism and avoids the attribute conflict. */

#endif /* VKRT_COMMON_H */
