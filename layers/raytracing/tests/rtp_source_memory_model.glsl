// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#ifdef VKRT_SOURCE_MEMORY_MODEL
#extension GL_KHR_memory_scope_semantics : require
#pragma use_vulkan_memory_model
void sourceRelease() {
    memoryBarrier(gl_ScopeShaderCallEXT, gl_StorageSemanticsBuffer,
                  gl_SemanticsRelease | gl_SemanticsMakeAvailable);
}
void sourceAcquire() {
    memoryBarrier(gl_ScopeShaderCallEXT, gl_StorageSemanticsBuffer,
                  gl_SemanticsAcquire | gl_SemanticsMakeVisible);
}
#else
// The GLSL450 control also publishes ordinary BDA memory across shader calls.
void sourceRelease() { memoryBarrierBuffer(); }
void sourceAcquire() { memoryBarrierBuffer(); }
#endif
