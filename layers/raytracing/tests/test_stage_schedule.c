/* SPDX-License-Identifier: MIT */
#include "vkrt_stage_schedule.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned failures;
#define CHECK(test)                                                 \
    do {                                                            \
        if (!(test)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); \
            ++failures;                                             \
        }                                                           \
    } while (0)
#define AS VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
#define RT VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
#define CS VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
#define COPY VK_PIPELINE_STAGE_2_COPY_BIT
#define TRANSFER_ATOMS (COPY | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT)

static struct vkrt_stage_schedule *create(void) {
    struct vkrt_stage_schedule *s = vkrt_stage_schedule_create();
    if (!s) {
        fputs("Cannot allocate planner\n", stderr);
        exit(1);
    }
    return s;
}

static vkrt_stage_id wait_node(struct vkrt_stage_schedule *s, VkPipelineStageFlags2 original, VkPipelineStageFlags2 expanded,
                               uint64_t cookie) {
    struct vkrt_stage_wait wait = {original, expanded, cookie, true};
    vkrt_stage_id id;
    CHECK(vkrt_stage_schedule_wait(s, &wait, &id) == VKRT_STAGE_OK);
    return id;
}

static vkrt_stage_id work_node(struct vkrt_stage_schedule *s, VkPipelineStageFlags2 logical, VkPipelineStageFlags2 physical,
                               uint64_t cookie, size_t n, const vkrt_stage_id *deps) {
    struct vkrt_stage_work work = {logical, physical, cookie, true, true};
    vkrt_stage_id id;
    CHECK(vkrt_stage_schedule_work(s, &work, n, deps, &id) == VKRT_STAGE_OK);
    return id;
}

static void expect_next(struct vkrt_stage_schedule *s, vkrt_stage_id expected) {
    vkrt_stage_id next;
    CHECK(vkrt_stage_schedule_next(s, &next) == VKRT_STAGE_OK);
    CHECK(next == expected);
}

static void expect_blocked(struct vkrt_stage_schedule *s) {
    vkrt_stage_id next = 123;
    CHECK(vkrt_stage_schedule_next(s, &next) == VKRT_STAGE_BLOCKED);
    CHECK(next == VKRT_STAGE_ID_INVALID);
}

/* Independent progress oracle for a timeline feedback workload. A host waits
 * for a later AS property's availability, then signals a TRANSFER wait. The
 * legal logical execution is prefix -> later producer -> host signal -> copy.
 * A native CS wait (whether at submit start or just before copy) has no step
 * after prefix: it also stalls the later producer's lowered CS. */
static void feedback_progress_oracle(void) {
    struct vkrt_stage_schedule *s = create();
    vkrt_stage_id gate = wait_node(s, VK_PIPELINE_STAGE_2_TRANSFER_BIT, TRANSFER_ATOMS, 10);
    vkrt_stage_id prefix = work_node(s, AS, CS, 20, 0, NULL);
    vkrt_stage_id copy = work_node(s, COPY, CS | COPY, 30, 1, &prefix);
    vkrt_stage_id fence;
    CHECK(vkrt_stage_schedule_full_join(s, VKRT_STAGE_FENCE, 40, &fence) == VKRT_STAGE_OK);
    expect_next(s, prefix);
    CHECK(vkrt_stage_schedule_issue(s, prefix, 100) == VKRT_STAGE_OK);
    expect_blocked(s);

    /* This is a new submission arriving after the blocked first one. */
    vkrt_stage_id producer = work_node(s, AS, CS, 50, 0, NULL);
    expect_next(s, producer);
    CHECK(vkrt_stage_schedule_issue(s, producer, 101) == VKRT_STAGE_OK);
    expect_blocked(s);
    bool producer_available = true; /* Completion of producer token, not fence. */
    CHECK(producer_available);
    CHECK(vkrt_stage_schedule_acquire(s, gate, 102) == VKRT_STAGE_OK);
    expect_next(s, copy);
    CHECK(vkrt_stage_schedule_issue(s, copy, 103) == VKRT_STAGE_OK);
    expect_next(s, fence);
    CHECK(vkrt_stage_schedule_issue(s, fence, 104) == VKRT_STAGE_OK);
    expect_blocked(s);

    /* Separate native-engine oracle, deliberately not using planner edges. */
    enum event { PREFIX_AS, WAIT_CS, QUERY_COPY_CS, LATER_AS_CS };
    const enum event widened_suffix[] = {PREFIX_AS, WAIT_CS, QUERY_COPY_CS, LATER_AS_CS};
    unsigned pc = 0;
    bool later_available = false, signaled = false;
    while (pc < sizeof(widened_suffix) / sizeof(widened_suffix[0])) {
        enum event event = widened_suffix[pc];
        if (event == WAIT_CS && !signaled) break;
        if (event == LATER_AS_CS) later_available = true;
        if (later_available) signaled = true;
        ++pc;
    }
    CHECK(pc == 1 && !later_available && !signaled);
    vkrt_stage_schedule_destroy(s);
}

