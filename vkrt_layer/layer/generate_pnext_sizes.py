# SPDX-License-Identifier: MIT
"""Generate a pNext size switch for structs present in vulkan_core.h."""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def main():
    registry, header, output = map(Path, sys.argv[1:])
    core = header.read_text(encoding="utf-8")
    declared = set(re.findall(r"typedef\s+struct\s+(Vk\w+)\s*\{", core))
    cases = {}
    for struct in ET.parse(registry).findall("./types/type[@category='struct']"):
        name = struct.get("name")
        if name not in declared or "vulkan" not in struct.get("api", "vulkan").split(","):
            continue
        for member in struct.findall("member"):
            if member.findtext("name") == "sType" and member.get("values"):
                cases.setdefault(member.get("values"), name)
    if "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2" not in cases:
        raise ValueError("Vulkan registry/header mismatch: missing Features2 metadata")
    output.write_text(
        "/* Generated from Vulkan registry metadata; do not edit. */\n"
        + "".join(f"case {stype}: return sizeof({name});\n" for stype, name in sorted(cases.items())),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
