/* SPDX-License-Identifier: MIT */
#include <stdatomic.h>

/* Delay the real platform mutex initialization so every released worker can
 * enter its first registry call while initialization is still in progress.
 * Keep this scheduling instrument entirely in the fixture: registry helpers,
 * the once primitive and every map operation are the production header code. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
void vkrt_test_mutex_init(SRWLOCK *mutex);
#define InitializeSRWLock vkrt_test_mutex_init
#else
#include <pthread.h>
#include <time.h>
int vkrt_test_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes);
#define pthread_mutex_init vkrt_test_mutex_init
#endif
#include "vkrt_common.h"
#if defined(_WIN32)
#undef InitializeSRWLock
#else
#undef pthread_mutex_init
#endif

/* CMake compiles this file twice. Both translation units must initialize and
 * access the same production registry even when their first calls overlap. */
struct registry_api {
    void (*map)(uint64_t, void *);
    void *(*find)(uint64_t);
    void *(*unmap)(uint64_t);
};

#ifdef VKRT_REGISTRY_WORKER_TU
static void worker_map(uint64_t key, void *value) { vkrt_map_object(key, value); }
static void *worker_find(uint64_t key) { return vkrt_find_object(key); }
static void *worker_unmap(uint64_t key) { return vkrt_unmap_object(key); }
const struct registry_api vkrt_worker_registry_api = {worker_map, worker_find, worker_unmap};
#else

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
extern const struct registry_api vkrt_worker_registry_api;
static atomic_uint initialization_count;

#if defined(_WIN32)
void vkrt_test_mutex_init(SRWLOCK *mutex) {
    atomic_fetch_add_explicit(&initialization_count, 1, memory_order_relaxed);
    Sleep(10);
    InitializeSRWLock(mutex);
}
#else
int vkrt_test_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes) {
    atomic_fetch_add_explicit(&initialization_count, 1, memory_order_relaxed);
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
    return pthread_mutex_init(mutex, attributes);
}
#endif

static void main_map(uint64_t key, void *value) { vkrt_map_object(key, value); }
static void *main_find(uint64_t key) { return vkrt_find_object(key); }
static void *main_unmap(uint64_t key) { return vkrt_unmap_object(key); }
static const struct registry_api main_registry_api = {main_map, main_find, main_unmap};

enum { THREAD_COUNT = 32, SLOT_COUNT = 32, ROUND_COUNT = 64 };
static const uint64_t shared_key = UINT64_MAX;
static const unsigned shared_value = 0x12345678;

struct barrier {
#if defined(_WIN32)
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
#else
    pthread_mutex_t mutex;
    pthread_cond_t condition;
#endif
    unsigned arrived;
    unsigned generation;
};

struct worker {
    unsigned index;
    unsigned failures;
    unsigned values[SLOT_COUNT];
};

static struct barrier start_barrier;
static struct worker workers[THREAD_COUNT];

static void require_platform(bool success) {
    if (!success) {
        fprintf(stderr, "thread synchronization failed\n");
        abort();
    }
}

static void barrier_init(struct barrier *barrier) {
#if defined(_WIN32)
    InitializeCriticalSection(&barrier->mutex);
    InitializeConditionVariable(&barrier->condition);
#else
    require_platform(pthread_mutex_init(&barrier->mutex, NULL) == 0);
    require_platform(pthread_cond_init(&barrier->condition, NULL) == 0);
#endif
}

static void barrier_wait(struct barrier *barrier) {
#if defined(_WIN32)
    EnterCriticalSection(&barrier->mutex);
#else
    require_platform(pthread_mutex_lock(&barrier->mutex) == 0);
#endif
    unsigned generation = barrier->generation;
    if (++barrier->arrived == THREAD_COUNT) {
        barrier->arrived = 0;
        ++barrier->generation;
#if defined(_WIN32)
        WakeAllConditionVariable(&barrier->condition);
#else
        require_platform(pthread_cond_broadcast(&barrier->condition) == 0);
#endif
    } else {
        while (generation == barrier->generation) {
#if defined(_WIN32)
            require_platform(SleepConditionVariableCS(&barrier->condition, &barrier->mutex, INFINITE) != 0);
#else
            require_platform(pthread_cond_wait(&barrier->condition, &barrier->mutex) == 0);
#endif
        }
    }
#if defined(_WIN32)
    LeaveCriticalSection(&barrier->mutex);
#else
    require_platform(pthread_mutex_unlock(&barrier->mutex) == 0);
#endif
}

static void barrier_destroy(struct barrier *barrier) {
#if defined(_WIN32)
    DeleteCriticalSection(&barrier->mutex);
#else
    require_platform(pthread_cond_destroy(&barrier->condition) == 0);
    require_platform(pthread_mutex_destroy(&barrier->mutex) == 0);
#endif
}