static void domains_dependencies_and_old_waits(void) {
    struct vkrt_stage_schedule *s = create();
    vkrt_stage_id transfer = wait_node(s, VK_PIPELINE_STAGE_2_TRANSFER_BIT, TRANSFER_ATOMS, 1);
    vkrt_stage_id blocked_copy = work_node(s, COPY, CS | COPY, 2, 0, NULL);
    vkrt_stage_id depends_on_copy = work_node(s, AS, CS, 3, 1, &blocked_copy);
    vkrt_stage_id compute_gate = wait_node(s, CS, CS, 4);
    vkrt_stage_id native_compute = work_node(s, CS, CS, 5, 0, NULL);
    vkrt_stage_id independent_as = work_node(s, AS, CS, 6, 0, NULL);
    vkrt_stage_id independent_rt = work_node(s, RT, CS, 7, 0, NULL);
    expect_next(s, independent_as);
    CHECK(vkrt_stage_schedule_issue(s, independent_as, 11) == VKRT_STAGE_OK);
    expect_next(s, independent_rt);
    CHECK(vkrt_stage_schedule_issue(s, independent_rt, 12) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_issue(s, depends_on_copy, 13) == VKRT_STAGE_BLOCKED);
    CHECK(vkrt_stage_schedule_issue(s, native_compute, 13) == VKRT_STAGE_BLOCKED);
    expect_blocked(s);

    /* A previous wait's stage scope reaches later submissions, independently
     * of intervening waits or physical compute-stage aliasing. */
    vkrt_stage_id later_clear = work_node(s, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, 8, 0, NULL);
    CHECK(vkrt_stage_schedule_issue(s, later_clear, 13) == VKRT_STAGE_BLOCKED);
    CHECK(vkrt_stage_schedule_acquire(s, transfer, 14) == VKRT_STAGE_OK);
    expect_next(s, blocked_copy);
    CHECK(vkrt_stage_schedule_issue(s, blocked_copy, 15) == VKRT_STAGE_OK);
    expect_next(s, depends_on_copy);
    CHECK(vkrt_stage_schedule_issue(s, depends_on_copy, 16) == VKRT_STAGE_OK);
    expect_next(s, later_clear);
    CHECK(vkrt_stage_schedule_issue(s, later_clear, 17) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_acquire(s, compute_gate, 18) == VKRT_STAGE_OK);
    expect_next(s, native_compute);
    vkrt_stage_schedule_destroy(s);
}

static void exact_copy_and_full_scopes(void) {
    struct vkrt_stage_schedule *s = create();
    vkrt_stage_id copy_gate = wait_node(s, COPY, COPY, 1);
    vkrt_stage_id copy = work_node(s, COPY, CS | COPY, 2, 0, NULL);
    vkrt_stage_id clear = work_node(s, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, 3, 0, NULL);
    expect_next(s, clear);
    CHECK(vkrt_stage_schedule_issue(s, clear, 10) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_issue(s, copy, 11) == VKRT_STAGE_BLOCKED);

    const VkPipelineStageFlags2 atoms = AS | RT | CS | TRANSFER_ATOMS;
    vkrt_stage_id all = wait_node(s, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, atoms, 4);
    vkrt_stage_id as = work_node(s, AS, CS, 5, 0, NULL);
    vkrt_stage_id rt = work_node(s, RT, CS, 6, 0, NULL);
    vkrt_stage_id native = work_node(s, CS, CS, 7, 0, NULL);
    expect_blocked(s);
    CHECK(vkrt_stage_schedule_acquire(s, all, 12) == VKRT_STAGE_OK);
    expect_next(s, as);
    CHECK(vkrt_stage_schedule_issue(s, as, 13) == VKRT_STAGE_OK);
    expect_next(s, rt);
    CHECK(vkrt_stage_schedule_issue(s, rt, 14) == VKRT_STAGE_OK);
    expect_next(s, native);
    CHECK(vkrt_stage_schedule_issue(s, native, 15) == VKRT_STAGE_OK);
    expect_blocked(s); /* copy still needs its distinct, older wait. */
    CHECK(vkrt_stage_schedule_acquire(s, copy_gate, 16) == VKRT_STAGE_OK);
    expect_next(s, copy);
    vkrt_stage_schedule_destroy(s);
}

