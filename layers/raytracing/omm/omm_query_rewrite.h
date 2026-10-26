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
    // Legacy preparation exposes a direct uint pointer. Typed consumers instead
    // expose state_pointer and leave flags_pointer zero.
    uint32_t flags_pointer;
    uint32_t state_pointer = 0;
};
struct QueryStateModule {
    std::vector<uint32_t> words;
    std::vector<QueryStateBinding> state;
    uint32_t initializations = 0;
    uint32_t flag_getters = 0;
    uint32_t proceeds = 0;
    uint32_t aabb_getters = 0;
    uint32_t typed_state_type = 0;
};

struct QueryInitializationMetadata {
    uint32_t initialize_word_offset;
    uint32_t metadata_value;
};
struct QueryConsumerOptions {
    // An existing, non-query function with scalar-value parameters:
    // uint classify(uvec2 scene, uint instance, uint geometry, uint primitive,
    //               vec2 barycentrics, bool triangle, uint logicalFlags).
    // Return 0=ignore, 1=opaque, 2=nonopaque. The callback supplies logical
    // geometry/instance metadata; native AS storage remains opaque.
    uint32_t classify_function = 0;
    // Source word offsets distinguish reinitializations of the same query.
    // Values must already be valid uvec2 SSA IDs that dominate the initialize.
    // Final Vulkan validation checks the resulting SSA and callback ABI.
    std::vector<QueryInitializationMetadata> initialization_metadata;
    bool use_constant_metadata = false;
    uint32_t constant_metadata_low = 0;
    uint32_t constant_metadata_high = 0;
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

// Experimental native consumer transform. Keeps a typed parallel query state
// {uint flags, uint candidateAabbOpaque, uvec2 sceneMetadata}, restores getters,
// and replaces each Proceed with an inline structured candidate-filtering loop.
// Query pointers never cross a new FunctionCall. Every initialize requires an
// explicit metadata binding or the explicitly enabled constant fixture context.
// This pass does not infer AS operand provenance or invent sidecar metadata.
// EXT capability/extension declarations are removed after flags are translated.
// KHR execution modes and mixed native pipeline OMM consumers fail closed until
// their independent consumer adapters can sanitize those paths as well.
QueryRewriteStatus rewrite_query_consumers(const std::vector<uint32_t>& source, const QueryConsumerOptions& options,
                                           QueryStateModule* output, std::string* diagnostic = nullptr);
}  // namespace vel::omm
