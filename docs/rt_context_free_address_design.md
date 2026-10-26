<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Candidate context-free AS address transport

This is a future proof/work item, not an implemented ABI or qualification claim.
The current resolver uses a device-owned radix tree and shader-object RQ lowering
specializes its context BDA. Recompiling exported RQ sources for another logical
device therefore needs original interfaces, linked peers and compiler environment.
Unrelated peer/interface features can make that reconstruction invalid even when
the requested binary itself is usable on the destination. Returning unsupported
does not complete the binary compatibility or resource-transparency goal.

An alternative worth implementing and measuring is a dense physical-root-cell
range for each public AS-storage buffer. It could remove the resolver-context
literal from ordinary RQ shaders and make their native binary transport independent
of the destination logical device. Regular buffer device addresses remain native.

## Arithmetic candidate

For backing-buffer size `S`, reserve `N = (S >> 8) + 1` eight-byte root cells.
The extra cell includes a legal zero-sized AS at `offset == S` when S is aligned.
With eight-aligned cell base `C` and a legal AS offset `O = 256*k`, define:

```
public value = (C << 5) + O
cell address = public value >> 5 = C + 8*k
physical AS root = load64(cell address)
```

This preserves 256-byte public alignment, same-buffer address differences and
same-offset GENERIC aliases. It uses `8*((S>>8)+1)` bytes rather than a 256-byte
physical cell for every slot. All multiplication, shift, address-span and addition
operations must be checked. In unsigned mode the entire cell span must fit the
59-bit encodable BDA range; checking only the first cell is insufficient. A signed
canonical encoding would require an explicit, separate sign-extension definition.
Neither mode can silently truncate arbitrary native 64-bit addresses.

## Required proofs before adoption

- Prove native allocation/virtual-address eligibility, alignment and complete span
  encoding, including large buffers and the last zero-size slot. No artificial
  four-GiB resource ceiling is acceptable.
- Measure the dense backing cost for large AS buffers with few AS views; compare
  it with the current sparse resolver. Plan coherent/private allocations and pool
  them without consuming advertised mandatory application budgets.
- Define the descriptor route as well as direct integer values. Current AS
  descriptors read a cell containing the public value, then resolve that value;
  an eight-byte physical-root cell cannot replace that descriptor cell directly.
  A paired value/root cell would need a corresponding encoding proof, and
  per-cell SSBO offsets must obey native storage-buffer alignment. Separate
  descriptor cells or alias buffers retain their own allocation/object costs.
- Preserve references through AS aliases, AS destruction, underlying-buffer
  destruction and native handle reuse. Cells cannot be reclaimed while a legal
  consumer can read them. First publication and updates require Vulkan memory
  dependencies rather than CPU atomics alone.
- Define zero/unknown/stale values within Vulkan's valid-usage contract. A direct
  dereference has different failure behavior from the current safe radix miss.
- Qualify sparse-binding buffers, queues, noncoherent memory where supported,
  serialization relocation and both RQ/RTP consumers of the same AS objects.
- Change the resolver/serialization/binary compatibility identities together;
  keep source/runtime layouts versioned and preserve all historical evidence.
- Demonstrate destination-independent RQ binaries with actual external CTS plus
  execution on different devices/profiles, including immutable interfaces and
  linked peers whose source features are absent on the destination.

Until these proofs pass, the existing shared AS representation and resolver remain
active. This candidate does not replace any M0–M5 exit criterion or justify a
complete support claim.