static void joins_and_wait_only_submissions(void) {
    struct vkrt_stage_schedule *s = create();
    /* NONE/BOTTOM has no command consumers, but may not be silently dropped
     * from the queue's completion/fence join. */
    vkrt_stage_id wait = wait_node(s, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, 1);
    vkrt_stage_id signal, fence;
    CHECK(vkrt_stage_schedule_full_join(s, VKRT_STAGE_SIGNAL, 2, &signal) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_full_join(s, VKRT_STAGE_FENCE, 3, &fence) == VKRT_STAGE_OK);
    expect_blocked(s);
    CHECK(vkrt_stage_schedule_acquire(s, wait, 4) == VKRT_STAGE_OK);
    expect_next(s, signal);
    CHECK(vkrt_stage_schedule_issue(s, signal, 5) == VKRT_STAGE_OK);
    expect_next(s, fence);
    struct vkrt_stage_node node;
    CHECK(vkrt_stage_schedule_node(s, fence, &node));
    CHECK(node.dependency_count == 2 && node.dependencies[0] == wait && node.dependencies[1] == signal);
    vkrt_stage_schedule_destroy(s);

    /* The graph can represent an exact signal first scope without forcing a
     * later independent action into it. Frontend, not planner, derives scope. */
    s = create();
    vkrt_stage_id prefix = work_node(s, AS, CS, 10, 0, NULL);
    CHECK(vkrt_stage_schedule_join(s, VKRT_STAGE_SIGNAL, 11, true, 1, &prefix, &signal) == VKRT_STAGE_OK);
    wait = wait_node(s, COPY, COPY, 12);
    vkrt_stage_id copy = work_node(s, COPY, CS | COPY, 13, 0, NULL);
    CHECK(vkrt_stage_schedule_full_join(s, VKRT_STAGE_FENCE, 14, &fence) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_issue(s, prefix, 20) == VKRT_STAGE_OK);
    expect_next(s, signal);
    CHECK(vkrt_stage_schedule_issue(s, signal, 21) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_issue(s, copy, 22) == VKRT_STAGE_BLOCKED);
    CHECK(vkrt_stage_schedule_issue(s, fence, 22) == VKRT_STAGE_BLOCKED);
    CHECK(vkrt_stage_schedule_acquire(s, wait, 23) == VKRT_STAGE_OK);
    vkrt_stage_schedule_destroy(s);
}

