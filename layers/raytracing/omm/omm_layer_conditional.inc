// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Included inside vel::omm::layer after Device/Command and fail_command.

bool internal_compute_ready(Device& dev, VkCommandBuffer cb) {
    if (!dev.software) return true;
    auto entry = command(dev, cb);
    if (!entry) {
        fail_command(dev, cb, VK_ERROR_INITIALIZATION_FAILED);
        return false;
    }
    if (dev.error.load() != VK_SUCCESS || entry->state.status() != VK_SUCCESS) return false;
    if (entry->conditional_rendering_active || entry->inherited_conditional_rendering) {
        // Public MM/AS operations are unconditional, whereas injected dispatches
        // obey conditional rendering. An inherited primary scope cannot be
        // suspended by a secondary command buffer to make those dispatches run.
        fail_command(dev, cb, VK_ERROR_FEATURE_NOT_PRESENT);
        return false;
    }
    return true;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginConditionalRenderingEXT(VkCommandBuffer cb, const VkConditionalRenderingBeginInfoEXT* info) {
    auto dev = device(cb);
    auto call = native<PFN_vkCmdBeginConditionalRenderingEXT>(*dev, "vkCmdBeginConditionalRenderingEXT");
    if (!dev->software) {
        call(cb, info);
        return;
    }
    auto entry = command(*dev, cb);
    if (!entry) {
        fail_command(*dev, cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    if (dev->error.load() != VK_SUCCESS || entry->state.status() != VK_SUCCESS) return;
    call(cb, info);
    entry->conditional_rendering_active = true;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginConditionalRendering2EXT(VkCommandBuffer cb, const VkConditionalRenderingBeginInfo2EXT* info) {
    auto dev = device(cb);
    auto call = native<PFN_vkCmdBeginConditionalRendering2EXT>(*dev, "vkCmdBeginConditionalRendering2EXT");
    if (!dev->software) {
        call(cb, info);
        return;
    }
    auto entry = command(*dev, cb);
    if (!entry) {
        fail_command(*dev, cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    if (dev->error.load() != VK_SUCCESS || entry->state.status() != VK_SUCCESS) return;
    call(cb, info);
    entry->conditional_rendering_active = true;
}

VKAPI_ATTR void VKAPI_CALL CmdEndConditionalRenderingEXT(VkCommandBuffer cb) {
    auto dev = device(cb);
    auto call = native<PFN_vkCmdEndConditionalRenderingEXT>(*dev, "vkCmdEndConditionalRenderingEXT");
    if (!dev->software) {
        call(cb);
        return;
    }
    auto entry = command(*dev, cb);
    if (!entry) {
        fail_command(*dev, cb, VK_ERROR_INITIALIZATION_FAILED);
        return;
    }
    // A rejected internal operation leaves the application's explicit scope
    // active natively. End must still balance that scope before native EndCB;
    // the sticky recording error continues to reject the completed CB.
    // Conversely, Begin may have been skipped because recording already failed.
    // Such an End must not introduce an unmatched native scope terminator.
    if (!entry->conditional_rendering_active && (dev->error.load() != VK_SUCCESS || entry->state.status() != VK_SUCCESS)) return;
    call(cb);
    // Never clear inherited_conditional_rendering: this command can only end an
    // explicit scope owned by this command buffer, not its primary's predicate.
    entry->conditional_rendering_active = false;
}
