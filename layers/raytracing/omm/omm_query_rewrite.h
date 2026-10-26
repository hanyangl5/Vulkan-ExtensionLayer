// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vel::omm {
enum class QueryRewriteStatus { Success, InvalidModule, Unsupported, OutOfMemory };
struct QueryStateBinding {
    uint32_t query_pointer;
    uint32_t flags_pointer;
};
struct QueryStateModule {
    std::vector<uint32_t> words;
    std::vector<QueryStateBinding> state;
    uint32_t initializations = 0;
    uint32_t flag_getters = 0;
};

// Internal compiler preparation pass, not a complete OMM shader transform.
// Preserves native query objects and shadows their Private/Function pointers,
// fixed arrays/structs, valid aliases and function parameters with uint storage.
// Capture-only mode preserves traversal behavior. Expose-candidates mode must be
// followed by candidate wrapping before execution: it deliberately requests
// NoOpaque and defers logical opacity/culling to that subsequent pass.
// Input/output validation targets Vulkan 1.2. Output is unchanged on failure.
QueryRewriteStatus prepare_query_state(const std::vector<uint32_t>& source, bool expose_candidates, QueryStateModule* output,
                                       std::string* diagnostic = nullptr);
}  // namespace vel::omm
