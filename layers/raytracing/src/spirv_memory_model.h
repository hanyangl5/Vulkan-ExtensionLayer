// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "spirv_environment.h"
#include <spirv/unified1/spirv.hpp>
#include <map>
#include <stdexcept>
#include <vector>

namespace vel::rt {
// A native compute invocation executes the complete logical RT call tree.
// Device scope includes every such tree and keeps acquire/release and explicit
// availability/visibility semantics. Invocation scope would require Relaxed
// semantics in Vulkan, and cannot replace these barriers. Rewrite parsed scope
// operands only: the same constant 6 can also be ordinary application data.
inline void RewriteTypedMemoryScopes(std::vector<uint32_t> &words, uint32_t from, uint32_t to) {
    struct Metadata {
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> constants;
        std::vector<size_t> scopes;
        size_t offset = 5;
        bool vulkan = false, device_scope = false;
    } metadata;
    auto parse = [](void *user, const spv_parsed_instruction_t *i) {
        auto &m = *static_cast<Metadata *>(user);
        if (i->opcode == spv::OpConstant && i->num_words == 4)
            m.constants.emplace(i->result_id, std::make_pair(i->type_id, i->words[3]));
        if (i->opcode == spv::OpMemoryModel) m.vulkan = i->words[2] == spv::MemoryModelVulkan;
        if (i->opcode == spv::OpCapability && i->words[1] == spv::CapabilityVulkanMemoryModelDeviceScope) m.device_scope = true;
        for (uint32_t n = 0; n < i->num_operands; ++n)
            if (i->operands[n].type == SPV_OPERAND_TYPE_SCOPE_ID) m.scopes.push_back(m.offset + i->operands[n].offset);
        m.offset += i->num_words;
        return SPV_SUCCESS;
    };
    auto context = spvContextCreate(UniversalEnvironmentForSPIRV(words.at(1)));
    const auto result = spvBinaryParse(context, &metadata, words.data(), words.size(), nullptr, parse, nullptr);
    spvContextDestroy(context);
    if (result != SPV_SUCCESS) throw std::runtime_error("typed memory scope parse failed");
    std::map<uint32_t, uint32_t> types;
    std::vector<uint32_t> constants;
    for (auto operand : metadata.scopes) {
        const auto c = metadata.constants.find(words[operand]);
        if (c == metadata.constants.end() || c->second.second != from) continue;
        auto [scope, inserted] = types.emplace(c->second.first, words[3]);
        if (inserted) {
            ++words[3];
            constants.insert(constants.end(), {(4u << 16) | spv::OpConstant, c->second.first, scope->second, to});
        }
        words[operand] = scope->second;
    }
    if (types.empty()) return;
    size_t declaration = 5;
    while (declaration < words.size() && (words[declaration] & 65535) != spv::OpFunction) declaration += words[declaration] >> 16;
    words.insert(words.begin() + declaration, constants.begin(), constants.end());
    if (to == spv::ScopeDevice && metadata.vulkan && !metadata.device_scope)
        words.insert(words.begin() + 5, {(2u << 16) | spv::OpCapability, spv::CapabilityVulkanMemoryModelDeviceScope});
}
inline void LowerTypedShaderCallScopes(std::vector<uint32_t> &words) {
    RewriteTypedMemoryScopes(words, spv::ScopeShaderCallKHR, spv::ScopeDevice);
}
}  // namespace vel::rt
