<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Pinned reference sources

These are unmodified references for the complete D3D12 fallback port. They are
**not compiled into the Vulkan layer**. Importing them is not feature completion.

- DirectX-Graphics-Samples: `e5975f9b0744fc5096dd593ed72bf8f5c004164f`.
  D3D12 fallback runtime, shaders, host passes, tests, public header and developer
  guide. Files retain the Microsoft MIT license in `d3d12/LICENSE`.
- DirectXShaderCompiler: `61e9771fc34bc914565dfc95a11f731f853e8ac9`.
  `lib/DxrFallback`, its public compiler header and license. These LLVM passes
  and the generated runtime bitcode are subject to `dxc/LICENSE.TXT`.

The commits independently pin the inspected source; they do not establish a
known-working DXC/D3D12 binary combination. DXC's LLVM dependency tree is not
vendored. Windows project/package settings are omitted from the runtime source
import. `manifest.json` records the exact selected paths and Git blob IDs.

```sh
python3 layers/raytracing/docs/upstream/verify.py
```

Do not reformat or edit these references. Implement Vulkan adaptations outside
this directory. Preserve attribution on derived files, and update pins only in
an explicit source-update commit. The developer guide's documented limitations
are part of the baseline; callable shaders, true compact copies, and other
unsupported upstream features must not be claimed merely because the Vulkan
API has commands for them.

See `docs/ray_tracing_fallback_port.md` for the port contract and actual status.
