// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "omm_query_rewrite.h"

namespace vel::omm {
enum class AsProvenanceStatus { Success, InvalidModule, Unsupported, OutOfMemory };
struct AsProvenanceOptions {
    // Existing scalar by-value functions. resolve(uvec2 nativeAddress) -> uvec2
    // RootCell address; fetch(uint set, uint binding, uvec2 flattenedIndex) ->
    // uvec4 {nativeAddress.xy, RootCell.zw}. The descriptor provider must read
    // one coherent layer-owned shadow record, including its native address.
    uint32_t integer_resolver_function = 0;
    uint32_t descriptor_fetch_function = 0;
};
struct AsProvenanceModule {
    std::vector<uint32_t> words;
    // Offsets refer to words above, after all injected instructions.
    std::vector<QueryInitializationMetadata> initialization_metadata;
    // Counts original producers, excluding synthesized descriptor conversions.
    uint32_t integer_conversions = 0;
    uint32_t descriptor_loads = 0;
};

// Preserves native AS values and propagates a parallel uvec2 RootCell address.
// Descriptor loads are replaced by one fetch and UToAS of its native address;
// opaque acceleration structures are never converted to integers or inspected.
// Function arguments/returns carry provenance without repeating application
// calls. Descriptor pointer arguments carry numeric paths, never new pointers.
// Both source and output are Vulkan 1.2 validated. Unsupported/unknown producers
// fail closed. Output is unchanged on any failure, including allocation failure.
AsProvenanceStatus propagate_as_provenance(const std::vector<uint32_t>& source, const AsProvenanceOptions& options,
                                           AsProvenanceModule* output, std::string* diagnostic = nullptr);
}  // namespace vel::omm
