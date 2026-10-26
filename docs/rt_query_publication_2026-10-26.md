<!-- Copyright 2026 The Khronos Group Inc. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Public AS query publication checkpoint

The canonical layer now has a bounded public AS query backend. This is progress toward the complete AS/RQ/RTP goal, not a conformance claim. It replaces the previous public query-pool creation failure on the tested compaction and serialization paths.

Each recorded query Reset/Write owns a mapped, coherent private payload and a native timestamp query. Compute reads the actual GPU-built AS header and writes the property. A separate publication dispatch updates GPU-owned query slots and assigns a sequence using a **per-query** device uint64 atomic counter. The host never polls those GPU buffers for availability.

Publication uses this dependency chain:

1. The property/publication compute stores finish before a shader-write to HOST_READ memory dependency, with COMPUTE and HOST destination stages.
2. A real `CmdDispatch(1,1,1)` follows that barrier. Its pushed count is zero, so its shader invocations exit without accessing memory. This compute action is in the barrier's destination scope and the following timestamp's source scope.
3. A native compute-stage timestamp follows the sentinel. The host polls its native availability, then calls native `GetQueryPoolResults` with WAIT to acquire an available marker before reading the immutable payload.
4. The host-read dependency transfers device writes to the host domain; HOST_COHERENT storage supplies host visibility. Timestamp numerical values never encode properties, sequences, or addresses.

The relevant normative mechanisms are the [timestamp execution scopes](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp2.html), [GetQueryPoolResults WAIT dependency](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html), and [device/host memory domains](https://docs.vulkan.org/spec/latest/appendices/memorymodel.html).

For a recording without SIMULTANEOUS_USE, a legal resubmit requires its previous execution to have completed. The submit preflight acquires and harvests its old payloads under the publication mutex **before** resetting private markers or permitting new GPU writes. Public old availability stays in CPU cache until the corresponding public GPU reset executes. CB reset, re-record, free, and command-pool reset harvest before releasing private storage. Cached public results outlive the producer CB. Submission rollback uses the address of a per-call stack token and only touches operations prepared by that call.

Per-query sequences track GPU execution order across queues independently of CPU submit order. Modular uint64 comparison handles wrap: each recorded operation has at most one unharvested execution, and every resubmit polls completed publications before reusing storage. Unrelated queries have separate counters.

GPU result copies format into private storage, followed by native TRANSFER copies of individual result regions. The destination needs only TRANSFER_DST usage, and stride gaps remain unchanged.

The formatter runs at COMPUTE, while the public query copy is a TRANSFER operation. A cross-queue semaphore wait at TRANSFER alone therefore cannot safely protect the private formatter. This backend explicitly rejects a query-copy submission when any of its waits, an earlier batch's waits, or the same queue's remembered prior waits omit COMPUTE, ALL_COMMANDS, or TOP_OF_PIPE after existing virtual-stage translation. It never widens those waits. The queue history is committed only after successful native submission and conservatively remains until a successful native QueueWaitIdle or DeviceWaitIdle. A completed fence alone does not clear this qualification history. A later batch with a narrower wait and no query copy does not reject an earlier query prefix. These are explicit support boundaries, not complete staged synchronization.

The backend privately enables hostQueryReset and shaderBufferInt64Atomics only when both are natively supported on a virtual RT device. Missing prerequisites do not reject ordinary device creation or the existing AS path. Query recording also requires a single-physical-device logical device and an unprotected compute command pool with timestamp support. Query-containing SIMULTANEOUS_USE recordings and operations under active native queries fail recording explicitly. Unsupported query-pool creation chains/flags remain rejected.

Complete simultaneous execution still needs fresh execution operands through replay or equivalent command segmentation. Logical AS/RTP/helper stages still share native compute; generic staged-wait scheduling and all native query interactions remain open work.

Validation evidence:

- All 22 offline tests pass, including publication acquisition/reuse/reset/wrap/rollback and private feature gating.
- Eight selected existing/new GPU tests pass, including RTP state restoration, AS copying/serialization, and the existing GPU property component.
- The public query fixture performs 64 legal resubmits with concurrent host reads, changes the GPU-built AS size, checks transfer-only result copies and padding, and checks cached results after CB reset/free and pool reset.
- Its two-batch timeline case reads first-batch queries before host-signaling a timeline that blocks the second batch in the same QueueSubmit. This checks prefix publication without relying on independent-stage progress or waiting for the whole-submit fence.
- Latest validation-above run: 7,354 reader iterations, zero errors. Validation-below: 31,831 iterations, zero errors. Logs are in `build-rq-memory/query-validation-{above,below}.txt`.
- External CTS compaction/serialization former failure pair passes 2/2; expanded public AS operations pass 24/24, matching the native control. The parent task retains the exact CTS case lists and logs.

- The new two-queue fixture delays a query producer on a host timeline. Its consumer waits at TRANSFER on the producer's timeline: native Vulkan copies fresh properties, while the bounded backend rejects before submitting private compute. A retry with ALL_COMMANDS succeeds. Separate-call and Submit2 history checks become usable after QueueWaitIdle/DeviceWaitIdle. Validation above/below and native control all report zero errors in `build-rq-memory/query-crossqueue-validation-{above,below,native}.txt`.
