// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#ifndef VEL_OMM_OPACITY_GLSL
#define VEL_OMM_OPACITY_GLSL

// Wire values match State, Decision, and the Vulkan/SPIR-V flag constants in
// omm_opacity.h: states 0/1/2/3, decisions Ignore=0/Opaque=1/NonOpaque=2.
// This evaluator has no resource bindings or traversal/backend dependencies.
// Precondition: the opacity ray flags 0x1/0x2/0x40/0x80 are mutually exclusive,
// instance flags 0x4/0x8 are mutually exclusive, and an active state is 0..3.
// hasAssociation identifies an OMM association, including special-only null micromaps.
uint ommResolveOpacity(uint state, bool hasAssociation, uint rayFlags, uint instanceFlags, uint geometryFlags) {
    bool opaque = (geometryFlags & 0x1u) != 0u;
    if (hasAssociation && (instanceFlags & 0x20u) == 0u) {
        bool forceTwoState = (rayFlags & 0x400u) != 0u || (instanceFlags & 0x10u) != 0u;
        if (state == 0u || (forceTwoState && state == 2u)) return 0u;
        opaque = state == 1u || (forceTwoState && state == 3u);
    }
    if ((instanceFlags & 0x4u) != 0u) opaque = true;
    if ((instanceFlags & 0x8u) != 0u) opaque = false;
    if ((rayFlags & 0x1u) != 0u) opaque = true;
    if ((rayFlags & 0x2u) != 0u) opaque = false;
    if ((opaque && (rayFlags & 0x40u) != 0u) || (!opaque && (rayFlags & 0x80u) != 0u)) return 0u;
    return opaque ? 1u : 2u;
}

#endif