static void scoped_signal_first_scope(void) {
    struct vkrt_stage_schedule *s = create();
    vkrt_stage_id copy_wait = wait_node(s, COPY, COPY, 1);
    vkrt_stage_id prefix_as = work_node(s, AS, CS, 2, 0, NULL);
    vkrt_stage_id blocked_copy = work_node(s, COPY, CS | COPY, 3, 0, NULL);
    struct vkrt_stage_signal signal = {AS, AS, 4, true};
    vkrt_stage_id signal_id;
    /* The frontend supplies no additional ordering edge in this graph. This
     * tests first-scope selection, not Vulkan wait/signal ordering inference. */
    CHECK(vkrt_stage_schedule_signal(s, &signal, 0, NULL, &signal_id) == VKRT_STAGE_OK);
    struct vkrt_stage_node node;
    CHECK(vkrt_stage_schedule_node(s, signal_id, &node));
    CHECK(node.original_mask == AS && node.logical_stages == AS);
    CHECK(node.dependency_count == 1 && node.dependencies[0] == prefix_as);
    CHECK(vkrt_stage_schedule_issue(s, prefix_as, 10) == VKRT_STAGE_OK);
    expect_next(s, signal_id);
    CHECK(vkrt_stage_schedule_issue(s, signal_id, 11) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_issue(s, blocked_copy, 12) == VKRT_STAGE_BLOCKED);
    /* Explicit ordering from capture is retained in addition to stage scope. */
    vkrt_stage_id ordering[] = {copy_wait, signal_id};
    signal.cookie = 5;
    CHECK(vkrt_stage_schedule_signal(s, &signal, 2, ordering, &signal_id) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_issue(s, signal_id, 12) == VKRT_STAGE_BLOCKED);
    CHECK(vkrt_stage_schedule_acquire(s, copy_wait, 13) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_issue(s, signal_id, 14) == VKRT_STAGE_OK);
    vkrt_stage_schedule_destroy(s);

    s = create();
    (void)work_node(s, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 1, 0, NULL);
    signal.original_mask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    signal.logical_src_scope = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    CHECK(vkrt_stage_schedule_signal(s, &signal, 0, NULL, &signal_id) == VKRT_STAGE_PARTIAL_SCOPE);
    signal.ordering_complete = false;
    CHECK(vkrt_stage_schedule_signal(s, &signal, 0, NULL, &signal_id) == VKRT_STAGE_INCOMPLETE_CAPTURE);
    CHECK(vkrt_stage_schedule_count(s) == 1);
    vkrt_stage_schedule_destroy(s);
}

static void capture_and_indivisible_blocks(void) {
    struct vkrt_stage_schedule *s = create();
    struct vkrt_stage_wait wait = {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   1, true};
    vkrt_stage_id id;
    CHECK(vkrt_stage_schedule_wait(s, &wait, &id) == VKRT_STAGE_OK);
    struct vkrt_stage_work renderpass = {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         2, true, true};
    CHECK(vkrt_stage_schedule_work(s, &renderpass, 0, NULL, &id) == VKRT_STAGE_PARTIAL_SCOPE);
    CHECK(id == VKRT_STAGE_ID_INVALID && vkrt_stage_schedule_count(s) == 1);
    struct vkrt_stage_work query_block = {AS | COPY, CS | COPY, 3, true, true};
    (void)wait_node(s, COPY, COPY, 4);
    CHECK(vkrt_stage_schedule_work(s, &query_block, 0, NULL, &id) == VKRT_STAGE_PARTIAL_SCOPE);
    struct vkrt_stage_work work = {AS, CS, 5, false, true};
    CHECK(vkrt_stage_schedule_work(s, &work, 0, NULL, &id) == VKRT_STAGE_INCOMPLETE_CAPTURE);
    work.dependencies_complete = true;
    work.replay_complete = false;
    CHECK(vkrt_stage_schedule_work(s, &work, 0, NULL, &id) == VKRT_STAGE_INCOMPLETE_CAPTURE);
    CHECK(vkrt_stage_schedule_join(s, VKRT_STAGE_SIGNAL, 6, false, 0, NULL, &id) == VKRT_STAGE_INCOMPLETE_CAPTURE);
    CHECK(vkrt_stage_schedule_count(s) == 2);
    vkrt_stage_schedule_destroy(s);
}

static void many_waits_and_repeated_secondary(void) {
    struct vkrt_stage_schedule *s = create();
    vkrt_stage_id waits[130];
    for (size_t i = 0; i < 130; ++i) waits[i] = wait_node(s, COPY, COPY, i + 1);
    vkrt_stage_id copy = work_node(s, COPY, CS | COPY, 200, 0, NULL);
    /* Same recording/state cookie executed twice: fresh execution identities. */
    vkrt_stage_id a = work_node(s, AS, CS, 201, 0, NULL);
    vkrt_stage_id b = work_node(s, AS, CS, 201, 0, NULL);
    CHECK(a != b);
    expect_next(s, a);
    CHECK(vkrt_stage_schedule_issue(s, a, 300) == VKRT_STAGE_OK);
    expect_next(s, b);
    CHECK(vkrt_stage_schedule_issue(s, b, 301) == VKRT_STAGE_OK);
    for (size_t i = 0; i < 129; ++i) CHECK(vkrt_stage_schedule_acquire(s, waits[i], 400 + i) == VKRT_STAGE_OK);
    expect_blocked(s);
    CHECK(vkrt_stage_schedule_acquire(s, waits[129], 600) == VKRT_STAGE_OK);
    expect_next(s, copy);
    struct vkrt_stage_node node;
    CHECK(vkrt_stage_schedule_node(s, copy, &node));
    CHECK(node.wait_count == 130 && node.waits[129] == waits[129]);
    vkrt_stage_schedule_destroy(s);
}

