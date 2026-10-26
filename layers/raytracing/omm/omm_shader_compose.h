// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vel::omm {
enum class ShaderCompositionStatus { Success, InvalidSource, InvalidLibrary, Unsupported, LinkFailed, OutOfMemory };
struct ComposedQueryShader {
    std::vector<uint32_t> words;
    uint32_t classifier_function = 0;
};

// Inject a resource-independent classifier library before AS provenance and
// rewrite_query_consumers. The library is an ordinary Vulkan 1.2 compute module
// with a dummy entry point, no global variables or specialization IDs, and a GLSL pointer-parameter
// function returning uint and accepting uvec2,uint,uint,uint,vec2,bool,uint.
// This adds a by-value thunk, removes the dummy entry declaration, and links
// types/IDs. Its resulting classifier ID and instruction offsets are NEW IDs;
// callers must construct per-initialize provenance AFTER composition.
//
// Internal memory access uses BDA/Int64 without descriptors or push constants.
// The caller must enable those features. PhysicalStorageBuffer64 addressing is
// introduced while retaining the application's GLSL450/Vulkan memory model.
// Both inputs and the result are validated for Vulkan 1.2; all temporary Linkage
// declarations are removed before returning. Output is unchanged on failure.
ShaderCompositionStatus compose_query_classifier(const std::vector<uint32_t>& source,
                                                 const std::vector<uint32_t>& classifier_library,
                                                 uint32_t library_classifier_function, ComposedQueryShader* output,
                                                 std::string* diagnostic = nullptr);
}  // namespace vel::omm
