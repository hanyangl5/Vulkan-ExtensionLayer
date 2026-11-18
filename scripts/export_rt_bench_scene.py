#!/usr/bin/env python3
# Copyright 2026 The Khronos Group Inc.
# SPDX-License-Identifier: Apache-2.0
"""Flatten a glTF scene into the scene file read by test_rtp_bench.

The output is uint32 mesh_count, uint32 triangle_count[mesh_count], then
non-indexed float3 vertices (three per triangle). Each mesh becomes one BLAS
and one TLAS instance. Only the first primitive of each mesh is used, and node
transforms are ignored; the Vulkan-Samples Sponza layout (scale 0.01, axes
rotated as in ray_tracing_extended) is applied to every vertex.

Example (Vulkan-Samples Sponza, 262,241 triangles in 25 meshes):
  python scripts/export_rt_bench_scene.py \
    <Vulkan-Samples>/assets/scenes/sponza/Sponza01.gltf build/sponza_tris.bin
  test_rtp_bench rtp_bench_scene.rgen.spv rtp_bench.rmiss.spv \
    rtp_bench_scene.rchit.spv 320 180 4 3 0 build/sponza_tris.bin
"""

import json
import os
import struct
import sys

COMPONENTS = {"SCALAR": 1, "VEC3": 3}
FORMATS = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    source, output = sys.argv[1], sys.argv[2]
    with open(source, encoding="utf-8") as handle:
        gltf = json.load(handle)
    with open(os.path.join(os.path.dirname(source), gltf["buffers"][0]["uri"]), "rb") as handle:
        data = handle.read()
    accessors, views = gltf["accessors"], gltf["bufferViews"]

    def read(index):
        accessor = accessors[index]
        view = views[accessor["bufferView"]]
        offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        count = COMPONENTS[accessor["type"]]
        fmt = FORMATS[accessor["componentType"]]
        stride = view.get("byteStride", count * struct.calcsize(fmt))
        return [struct.unpack_from("<" + fmt * count, data, offset + i * stride) for i in range(accessor["count"])]

    vertices = bytearray()
    counts = []
    for mesh in gltf["meshes"]:
        primitive = mesh["primitives"][0]
        positions = read(primitive["attributes"]["POSITION"])
        indices = [value[0] for value in read(primitive["indices"])]
        counts.append(len(indices) // 3)
        for index in indices[: counts[-1] * 3]:
            x, y, z = positions[index]
            vertices += struct.pack("<3f", y * 0.01, z * 0.01, x * 0.01)
    with open(output, "wb") as handle:
        handle.write(struct.pack("<I", len(counts)))
        handle.write(struct.pack("<%dI" % len(counts), *counts))
        handle.write(vertices)
    print(f"{len(counts)} meshes, {sum(counts)} triangles -> {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