static uint64_t marker_key(unsigned index) { return (uint64_t)index + 1; }
static uint64_t slot_key(unsigned index, unsigned slot) { return 0x10000 + (uint64_t)index * SLOT_COUNT + slot; }

static void check(struct worker *worker, bool success) {
    if (!success) ++worker->failures;
}

static void run_worker(struct worker *worker) {
    const struct registry_api *api = (worker->index & 1) ? &main_registry_api : &vkrt_worker_registry_api;
    const struct registry_api *other_api = (worker->index & 1) ? &vkrt_worker_registry_api : &main_registry_api;

    /* Do not call any registry helper before this barrier: every worker races
     * its actual first insertion against first calls in the other TU. */
    barrier_wait(&start_barrier);
    api->map(marker_key(worker->index), worker);
    barrier_wait(&start_barrier);
    for (unsigned i = 0; i < THREAD_COUNT; ++i) check(worker, other_api->find(marker_key(i)) == &workers[i]);

    if (!worker->index) api->map(shared_key, (void *)&shared_value);
    barrier_wait(&start_barrier);
    for (unsigned round = 0; round < ROUND_COUNT; ++round) {
        /* Retain enough objects to grow the map repeatedly while other threads
         * insert, replace, look up and remove colliding clusters. */
        for (unsigned slot = 0; slot < SLOT_COUNT; ++slot) api->map(slot_key(worker->index, slot), &worker->values[slot]);
        for (unsigned slot = 0; slot < SLOT_COUNT; ++slot)
            check(worker, other_api->find(slot_key(worker->index, slot)) == &worker->values[slot]);
        for (unsigned slot = 0; slot < SLOT_COUNT; slot += 2) {
            check(worker, api->unmap(slot_key(worker->index, slot)) == &worker->values[slot]);
            check(worker, other_api->find(slot_key(worker->index, slot)) == NULL);
        }
        api->map(shared_key, (void *)&shared_value);
        check(worker, other_api->find(shared_key) == &shared_value);
        check(worker, other_api->find(marker_key(worker->index)) == worker);
    }

    barrier_wait(&start_barrier);
    check(worker, other_api->find(shared_key) == &shared_value);
    barrier_wait(&start_barrier);
    if (!worker->index) check(worker, api->unmap(shared_key) == &shared_value);
    barrier_wait(&start_barrier);
    check(worker, other_api->find(shared_key) == NULL);
    check(worker, api->unmap(marker_key(worker->index)) == worker);
    for (unsigned slot = 0; slot < SLOT_COUNT; ++slot) {
        void *expected = (slot & 1) ? &worker->values[slot] : NULL;
        check(worker, other_api->unmap(slot_key(worker->index, slot)) == expected);
        check(worker, api->find(slot_key(worker->index, slot)) == NULL);
    }
}

#if defined(_WIN32)
static DWORD WINAPI thread_main(void *data) {
    run_worker(data);
    return 0;
}
#else
static void *thread_main(void *data) {
    run_worker(data);
    return NULL;
}
#endif

int main(void) {
#if defined(_WIN32)
    HANDLE threads[THREAD_COUNT];
#else
    pthread_t threads[THREAD_COUNT];
#endif
    barrier_init(&start_barrier);
    for (unsigned i = 0; i < THREAD_COUNT; ++i) {
        workers[i].index = i;
#if defined(_WIN32)
        threads[i] = CreateThread(NULL, 0, thread_main, &workers[i], 0, NULL);
        require_platform(threads[i] != NULL);
#else
        require_platform(pthread_create(&threads[i], NULL, thread_main, &workers[i]) == 0);
#endif
    }
    unsigned failures = 0;
    for (unsigned i = 0; i < THREAD_COUNT; ++i) {
#if defined(_WIN32)
        require_platform(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0);
        require_platform(CloseHandle(threads[i]) != 0);
#else
        require_platform(pthread_join(threads[i], NULL) == 0);
#endif
        failures += workers[i].failures;
    }
    barrier_destroy(&start_barrier);
    unsigned initializations = atomic_load_explicit(&initialization_count, memory_order_relaxed);
    if (initializations != 1) ++failures;
    if (vkrt_global.objects.count || vkrt_global.objects.capacity <= 64) ++failures;
    if (main_registry_api.find(shared_key) || vkrt_worker_registry_api.find(shared_key)) ++failures;
    /* The production registry lives until process exit. Never reset its once
     * state or destroy/reinitialize its lock to simulate another first call. */
    printf("registry concurrency: %u threads, two translation units, %u rounds, %u initializations, %u failures\n", THREAD_COUNT,
           ROUND_COUNT, initializations, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
#endif
