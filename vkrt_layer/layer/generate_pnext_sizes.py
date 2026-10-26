# SPDX-License-Identifier: MIT
"""Generate pNext metadata for structs present in vulkan_core.h."""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def main():
    registry, header, output = map(Path, sys.argv[1:])
    core = header.read_text(encoding="utf-8")
    declared = set(re.findall(r"typedef\s+struct\s+(Vk\w+)\s*\{", core))
    root = ET.parse(registry).getroot()
    platform_guards = {p.get("name"): p.get("protect") for p in root.findall("./platforms/platform")}
    guarded_types = {}
    for extension in root.findall("./extensions/extension[@platform]"):
        guard = platform_guards.get(extension.get("platform"))
        if guard and "vulkan" in extension.get("supported", "").split(","):
            for required in extension.findall("./require/type"):
                guarded_types.setdefault(required.get("name"), guard)
    platform_declared = set()
    for platform_header in header.parent.glob("vulkan_*.h"):
        platform_declared.update(re.findall(r"typedef\s+struct\s+(Vk\w+)\s*\{", platform_header.read_text(encoding="utf-8")))
    cases = {}
    buffer_prefix = {}
    for struct in root.findall("./types/type[@category='struct']"):
        name = struct.get("name")
        if "vulkan" not in struct.get("api", "vulkan").split(","):
            continue
        for member in struct.findall("member"):
            if member.findtext("name") == "sType" and member.get("values"):
                if name in declared:
                    cases.setdefault(member.get("values"), name)
                if ("VkBufferCreateInfo" in struct.get("structextends", "").split(",")
                        and name not in {"VkBufferUsageFlags2CreateInfo", "VkBufferUsageFlags2CreateInfoKHR"}
                        and (name in declared or (name in platform_declared and name in guarded_types))):
                    buffer_prefix.setdefault(member.get("values"), (name, None if name in declared else guarded_types[name]))
    if "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2" not in cases:
        raise ValueError("Vulkan registry/header mismatch: missing Features2 metadata")
    output.write_text(
        "/* Generated from Vulkan registry metadata; do not edit. */\n"
        + "".join(f"case {stype}: return sizeof({name});\n" for stype, name in sorted(cases.items())),
        encoding="utf-8",
    )
    if not buffer_prefix:
        raise ValueError("Vulkan registry/header mismatch: no buffer-chain metadata")
    # Valid BufferCreateInfo chains contain each sType at most once. A union
    # slot for every declared legal prefix type therefore bounds every valid
    # chain, with compiler-provided size/alignment rather than a guessed arena.
    # Platform nodes use the registry's own platform guards and Vulkan umbrella
    # header, so the bound matches the selected build on every platform.
    def guarded(text, guard):
        return f"#ifdef {guard}\n{text}#endif\n" if guard else text

    output.with_name("vkrt_buffer_chain_storage.h").write_text(
        "/* Generated from Vulkan registry/header metadata; do not edit. */\n"
        "#ifndef VKRT_BUFFER_CHAIN_STORAGE_H\n#define VKRT_BUFFER_CHAIN_STORAGE_H\n"
        + "".join(guarded("#include <vulkan/vulkan.h>\n", guard) for guard in sorted({g for _, g in buffer_prefix.values() if g}))
        + "union vkrt_buffer_chain_node {\n"
        + "".join(guarded(f"   {name} value_{name};\n", guard) for name, guard in sorted(buffer_prefix.values()))
        + "};\n"
        + "enum { VKRT_BUFFER_CHAIN_NODE_COUNT = 0\n"
        + "".join(guarded("   + 1\n", guard) for _, guard in buffer_prefix.values())
        + "};\n"
        + "static inline size_t vkrt_buffer_chain_prefix_size(VkStructureType type) {\n"
          "   switch (type) {\n"
        + "".join(guarded(f"   case {stype}: return sizeof({name});\n", guard) for stype, (name, guard) in sorted(buffer_prefix.items()))
        + "   default: return 0;\n   }\n}\n#endif\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
