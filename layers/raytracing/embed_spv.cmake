# SPDX-License-Identifier: MIT
file(READ "${INPUT}" _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])" "0x\\4\\3\\2\\1u," _words "${_hex}")
file(WRITE "${OUTPUT}" "/* Generated SPIR-V. */\nstatic const uint32_t vkrt_spv_${NAME}[] = {${_words}};\n")
