// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "continuations.h"

namespace vel::rt {
struct ContinuationTypeCodec {
    uint32_t type;
    // SPIR-V assembly IDs in the returned component. They do not reuse input
    // module IDs, so the component can be included in a compute state module.
    std::string spirv_type, load, store;
};
struct ContinuationSuspensionCodec {
    uint32_t function, word_offset;
    // Parameters/results are in this order. Function storage is already in
    // persistent backing and is deliberately absent. The suspended operation's
    // result is also absent: the scheduler must supply it after completion.
    std::vector<uint32_t> saved_values;
    std::string spill, restore, restored_type;
};
struct ContinuationFrameCodecs {
    ContinuationFrames plan;
    // Append these sections in SPIR-V logical order. The generated component
    // owns %<prefix>memory, a uint StorageBuffer runtime array at set/binding.
    // It contains no entry point, RT capability, source names, or source code.
    std::string capabilities, annotations, declarations, functions;
    std::string word_type, bool_type, void_type, memory, word_pointer_type;
    std::vector<ContinuationTypeCodec> types;
    std::vector<ContinuationSuspensionCodec> suspensions;
};

// Emit typed frame load/store functions from the validated frame plan. Each
// function takes (uint base_word, uint byte_offset [, typed value]). Offsets are
// relative to the invocation frame, whose base/stride must be word-aligned;
// each typed offset must satisfy the corresponding planned type alignment.
// Scalars are encoded little-endian; bool is normalized to 0/1; aggregates are
// recursively extracted/constructed using the compiler-owned layout. Narrow
// stores preserve neighboring bytes. One invocation must own each frame word.
//
// The state emitter can relocate Function storage to its slot for the entire
// invocation and rematerialize an alias as a byte offset into that same slot.
// A store is emitted only at an actual source write: this component never
// speculatively reads uninitialized locals. It deliberately does not lower the
// source CFG, pointer recipes, calls, RT operations, or suspension return ABI.
// Each planned suspension also gets a spill(base_word, saved SSA values...)
// and restore(base_word)->typed struct function; empty restore sets return void.
// It is an executable backend component, not a complete RT shader compiler.
ContinuationFrameCodecs EmitContinuationFrameCodecs(const std::vector<uint32_t> &spirv, const std::string &entry,
                                                    uint32_t execution_model, uint32_t descriptor_set, uint32_t binding,
                                                    const std::string &prefix = "vel_cf_",
                                                    const std::vector<uint32_t> &additional_value_types = {});
}  // namespace vel::rt
