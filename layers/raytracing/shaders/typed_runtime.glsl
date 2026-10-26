// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// One invocation owns one checked BDA arena. Nodes are dynamically linked;
// suspension, traversal and nested stage calls do not unroll the source CFG.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
#ifdef RT_CONTEXT_PUSH
layout(push_constant, std430) uniform RtContextPush {
    uint low;
    uint high;
} rt_context_push;
#else
layout(constant_id = 0) const uint rt_context_low = 0u;
layout(constant_id = 1) const uint rt_context_high = 0u;
#endif
layout(buffer_reference, std430, buffer_reference_align = 4) buffer RtWords { uint w[]; };
#ifdef RT_PRIVATE_WORDS
uint vel_rt_private_frames[RT_PRIVATE_WORDS];
#endif
uint64_t rt_ctx, rt_arena, rt_limit, rt_cursor, rt_top;
uint rt_error;
uvec3 rt_launch, rt_launch_size;

uint rt_p(uint64_t p) { return RtWords(p).w[0]; }
void rt_pw(uint64_t p, uint v) { RtWords(p).w[0] = v; }
uint64_t rt_p64(uint64_t p) { return uint64_t(rt_p(p)) | (uint64_t(rt_p(p + 4ul)) << 32); }
uint rt_u(uint64_t p) {
#ifdef RT_PRIVATE_WORDS
    return vel_rt_private_frames[uint(p / 4ul)];
#else
    return rt_p(p);
#endif
}
void rt_w(uint64_t p, uint v) {
#ifdef RT_PRIVATE_WORDS
    vel_rt_private_frames[uint(p / 4ul)] = v;
#else
    rt_pw(p, v);
#endif
}
uint64_t rt_u64(uint64_t p) { return uint64_t(rt_u(p)) | (uint64_t(rt_u(p + 4ul)) << 32); }
void rt_w64(uint64_t p, uint64_t v) {
    rt_w(p, uint(v));
    rt_w(p + 4ul, uint(v >> 32));
}
float rt_f(uint64_t p) { return uintBitsToFloat(rt_u(p)); }
void rt_wf(uint64_t p, float v) { rt_w(p, floatBitsToUint(v)); }
vec3 rt_v3(uint64_t p) { return vec3(rt_f(p), rt_f(p + 4ul), rt_f(p + 8ul)); }
void rt_wv3(uint64_t p, vec3 v) {
    rt_wf(p, v.x);
    rt_wf(p + 4ul, v.y);
    rt_wf(p + 8ul, v.z);
}
void rt_zero(uint64_t p, uint64_t n) {
    for (uint64_t i = 0ul; i < n; i += 4ul) rt_w(p + i, 0u);
}
// Typed frame offsets can be byte-aligned (u8/u16/packed composites). Byte
// copies preserve neighboring components; arenas never have concurrent writers.
void rt_copy(uint64_t dst, uint64_t src, uint n) {
    for (uint i = 0u; i < n; ++i) {
        uint64_t s = src + uint64_t(i), d = dst + uint64_t(i);
        uint byteValue = (rt_u(s & ~3ul) >> (uint(s & 3ul) * 8u)) & 255u;
        uint shift = uint(d & 3ul) * 8u;
        uint64_t aligned = d & ~3ul;
        rt_w(aligned, (rt_u(aligned) & ~(255u << shift)) | (byteValue << shift));
    }
}
void rt_fail(uint code) {
    if (rt_error == 0u) rt_error = code;
}
bool rt_product(uint64_t a, uint64_t b, out uint64_t v) {
    // Exact full-width multiply with explicit limbs. Besides preserving the
    // 64-bit pool/launch ABI, this avoids a native compiler failure on the
    // otherwise equivalent UINT64_MAX / b overflow-bound idiom.
    uint64_t al = a & 0xfffffffful, ah = a >> 32, bl = b & 0xfffffffful, bh = b >> 32;
    uint64_t low = al * bl, one = ah * bl, two = al * bh;
    v = 0ul;
    if ((ah != 0ul && bh != 0ul) || one > 0xfffffffful || two > 0xfffffffful) return false;
    uint64_t high = one + two + (low >> 32);
    if (high > 0xfffffffful) return false;
    v = (high << 32) | (low & 0xfffffffful);
    return true;
}
uint64_t rt_alloc(uint64_t bytes) {
    if (bytes > ~0ul - 15ul) {
        rt_fail(VEL_RT_STATUS_STACK_OVERFLOW);
        return 0ul;
    }
    bytes = (bytes + 15ul) & ~15ul;
    if (rt_cursor > rt_limit || bytes > rt_limit - rt_cursor) {
        rt_fail(VEL_RT_STATUS_STACK_OVERFLOW);
        return 0ul;
    }
    uint64_t p = rt_cursor;
    rt_cursor += bytes;
    rt_zero(p, bytes);
    return p;
}
uint64_t rt_frame(uint64_t node) { return rt_u64(node + 24ul); }
uint64_t rt_control(uint64_t node) { return rt_frame(node) + uint64_t(rt_control_offset[rt_u(node + 4ul)]); }
void rt_pop(uint64_t node) {
    rt_cursor = rt_u64(node + 16ul);
    rt_top = rt_u64(node + 8ul);
}
void rt_ready(uint64_t shader) { rt_w(rt_control(shader) + 8ul, 1u); }
uint64_t rt_owner_frame(uint64_t node) {
    if (node == 0ul) return 0ul;
    if (rt_u(node) == 1u) return rt_frame(node);
    return rt_frame(rt_u64(node + 8ul));
}
bool rt_sbt(uint region, uint64_t index, out uint group, out uint64_t record) {
    uint64_t p = rt_ctx + uint64_t(region), base = rt_p64(p), stride = rt_p64(p + 8ul), size = rt_p64(p + 16ul);
    group = 0xffffffffu;
    record = 0ul;
    if (base == 0ul && size == 0ul) return true;
    if (base == 0ul || (base & 3ul) != 0ul || stride < 32ul || stride % 4ul != 0ul || size < 32ul ||
        index > (size - 32ul) / stride) {
        rt_fail(VEL_RT_STATUS_SBT);
        return false;
    }
    uint64_t offset = index * stride;
    if (offset > ~0ul - base || 32ul > ~0ul - (base + offset)) {
        rt_fail(VEL_RT_STATUS_SBT);
        return false;
    }
    uint64_t h = base + offset;
    bool zeroHandle = true;
    for (uint i = 0u; i < 8u; ++i) zeroHandle = zeroHandle && rt_p(h + uint64_t(i) * 4ul) == 0u;
    if (zeroHandle) {
        group = 0xffffffffu;
        record = h + 32ul;
        return true;
    }
    group = rt_p(h);
    uint lo = uint(rt_pipeline_id), hi = uint(rt_pipeline_id >> 32);
    if (group >= rt_group_count || rt_p(h + 4ul) != VEL_RT_SBT_HANDLE_MAGIC || rt_p(h + 8ul) != lo || rt_p(h + 12ul) != hi ||
        rt_p(h + 16ul) != (group ^ 0xa511e9b3u) || rt_p(h + 20ul) != ~group || rt_p(h + 24ul) != (lo ^ 0x63d83595u) ||
        rt_p(h + 28ul) != (hi ^ 0x9e3779b9u)) {
        rt_fail(VEL_RT_STATUS_SBT);
        return false;
    }
    record = h + 32ul;
    return true;
}
bool rt_general_record(uint region, uint index, uint model, out uint stage, out uint64_t record) {
    uint group;
    if (!rt_sbt(region, uint64_t(index), group, record)) return false;
    if (group == 0xffffffffu) {
        stage = 0xffffffffu;
        return true;
    }
    stage = rt_general[group];
    if (rt_group_type[group] != 0u || stage >= rt_stage_count || rt_models[stage] != model) {
        rt_fail(VEL_RT_STATUS_SBT);
        return false;
    }
    return true;
}
void rt_system(uint64_t frame, uint64_t inherited, uint64_t record, uint stage) {
    if (inherited != 0ul) rt_copy(frame, inherited, VEL_RT_SYSTEM_BYTES);
    for (uint i = 0u; i < 3u; ++i) {
        rt_w(frame + uint64_t(VEL_RT_SYS_LAUNCH_ID + i * 4u), rt_launch[i]);
        rt_w(frame + uint64_t(VEL_RT_SYS_LAUNCH_SIZE + i * 4u), rt_launch_size[i]);
    }
    rt_w64(frame + uint64_t(VEL_RT_SYS_SHADER_RECORD_ADDRESS), record);
    uint64_t pushes = rt_p64(rt_ctx + uint64_t(VEL_RT_RUN_PUSH_TABLE));
    rt_w64(frame + uint64_t(VEL_RT_SYS_APP_PUSH_ADDRESS), pushes == 0ul ? 0ul : rt_p64(pushes + uint64_t(stage) * 8ul));
    rt_w64(frame + uint64_t(VEL_RT_SYS_CONTEXT_ADDRESS), rt_ctx);
}
uint64_t rt_push_shader(uint stage, uint64_t record, uint64_t parent, uint64_t owner, uint action, uint depth, uint64_t payload,
                        uint payloadSize, uint payloadType, uint64_t attrs, uint attrSize, uint attrType) {
    if (stage >= rt_stage_count) {
        rt_fail(VEL_RT_STATUS_SBT);
        return 0ul;
    }
    uint64_t node = rt_alloc(128ul + rt_frame_size[stage]);
    if (node == 0ul) return 0ul;
    rt_w(node, 1u);
    rt_w(node + 4ul, stage);
    rt_w64(node + 8ul, parent);
    rt_w64(node + 16ul, node);
    rt_w64(node + 24ul, node + 128ul);
    rt_w64(node + 32ul, owner);
    rt_w64(node + 40ul, payload);
    rt_w(node + 60ul, action);
    rt_w(node + 64ul, depth);
    uint64_t frame = node + 128ul;
    rt_system(frame, rt_owner_frame(parent), record, stage);
    uvec4 data = rt_interface(stage, action == 1u ? 5329u : 5342u);
    if (data.w != 0u) {
        if (data.y != payloadSize || data.z != payloadType || payload == 0ul) {
            rt_fail(VEL_RT_STATUS_PAYLOAD_TYPE);
            return 0ul;
        }
        rt_copy(frame + uint64_t(data.x), payload, data.y);
        rt_w64(node + 48ul, uint64_t(data.x));
        rt_w(node + 56ul, data.y);
    }
    uvec4 attrInterface = rt_interface(stage, 5339u);
    if (attrInterface.w != 0u && attrs != 0ul) {
        if (attrType != rt_triangle_type && (attrInterface.y != attrSize || attrInterface.z != attrType)) {
            rt_fail(VEL_RT_STATUS_ATTRIBUTE_TYPE);
            return 0ul;
        }
        // Triangle barycentrics have a built-in eight-byte representation,
        // including when consumed through a float array/block. Extra declared
        // components have no defined triangle value; leave their fresh frame
        // bytes untouched. In particular, an unused larger declaration must
        // not suppress execution of an otherwise valid hit shader.
        rt_copy(frame + uint64_t(attrInterface.x), attrs, min(attrSize, attrInterface.y));
    }
    rt_top = node;
    return node;
}
void rt_writeback(uint64_t shader) {
    uint n = rt_u(shader + 56ul);
    if (n != 0u) rt_copy(rt_u64(shader + 40ul), rt_frame(shader) + rt_u64(shader + 48ul), n);
}
sw_hit rt_load_hit(uint64_t p) {
    sw_hit h;
    h.instance_id = rt_u(p);
    h.instance_custom_index = rt_u(p + 4ul);
    h.geometry_index = rt_u(p + 8ul);
    h.primitive_index = rt_u(p + 12ul);
    h.barycentrics = vec2(rt_f(p + 16ul), rt_f(p + 20ul));
    h.distance = rt_f(p + 24ul);
    h.front_face = rt_u(p + 28ul) != 0u;
    h.opaque = rt_u(p + 32ul) != 0u;
    h.instance_node = rt_u(p + 36ul);
    return h;
}
void rt_store_hit(uint64_t p, sw_hit h) {
    rt_w(p, h.instance_id);
    rt_w(p + 4ul, h.instance_custom_index);
    rt_w(p + 8ul, h.geometry_index);
    rt_w(p + 12ul, h.primitive_index);
    rt_wf(p + 16ul, h.barycentrics.x);
    rt_wf(p + 20ul, h.barycentrics.y);
    rt_wf(p + 24ul, h.distance);
    rt_w(p + 28ul, h.front_face ? 1u : 0u);
    rt_w(p + 32ul, h.opaque ? 1u : 0u);
    rt_w(p + 36ul, h.instance_node);
}
sw_ray_query_state rt_load_q(uint64_t node) {
    uint64_t p = node + 256ul;
    sw_ray_query_state q;
    q.as_address = rt_u64(p);
    q.ray.origin = rt_v3(p + 8ul);
    q.ray.direction = rt_v3(p + 20ul);
    q.ray.inv_direction = rt_v3(p + 32ul);
    q.tmin = rt_f(p + 44ul);
    q.tclosest = rt_f(p + 48ul);
    q.ray_flags = rt_u(p + 52ul);
    q.cull_mask = rt_u(p + 56ul);
    q.intersection_type = rt_u(p + 60ul);
    q.committed_intersection_type = rt_u(p + 64ul);
    q.candidate = rt_load_hit(p + 68ul);
    q.committed = rt_load_hit(p + 108ul);
    q.tlas_next = rt_u(p + 148ul);
    q.blas_next = rt_u(p + 152ul);
    q.current_instance = rt_u(p + 156ul);
    q.blas_active = rt_u(p + 160ul) != 0u;
    q.gen_thit = rt_f(p + 164ul);
    q.want_committed = rt_u(p + 168ul);
    return q;
}
void rt_store_q(uint64_t node, sw_ray_query_state q) {
    uint64_t p = node + 256ul;
    rt_w64(p, q.as_address);
    rt_wv3(p + 8ul, q.ray.origin);
    rt_wv3(p + 20ul, q.ray.direction);
    rt_wv3(p + 32ul, q.ray.inv_direction);
    rt_wf(p + 44ul, q.tmin);
    rt_wf(p + 48ul, q.tclosest);
    rt_w(p + 52ul, q.ray_flags);
    rt_w(p + 56ul, q.cull_mask);
    rt_w(p + 60ul, q.intersection_type);
    rt_w(p + 64ul, q.committed_intersection_type);
    rt_store_hit(p + 68ul, q.candidate);
    rt_store_hit(p + 108ul, q.committed);
    rt_w(p + 148ul, q.tlas_next);
    rt_w(p + 152ul, q.blas_next);
    rt_w(p + 156ul, q.current_instance);
    rt_w(p + 160ul, q.blas_active ? 1u : 0u);
    rt_wf(p + 164ul, q.gen_thit);
    rt_w(p + 168ul, q.want_committed);
}
// q is the exact shared RQ BVH4 state, stored in the trace node across calls.
void rt_hit_system(uint64_t shader, uint64_t trace, bool committed, uint kind, float distance) {
    uint64_t f = rt_frame(shader);
    sw_ray_query_state q = rt_load_q(trace);
    q.want_committed = committed ? 1u : 0u;
    rt_w(f + uint64_t(VEL_RT_SYS_RAY_FLAGS), q.ray_flags);
    rt_w(f + uint64_t(VEL_RT_SYS_CULL_MASK), rt_u(trace + 176ul));
    rt_wv3(f + uint64_t(VEL_RT_SYS_WORLD_ORIGIN), q.ray.origin);
    rt_wv3(f + uint64_t(VEL_RT_SYS_WORLD_DIRECTION), q.ray.direction);
    rt_wf(f + uint64_t(VEL_RT_SYS_TMIN), q.tmin);
    rt_wf(f + uint64_t(VEL_RT_SYS_TMAX), distance);
    rt_w(f + uint64_t(VEL_RT_SYS_HIT_KIND), kind);
    bool hit = committed ? q.committed_intersection_type != SW_COMM_NONE : q.intersection_type != SW_CAND_NONE;
    if (hit) {
        rt_w(f + uint64_t(VEL_RT_SYS_INSTANCE_CUSTOM_INDEX), uint(sw_get_intersection_instance_custom_index(q)));
        rt_w(f + uint64_t(VEL_RT_SYS_INSTANCE_ID), uint(sw_get_intersection_instance_id(q)));
        rt_w(f + uint64_t(VEL_RT_SYS_GEOMETRY_INDEX), uint(sw_get_intersection_geometry_index(q)));
        rt_w(f + uint64_t(VEL_RT_SYS_PRIMITIVE_ID), uint(sw_get_intersection_primitive_index(q)));
        rt_wv3(f + uint64_t(VEL_RT_SYS_OBJECT_ORIGIN), sw_get_object_ray_origin(q));
        rt_wv3(f + uint64_t(VEL_RT_SYS_OBJECT_DIRECTION), sw_get_object_ray_direction(q));
        mat4x3 otw = sw_get_object_to_world(q), wto = sw_get_world_to_object(q);
        for (uint c = 0u; c < 4u; ++c) {
            rt_wv3(f + uint64_t(VEL_RT_SYS_OBJECT_TO_WORLD + c * 12u), otw[c]);
            rt_wv3(f + uint64_t(VEL_RT_SYS_WORLD_TO_OBJECT + c * 12u), wto[c]);
        }
    }
}
bool rt_hit_record(uint64_t trace, bool committed, out uint group, out uint64_t record) {
    sw_ray_query_state q = rt_load_q(trace);
    q.want_committed = committed ? 1u : 0u;
    uint64_t index = uint64_t(sw_get_intersection_instance_sbt_offset(q)) + uint64_t(rt_u(trace + 72ul) & 15u) +
                     uint64_t(uint(sw_get_intersection_geometry_index(q))) * uint64_t(rt_u(trace + 76ul) & 15u);
    if (!rt_sbt(VEL_RT_RUN_HIT_ADDRESS, index, group, record)) return false;
    if (group == 0xffffffffu) return true;
    uint expected =
        (committed ? q.committed_intersection_type == SW_COMM_TRIANGLE : q.intersection_type == SW_CAND_TRIANGLE) ? 1u : 2u;
    if (rt_group_type[group] != expected) {
        rt_fail(VEL_RT_STATUS_SBT);
        return false;
    }
    return true;
}
void rt_complete_trace(uint64_t trace) {
    uint64_t parent = rt_u64(trace + 8ul);
    rt_pop(trace);
    rt_ready(parent);
}
void rt_accept_report(uint64_t trace, bool accepted, bool terminateRay) {
    uint64_t intersection = rt_u64(trace + 112ul), control = rt_control(intersection);
    if (accepted) {
        sw_ray_query_state q = rt_load_q(trace);
        q.gen_thit = rt_f(trace + 104ul);
        sw_ray_query_generate_intersection(q);
        rt_store_q(trace, q);
        rt_copy(trace + 512ul + rt_attr_stride, trace + 512ul, rt_u(trace + 100ul));
        rt_w(trace + 84ul, rt_u(trace + 92ul));
        rt_w(trace + 88ul, rt_u(trace + 96ul));
        rt_w(trace + 148ul, rt_u(trace + 100ul));
        rt_wf(rt_frame(intersection) + uint64_t(VEL_RT_SYS_TMAX), q.tclosest);
        if (terminateRay || (q.ray_flags & SW_RAY_FLAG_TERMINATE_ON_FIRST_HIT) != 0u) {
            sw_ray_query_terminate(q);
            rt_store_q(trace, q);
            // Ray termination abandons the active intersection invocation.
            // In particular no later Report may replace the accepted first hit.
            rt_pop(intersection);
            return;
        }
    }
    rt_w(control + 12ul, accepted ? 1u : 0u);
    rt_ready(intersection);
}
void rt_shader_done(uint64_t shader, uint disposition) {
    uint action = rt_u(shader + 60ul);
    uint64_t trace = rt_u64(shader + 32ul), parent = rt_u64(shader + 8ul);
    rt_writeback(shader);
    rt_pop(shader);
    if (action == 0u) return;
    if (action == 1u) {
        rt_ready(parent);
        return;
    }
    if (action == 2u) {
        rt_complete_trace(trace);
        return;
    }
    if (action == 3u) {
        if (disposition != 4u) {
            sw_ray_query_state q = rt_load_q(trace);
            sw_ray_query_confirm(q);
            if (disposition == 5u) sw_ray_query_terminate(q);
            rt_store_q(trace, q);
        }
        return;
    }
    if (action == 4u) return;
    if (action == 5u) {
        rt_accept_report(trace, disposition != 4u, disposition == 5u);
        return;
    }
    rt_fail(VEL_RT_STATUS_REQUEST);
}
void rt_begin_trace(uint64_t shader) {
    uint depth = rt_u(shader + 64ul);
    if (depth >= rt_max_depth) {
        rt_fail(VEL_RT_STATUS_RECURSION);
        return;
    }
    uint64_t c = rt_control(shader), node = rt_alloc(512ul + 2ul * rt_attr_stride);
    if (node == 0ul) return;
    rt_w(node, 2u);
    rt_w64(node + 8ul, shader);
    rt_w64(node + 16ul, node);
    rt_w(node + 64ul, depth + 1u);
    rt_w(node + 72ul, rt_u(c + 36ul));
    rt_w(node + 76ul, rt_u(c + 40ul));
    rt_w(node + 80ul, rt_u(c + 44ul) & 65535u);
    rt_w(node + 128ul, rt_type(rt_u(shader + 4ul), rt_u(c + 96ul)));
    rt_w(node + 132ul, rt_u(c + 84ul));
    rt_w64(node + 136ul, rt_frame(shader) + uint64_t(rt_u(c + 80ul)));
    rt_w(node + 176ul, rt_u(c + 32ul) & 255u);
    sw_ray_query_state q = rt_load_q(node);
    q.as_address = vel_rt_resolve_as(rt_p64(rt_ctx + uint64_t(VEL_RT_RUN_AS_REGISTRY)), rt_u64(c + 16ul));
    q.ray_flags = rt_u(c + 28ul);
    q.cull_mask = rt_u(c + 32ul) & 255u;
    q.ray.origin = rt_v3(c + 48ul);
    q.ray.direction = rt_v3(c + 64ul);
    q.tmin = rt_f(c + 60ul);
    q.tclosest = rt_f(c + 76ul);
    sw_ray_query_initialize_data(q);
    rt_store_q(node, q);
    rt_top = node;
}
void rt_callable(uint64_t shader) {
    uint64_t c = rt_control(shader), record;
    uint stage;
    if (!rt_general_record(VEL_RT_RUN_CALLABLE_ADDRESS, rt_u(c + 16ul), 5318u, stage, record)) return;
    if (stage == 0xffffffffu) {
        rt_ready(shader);
        return;
    }
    rt_push_shader(stage, record, shader, 0ul, 1u, rt_u(shader + 64ul), rt_frame(shader) + uint64_t(rt_u(c + 80ul)), rt_u(c + 84ul),
                   rt_type(rt_u(shader + 4ul), rt_u(c + 96ul)), 0ul, 0u, 0u);
}
void rt_report(uint64_t shader) {
    uint64_t trace = rt_u64(shader + 32ul), c = rt_control(shader);
    if (rt_u(shader + 60ul) != 4u || trace == 0ul) {
        rt_fail(VEL_RT_STATUS_REQUEST);
        return;
    }
    sw_ray_query_state q = rt_load_q(trace);
    float distance = rt_f(c + 16ul);
    if (!(q.tmin <= distance && distance <= q.tclosest)) {
        rt_w(c + 12ul, 0u);
        rt_ready(shader);
        return;
    }
    uint size = rt_u(c + 84ul), type = rt_type(rt_u(shader + 4ul), rt_u(c + 96ul));
    if (uint64_t(size) > rt_attr_stride) {
        rt_fail(VEL_RT_STATUS_ATTRIBUTE_TYPE);
        return;
    }
    rt_copy(trace + 512ul, rt_frame(shader) + uint64_t(rt_u(c + 80ul)), size);
    rt_w64(trace + 112ul, shader);
    rt_w(trace + 92ul, rt_u(c + 20ul));
    rt_w(trace + 96ul, type);
    rt_w(trace + 100ul, size);
    rt_wf(trace + 104ul, distance);
    uint group = rt_u(trace + 108ul), any = rt_any[group];
    if (any == 0xffffffffu || q.candidate.opaque) {
        rt_accept_report(trace, true, false);
        return;
    }
    uint64_t ah = rt_push_shader(any, rt_u64(trace + 152ul), shader, trace, 5u, rt_u(trace + 64ul), rt_u64(trace + 136ul),
                                 rt_u(trace + 132ul), rt_u(trace + 128ul), trace + 512ul, size, type);
    if (ah != 0ul) rt_hit_system(ah, trace, false, rt_u(c + 20ul), distance);
}
void rt_trace_step(uint64_t trace) {
    sw_ray_query_state q = rt_load_q(trace);
    bool candidate = sw_ray_query_proceed(q);
    rt_store_q(trace, q);
    if (candidate) {
        uint group;
        uint64_t record;
        if (!rt_hit_record(trace, false, group, record)) return;
        if (group == 0xffffffffu) {
            if (q.intersection_type == SW_CAND_TRIANGLE) {
                sw_ray_query_confirm(q);
                rt_store_q(trace, q);
            }
            return;
        }
        rt_w(trace + 108ul, group);
        rt_w64(trace + 152ul, record);
        if (q.intersection_type == SW_CAND_AABB) {
            uint64_t child =
                rt_push_shader(rt_intersection[group], record, trace, trace, 4u, rt_u(trace + 64ul), 0ul, 0u, 0u, 0ul, 0u, 0u);
            if (child != 0ul) rt_hit_system(child, trace, false, 0u, q.tclosest);
            return;
        }
        uint any = rt_any[group];
        if (any == 0xffffffffu) {
            sw_ray_query_confirm(q);
            rt_store_q(trace, q);
            return;
        }
        rt_wf(trace + 512ul, q.candidate.barycentrics.x);
        rt_wf(trace + 516ul, q.candidate.barycentrics.y);
        uint64_t child = rt_push_shader(any, record, trace, trace, 3u, rt_u(trace + 64ul), rt_u64(trace + 136ul),
                                        rt_u(trace + 132ul), rt_u(trace + 128ul), trace + 512ul, 8u, rt_triangle_type);
        if (child != 0ul) rt_hit_system(child, trace, false, sw_get_intersection_hit_kind(q), q.candidate.distance);
        return;
    }
    uint stage;
    uint64_t record;
    bool committed = q.committed_intersection_type != SW_COMM_NONE;
    uint kind = 0u, attrSize = 0u, attrType = 0u;
    uint64_t attrs = 0ul;
    if (committed) {
        if ((q.ray_flags & SW_RAY_FLAG_SKIP_CLOSEST_HIT) != 0u) {
            rt_complete_trace(trace);
            return;
        }
        uint group;
        if (!rt_hit_record(trace, true, group, record)) return;
        if (group == 0xffffffffu) {
            rt_complete_trace(trace);
            return;
        }
        stage = rt_closest[group];
        if (stage == 0xffffffffu) {
            rt_complete_trace(trace);
            return;
        }
        if (q.committed_intersection_type == SW_COMM_TRIANGLE) {
            kind = sw_get_intersection_hit_kind(q);
            attrs = trace + 512ul;
            attrSize = 8u;
            attrType = rt_triangle_type;
            rt_wf(attrs, q.committed.barycentrics.x);
            rt_wf(attrs + 4ul, q.committed.barycentrics.y);
        } else {
            kind = rt_u(trace + 84ul);
            attrs = trace + 512ul + rt_attr_stride;
            attrSize = rt_u(trace + 148ul);
            attrType = rt_u(trace + 88ul);
        }
    } else if (!rt_general_record(VEL_RT_RUN_MISS_ADDRESS, rt_u(trace + 80ul), 5317u, stage, record))
        return;
    if (stage == 0xffffffffu) {
        rt_complete_trace(trace);
        return;
    }
    uint64_t child = rt_push_shader(stage, record, trace, trace, 2u, rt_u(trace + 64ul), rt_u64(trace + 136ul), rt_u(trace + 132ul),
                                    rt_u(trace + 128ul), attrs, attrSize, attrType);
    if (child != 0ul) rt_hit_system(child, trace, committed, kind, q.tclosest);
}
void main() {
#ifdef RT_CONTEXT_PUSH
    rt_ctx = uint64_t(rt_context_push.low) | (uint64_t(rt_context_push.high) << 32);
#else
    rt_ctx = uint64_t(rt_context_low) | (uint64_t(rt_context_high) << 32);
#endif
    rt_error = 0u;
    rt_top = 0ul;
    if (rt_ctx == 0ul || (rt_ctx & 7ul) != 0ul || rt_p(rt_ctx) != VEL_RT_RUNTIME_MAGIC ||
        rt_p(rt_ctx + 4ul) != VEL_RT_RUNTIME_VERSION)
        return;
    uint64_t dispatchX = uint64_t(gl_NumWorkGroups.x), dispatchY = uint64_t(gl_NumWorkGroups.y), row, localIndex;
    if (!rt_product(dispatchY, uint64_t(gl_GlobalInvocationID.z), row) || row > ~0ul - uint64_t(gl_GlobalInvocationID.y)) return;
    row += uint64_t(gl_GlobalInvocationID.y);
    if (!rt_product(dispatchX, row, localIndex) || localIndex > ~0ul - uint64_t(gl_GlobalInvocationID.x)) return;
    localIndex += uint64_t(gl_GlobalInvocationID.x);
    if (localIndex >= rt_p64(rt_ctx + uint64_t(VEL_RT_RUN_INVOCATION_COUNT))) return;
    uint64_t status = rt_p64(rt_ctx + uint64_t(VEL_RT_RUN_STATUS_ADDRESS));
    if (localIndex >= rt_p64(rt_ctx + uint64_t(VEL_RT_RUN_STATUS_COUNT)) || localIndex > (~0ul - status) / 4ul)
        status = 0ul;
    else if (status != 0ul)
        status += localIndex * 4ul;
    rt_launch_size = uvec3(rt_p(rt_ctx + 40ul), rt_p(rt_ctx + 44ul), rt_p(rt_ctx + 48ul));
    uint64_t xy, total, ray = rt_p64(rt_ctx + 56ul);
    if (any(equal(rt_launch_size, uvec3(0u))) || !rt_product(uint64_t(rt_launch_size.x), uint64_t(rt_launch_size.y), xy) ||
        !rt_product(xy, uint64_t(rt_launch_size.z), total) || ray > ~0ul - localIndex)
        rt_fail(VEL_RT_STATUS_CONTEXT);
    else {
        ray += localIndex;
        if (ray >= total)
            rt_fail(VEL_RT_STATUS_CONTEXT);
        else
            rt_launch = uvec3(uint(ray % uint64_t(rt_launch_size.x)),
                              uint((ray / uint64_t(rt_launch_size.x)) % uint64_t(rt_launch_size.y)), uint(ray / xy));
    }
#ifdef RT_PRIVATE_WORDS
    if (rt_p64(rt_ctx + 24ul) != uint64_t(RT_PRIVATE_WORDS) * 4ul || rt_p64(rt_ctx + 176ul) != rt_pipeline_id)
        rt_fail(VEL_RT_STATUS_CONTEXT);
    rt_arena = 16ul;
    rt_cursor = 16ul;
    rt_limit = uint64_t(RT_PRIVATE_WORDS) * 4ul;
#else
    uint64_t pool = rt_p64(rt_ctx + 8ul), capacity = rt_p64(rt_ctx + 16ul), stride = rt_p64(rt_ctx + 24ul), offset;
    if (pool == 0ul || (pool & 15ul) != 0ul || (stride & 15ul) != 0ul || !rt_product(localIndex, stride, offset) ||
        offset > capacity || stride > capacity - offset || offset > ~0ul - pool || stride > ~0ul - (pool + offset) ||
        rt_p64(rt_ctx + 176ul) != rt_pipeline_id)
        rt_fail(VEL_RT_STATUS_CONTEXT);
    rt_arena = pool + offset;
    rt_cursor = rt_arena;
    rt_limit = rt_arena + stride;
#endif
    if (rt_error == 0u) {
        uint stage;
        uint64_t record;
        if (rt_general_record(VEL_RT_RUN_RAYGEN_ADDRESS, 0u, 5313u, stage, record) && stage != 0xffffffffu)
            rt_push_shader(stage, record, 0ul, 0ul, 0u, 0u, 0ul, 0u, 0u, 0ul, 0u, 0u);
        while (rt_top != 0ul && rt_error == 0u) {
            uint64_t node = rt_top;
            if (rt_u(node) == 2u) {
                rt_trace_step(node);
                continue;
            }
            uint64_t c = rt_control(node);
            rt_step(rt_u(node + 4ul), rt_frame(node));
            uint pending = rt_u(c + 4ul);
            if (rt_u(c) == 0xffffffffu) {
                rt_shader_done(node, pending);
                continue;
            }
            if (pending == 1u)
                rt_begin_trace(node);
            else if (pending == 3u)
                rt_callable(node);
            else if (pending == 2u)
                rt_report(node);
            else
                rt_fail(VEL_RT_STATUS_REQUEST);
        }
    }
    if (status != 0ul) atomicExchange(RtWords(status).w[0], rt_error);
}
