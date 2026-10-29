# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
set(source "// Generated from validated SPIR-V.\n#pragma once\n#include <cstdint>\n")
foreach(kernel build copy metadata instances publish classifier property serialize)
    file(READ "${INPUT_${kernel}}" bytes HEX)
    # Convert little-endian words in memory instead of opening the generated
    # header once per SPIR-V word (particularly costly on Windows).
    string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1,\n" words "${bytes}")
    string(APPEND source "inline constexpr uint32_t vel_omm_${kernel}_spv[] = {\n${words}};\n")
endforeach()
file(WRITE "${OUTPUT}" "${source}")
