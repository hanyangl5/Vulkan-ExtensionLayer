// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#define main omm_gpu_build_metadata_fixture_main
#include "test_omm_gpu_build.cpp"
#undef main
#include "../omm_metadata.h"

namespace {
unsigned assertions = 0;
void require_metadata(bool value, const char* message) {
    ++assertions;
    if (!value) {
        std::fprintf(stderr, "FAIL metadata GPU: %s\n", message);
        std::exit(1);
    }
}
std::vector<uint8_t> canonical_reference(uint32_t count) {
    constexpr uint32_t stride = 13, seed = 1, data_size = 1041;
    std::vector<uint8_t> records(count ? size_t(count - 1) * stride + 8 : 0, 0xcd), data(data_size), result;
    for (uint32_t i = 0; i < data_size; ++i) data[i] = uint8_t((i * 37u + seed * 19u + 0xa7u) ^ (i >> 3));
    for (uint32_t i = 0; i < count; ++i) {
        const size_t record = i * stride;
        put32(records, record, (i % 3) * 3 + 7);
        records[record + 4] = uint8_t((i + seed) % 7);
        records[record + 5] = 0;
        records[record + 6] = uint8_t(1 + ((i ^ seed) & 1));
        records[record + 7] = 0;
    }
    require_metadata(
        storage_build({records.data(), records.size(), count, stride, data.data(), data.size()}, &result) == StorageResult::Success,
        "independent canonical CPU oracle");
    return result;
}
void gpu_canonical(Fixture& fixture, Program& build, Program& generate, const Buffer& destination, uint32_t count,
                   uint64_t active_size) {
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const uint64_t logical = uint64_t(count - 1) * 13 + 8, accessible = align4(logical);
    auto triangles = fixture.buffer(accessible, usage, false), data = fixture.buffer(1044, usage, false);
    auto status = fixture.buffer(4, usage, false);
    GeneratePush generator{triangles.address, data.address, count, 13, 1, accessible, 1044, 6};
    BuildPush push{triangles.address, data.address, destination.address,
                   status.address,    logical,      1041,
                   active_size,       count,        13,
                   accessible,        1044,         0};
    auto command = fixture.begin();
    generate.dispatch(command, &generator, sizeof(generator), 128);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    build.dispatch(command, &push, sizeof(push));
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    push.phase = 1;
    build.dispatch(command, &push, sizeof(push), 128);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(command, triangles.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeee);
    vkCmdFillBuffer(command, data.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeee);
    fixture.submit(command);
    release_buffer(fixture, triangles);
    release_buffer(fixture, data);
    release_buffer(fixture, status);
}
void submit_metadata(Fixture& fixture, VkCommandBuffer command) {
    auto create = init<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    VkFence fence{};
    CHECK(vkCreateFence(fixture.device, &create, nullptr, &fence));
    auto submit = init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    CHECK(vkQueueSubmit(fixture.queue, 1, &submit, fence));
    CHECK(vkWaitForFences(fixture.device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(fixture.device, fence, nullptr);
}
void put16(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
}

enum class BadJob { Carry, NullOrdinary, Width, IndexBounds, AssociationOffset, StatusJobs, StatusIndices, StatusSnapshot };
void invalid_job(Fixture& fixture, Program& metadata, const std::vector<MetadataGeometrySource>& original_sources,
                 const std::vector<uint8_t>& original_indices, const Buffer& input_indices, const Buffer& snapshot,
                 uint64_t body_size, BadJob kind) {
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto sources = original_sources;
    auto indices = original_indices;
    auto jobs = fixture.buffer(sources.size() * sizeof(MetadataGeometrySource), usage, false);
    auto output = fixture.buffer(body_size + 64, usage, true), status = fixture.buffer(4, usage, true);
    MetadataBuildPush push{jobs.address, output.address, body_size + 64, status.address, sources.size(), 88, 0, 0, 0};
    VkBuffer status_source = status.handle;
    bool static_rejection = true;
    const char* name = "";
    switch (kind) {
        case BadJob::Carry:
            sources[5].base_triangle = UINT32_MAX;
            static_rejection = false;
            name = "u32+base keeps u64 carry";
            break;
        case BadJob::NullOrdinary:
            put16(indices, 48, 0);
            static_rejection = false;
            name = "null snapshot ordinary index";
            break;
        case BadJob::Width:
            sources[1].index_width = 8;
            name = "EXT unsupported index width";
            break;
        case BadJob::IndexBounds:
            sources[5].indices_bytes = 19;
            name = "index range bounds";
            break;
        case BadJob::AssociationOffset:
            sources[1].geometry.association_offset = 0;
            name = "association offset bounds";
            break;
        case BadJob::StatusJobs:
            push.status_address = jobs.address;
            status_source = jobs.handle;
            name = "status aliases jobs";
            break;
        case BadJob::StatusIndices:
            push.status_address = input_indices.address;
            status_source = input_indices.handle;
            name = "status aliases indices";
            break;
        case BadJob::StatusSnapshot:
            push.status_address = snapshot.address;
            status_source = snapshot.handle;
            name = "status aliases snapshot";
            break;
    }
    auto command = fixture.begin();
    vkCmdFillBuffer(command, output.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5);
    vkCmdFillBuffer(command, status.handle, 0, VK_WHOLE_SIZE, UINT32_MAX);
    vkCmdUpdateBuffer(command, jobs.handle, 0, sources.size() * sizeof(MetadataGeometrySource), sources.data());
    vkCmdUpdateBuffer(command, input_indices.handle, 0, indices.size(), indices.data());
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    metadata.dispatch(command, &push, sizeof(push));
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    push.phase = 1;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, metadata.pipeline);
    vkCmdPushConstants(command, metadata.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command, 128, uint32_t(sources.size()), 1);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    if (status_source != status.handle) {
        VkBufferCopy copy{0, 0, 4};
        vkCmdCopyBuffer(command, status_source, status.handle, 1, &copy);
    }
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_HOST_READ_BIT);
    fixture.submit(command);
    require_metadata(*static_cast<uint32_t*>(status.mapped) == 1, name);
    const auto* bytes = static_cast<const uint8_t*>(output.mapped);
    require_metadata(std::all_of(bytes + body_size, bytes + body_size + 64, [](uint8_t value) { return value == 0xa5; }),
                     "invalid GPU job preserves output allocation tail");
    if (static_rejection) {
        require_metadata(std::all_of(bytes, bytes + body_size, [](uint8_t value) { return value == 0xa5; }),
                         "phase-0 invalid job leaves all private body canaries intact");
    } else {
        require_metadata(metadata_validate_body(bytes, body_size) != MetadataResult::Success,
                         "phase-1 invalid semantic body is rejected and must not be published");
    }
    std::printf("PASS metadata GPU rejection: %s\n", name);
    release_buffer(fixture, jobs);
    release_buffer(fixture, output);
    release_buffer(fixture, status);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: test_omm_metadata_gpu metadata.spv build.spv generator.spv\n");
        return 1;
    }
    Fixture fixture;
    if (!fixture.open()) return 77;
    Program metadata(fixture, argv[1], sizeof(MetadataBuildPush)), build(fixture, argv[2], sizeof(BuildPush)),
        generate(fixture, argv[3], sizeof(GeneratePush));
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto canonical17 = canonical_reference(17), canonical8 = canonical_reference(8);
    const uint64_t source_capacity = canonical17.size() + 256;
    auto source = fixture.buffer(source_capacity, usage, false);
    gpu_canonical(fixture, build, generate, source, 17, canonical17.size());
    std::vector<uint8_t> snapshot(source_capacity, 0xa5), indices(256, 0xcd);
    std::memcpy(snapshot.data(), canonical17.data(), canonical17.size());
    put16(indices, 2, 0);
    put16(indices, 6, 1);
    put16(indices, 10, 0xfffe);
    put16(indices, 48, 0xfffd);
    for (uint32_t i = 0; i < 4; ++i) put16(indices, 64 + i * 2, 0xffff - i);
    put32(indices, 80, 1);
    put32(indices, 88, 0xfffffffe);
    put32(indices, 96, 3);
    MetadataGeometryInput inputs[] = {
        {{kMetadataTriangles, 1, 3, false, 0}, nullptr, 0, 0, 0, 0, nullptr, 0},
        {{kMetadataTriangles, 2, 3, true, source_capacity}, indices.data() + 2, 14, 4, 16, 1, snapshot.data(), snapshot.size()},
        {{kMetadataTriangles, 1, 4, true, 0}, indices.data() + 48, 2, 0, 16, UINT32_MAX, nullptr, 0},
        {{kMetadataAabbs, 1, 2, false, 0}, nullptr, 0, 0, 0, 0, nullptr, 0},
        {{kMetadataTriangles, 0, 3, true, source_capacity}, nullptr, 0, 0, 0, 3, snapshot.data(), snapshot.size()},
        {{kMetadataTriangles, 0, 3, true, source_capacity}, indices.data() + 80, 20, 8, 32, 2, snapshot.data(), snapshot.size()},
        {{kMetadataTriangles, 0, 4, true, 0}, indices.data() + 64, 8, 2, 16, UINT32_MAX, nullptr, 0},
    };
    constexpr size_t geometry_count = sizeof(inputs) / sizeof(inputs[0]);
    MetadataGeometryPlan plans[geometry_count];
    MetadataGeometry geometries[geometry_count];
    for (size_t i = 0; i < geometry_count; ++i) plans[i] = inputs[i].plan;
    uint64_t body_size;
    require_metadata(metadata_plan_blas(plans, geometry_count, geometries, geometry_count, &body_size) == MetadataResult::Success,
                     "CPU bounds/offset planner only");
    std::vector<uint8_t> expected;
    require_metadata(metadata_pack_blas(inputs, geometry_count, 0x1234, 77, &expected) == MetadataResult::Success,
                     "independent metadata oracle");
    auto gpu_indices = fixture.buffer(indices.size(), usage, false);
    std::vector<MetadataGeometrySource> sources(geometry_count);
    for (size_t i = 0; i < geometry_count; ++i) {
        auto& job = sources[i];
        job.geometry = geometries[i];
        if (inputs[i].plan.snapshot_capacity) {
            job.snapshot_address = source.address;
            job.source_snapshot_capacity = source_capacity;
        }
        if (inputs[i].indices) {
            job.indices_address = gpu_indices.address;
            job.indices_accessible_bytes = indices.size();
            job.indices_offset = uint64_t(inputs[i].indices - indices.data());
            job.indices_bytes = inputs[i].indices_size;
        }
        job.index_stride = inputs[i].index_stride;
        job.index_width = inputs[i].index_width;
        job.base_triangle = inputs[i].base_triangle;
    }
    auto gpu_jobs = fixture.buffer(sources.size() * sizeof(MetadataGeometrySource), usage, false);
    auto output = fixture.buffer(body_size + 64, usage, true), cloned = fixture.buffer(body_size + 64, usage, true);
    auto status = fixture.buffer(4, usage, true);
    MetadataBuildPush push{gpu_jobs.address, output.address, body_size + 64, status.address, geometry_count, 77, 0, 0x1234, 0};
    // Record once, then replay after rebuilding the source micromap with a
    // different GPU active size. No host source header or decisions are read.
    auto command = fixture.begin();
    vkCmdFillBuffer(command, output.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5);
    vkCmdFillBuffer(command, cloned.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5);
    vkCmdFillBuffer(command, status.handle, 0, VK_WHOLE_SIZE, UINT32_MAX);
    vkCmdUpdateBuffer(command, gpu_jobs.handle, 0, sources.size() * sizeof(MetadataGeometrySource), sources.data());
    vkCmdUpdateBuffer(command, gpu_indices.handle, 0, indices.size(), indices.data());
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    metadata.dispatch(command, &push, sizeof(push));
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    push.phase = 1;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, metadata.pipeline);
    vkCmdPushConstants(command, metadata.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command, 128, uint32_t(geometry_count), 1);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferCopy copy{0, 0, body_size};
    vkCmdCopyBuffer(command, output.handle, cloned.handle, 1, &copy);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(command, gpu_indices.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeee);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_HOST_READ_BIT);
    CHECK(vkEndCommandBuffer(command));
    submit_metadata(fixture, command);
    require_metadata(*static_cast<uint32_t*>(status.mapped) == 0, "GPU normalized association/snapshot succeeds");
    require_metadata(!std::memcmp(output.mapped, expected.data(), expected.size()) &&
                         !std::memcmp(cloned.mapped, expected.data(), expected.size()),
                     "all GPU-produced bytes match CPU oracle");
    gpu_canonical(fixture, build, generate, source, 8, canonical8.size());
    std::fill(snapshot.begin(), snapshot.end(), 0xa5);
    std::memcpy(snapshot.data(), canonical8.data(), canonical8.size());
    require_metadata(metadata_pack_blas(inputs, geometry_count, 0x1234, 77, &expected) == MetadataResult::Success,
                     "new independent snapshot active-size oracle");
    submit_metadata(fixture, command);
    require_metadata(*static_cast<uint32_t*>(status.mapped) == 0, "same producer CB replays after source active size changes");
    for (const auto bad : {BadJob::Carry, BadJob::NullOrdinary, BadJob::Width, BadJob::IndexBounds, BadJob::AssociationOffset,
                           BadJob::StatusJobs, BadJob::StatusIndices, BadJob::StatusSnapshot})
        invalid_job(fixture, metadata, sources, indices, gpu_indices, source, body_size, bad);
    vkFreeCommandBuffers(fixture.device, fixture.pool, 1, &command);
    release_buffer(fixture, gpu_indices);
    release_buffer(fixture, gpu_jobs);
    release_buffer(fixture, source);
    require_metadata(!std::memcmp(output.mapped, expected.data(), expected.size()) &&
                         !std::memcmp(cloned.mapped, expected.data(), expected.size()),
                     "replayed body/clone self contained after input destruction");
    require_metadata(metadata_validate_body(static_cast<const uint8_t*>(output.mapped), body_size) == MetadataResult::Success &&
                         metadata_validate_body(static_cast<const uint8_t*>(cloned.mapped), body_size) == MetadataResult::Success,
                     "GPU bodies validate after all association/micromap input lifetimes end");
    require_metadata(
        std::all_of(static_cast<const uint8_t*>(output.mapped) + body_size,
                    static_cast<const uint8_t*>(output.mapped) + body_size + 64, [](uint8_t byte) { return byte == 0xa5; }),
        "larger known allocation does not change body format capacity or canaries");
    std::printf("Verified canonical source active bytes: %zu -> %zu (CPU oracle, byte-exact GPU inline snapshots)\n",
                canonical17.size(), canonical8.size());
    std::printf("OMM metadata GPU: %u assertions, %zu geometries, %llu bytes, replay/snapshot lifetime/8 invalid cases PASS\n",
                assertions, geometry_count, static_cast<unsigned long long>(body_size));
    return 0;
}