static void owned_inputs_and_bad_arguments(void) {
    struct vkrt_stage_schedule *s = create();
    vkrt_stage_id a = work_node(s, AS, CS, 1, 0, NULL);
    vkrt_stage_id deps[] = {a};
    struct vkrt_stage_work work = {AS, CS, 2, true, true};
    vkrt_stage_id b, bad;
    CHECK(vkrt_stage_schedule_work(s, &work, 1, deps, &b) == VKRT_STAGE_OK);
    deps[0] = 1234;
    work.replay_cookie = 999;
    struct vkrt_stage_node node;
    CHECK(vkrt_stage_schedule_node(s, b, &node));
    CHECK(node.cookie == 2 && node.dependencies[0] == a);
    CHECK(vkrt_stage_schedule_work(s, &work, 1, deps, &bad) == VKRT_STAGE_INVALID);
    CHECK(vkrt_stage_schedule_work(s, &work, 1, NULL, &bad) == VKRT_STAGE_INVALID);
    CHECK(vkrt_stage_schedule_work(s, &work, SIZE_MAX, deps, &bad) == VKRT_STAGE_OUT_OF_MEMORY);
    CHECK(vkrt_stage_schedule_count(s) == 2);
    CHECK(vkrt_stage_schedule_issue(s, a, 0) == VKRT_STAGE_INVALID);
    CHECK(vkrt_stage_schedule_acquire(s, a, 1) == VKRT_STAGE_INVALID);
    CHECK(vkrt_stage_schedule_issue(s, b, 1) == VKRT_STAGE_BLOCKED);
    struct vkrt_stage_wait wait = {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, true};
    CHECK(vkrt_stage_schedule_wait(s, &wait, &bad) == VKRT_STAGE_INVALID);
    wait.logical_dst_scope = COPY;
    wait.scope_complete = false;
    CHECK(vkrt_stage_schedule_wait(s, &wait, &bad) == VKRT_STAGE_INCOMPLETE_CAPTURE);
    wait.scope_complete = true;
    CHECK(vkrt_stage_schedule_wait(s, &wait, &bad) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_node(s, bad, &node));
    CHECK(node.original_mask == VK_PIPELINE_STAGE_2_TRANSFER_BIT && node.logical_stages == COPY);
    CHECK(vkrt_stage_schedule_acquire(s, bad, 0) == VKRT_STAGE_INVALID);
    CHECK(vkrt_stage_schedule_acquire(s, bad, 2) == VKRT_STAGE_OK);
    CHECK(vkrt_stage_schedule_acquire(s, bad, 3) == VKRT_STAGE_INVALID);
    CHECK(vkrt_stage_schedule_join(s, VKRT_STAGE_WORK, 0, true, 0, NULL, &bad) == VKRT_STAGE_INVALID);
    CHECK(!vkrt_stage_schedule_node(s, SIZE_MAX, &node));
    CHECK(vkrt_stage_schedule_next(NULL, &bad) == VKRT_STAGE_INVALID);
    CHECK(vkrt_stage_schedule_next(s, NULL) == VKRT_STAGE_INVALID);
    vkrt_stage_schedule_destroy(s);
    vkrt_stage_schedule_destroy(NULL);
}

int main(void) {
    feedback_progress_oracle();
    domains_dependencies_and_old_waits();
    exact_copy_and_full_scopes();
    joins_and_wait_only_submissions();
    scoped_signal_first_scope();
    capture_and_indivisible_blocks();
    many_waits_and_repeated_secondary();
    owned_inputs_and_bad_arguments();
    if (failures)
        fprintf(stderr, "%u stage-schedule checks failed\n", failures);
    else
        puts("stage schedule: dependency, scope, and feedback progress oracles passed");
    return failures ? 1 : 0;
}
