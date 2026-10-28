/* SPDX-License-Identifier: MIT */
#ifndef VKRT_SPIRV_IDS_H
#define VKRT_SPIRV_IDS_H
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkrt_spv {
bool remap_id_uses(std::vector<uint32_t> *words,
   const std::unordered_map<uint32_t, uint32_t> &substitutions, std::string *error);
bool dedupe_types(std::vector<uint32_t> *words, std::string *error);
}
#endif
