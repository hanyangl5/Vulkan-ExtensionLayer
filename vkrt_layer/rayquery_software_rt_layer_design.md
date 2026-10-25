# Vulkan Ray Query 软件实现 Layer 设计文档

**版本：v0.2｜范围：仅 `VK_KHR_ray_query`，不支持 `VK_KHR_ray_tracing_pipeline`**

## 1. 目标与结论

目标是在 Vulkan layer 中替代驱动的 acceleration-structure 构建和 ray-query 遍历，使应用能够在**没有硬件 RT 单元**的 GPU 上使用 Ray Query 语义。

最终执行的是 GPU 软件实现：自定义 BVH 存在普通 buffer 中；AABB、三角形相交和 BVH traversal 由普通 SPIR-V ALU/load/store 指令完成。

最关键的结论是：

> 软件 traversal **不能**从当前 shader 动态启动一个新的 traversal shader、等待其返回后再继续。它必须被注入为当前 Ray Query 所在 shader module 内的普通函数，最终成为同一 invocation 的代码。

因此有两类不同的 layer 工作：

```text
AS build API                     Ray Query shader
vkCmdBuildAcceleration...        OpRayQueryProceedKHR
      |                                      |
      v                                      v
插入独立的 compute build pass       改写为同 module 的函数调用
      |                                      |
custom BVH buffer               sw_ray_query_proceed(state)
```

## 2. 范围

### v1 支持

- `VK_KHR_acceleration_structure` 的 BLAS/TLAS 对象和 build API；
- `VK_KHR_ray_query` 在 compute shader 中的使用；
- 三角形 BLAS、TLAS instance、mask、变换；
- GPU LBVH builder，输出自定义 BVH4；
- Ray Query 的 `Initialize / Proceed / Confirm / Terminate / GetIntersection*`；
- 原始 primitive ID 的正确映射。

### 明确不支持

- `VK_KHR_ray_tracing_pipeline`、`vkCmdTraceRaysKHR`、SBT；
- raygen / miss / closest-hit / any-hit / intersection / callable shader；
- 使用硬件 RT Core 或导入硬件私有 AS 格式；
- 任意第三方应用的完全透明兼容；
- v1 中的 AABB procedural geometry、AS update/refit、compaction、serialization、indirect build。

## 3. 为什么 Ray Query 不需要 callable shader

Ray Query 和 RT pipeline 的区别不是“遍历算法不同”而是**控制流模型不同**。

| 功能 | RT pipeline | Ray Query | 本设计 |
|---|---|---|---|
| 开始 tracing | `OpTraceRayKHR` | `OpRayQueryProceedKHR` | `sw_ray_query_proceed()` |
| 是否切换 shader stage | 会 | 不会 | 不会 |
| any-hit 类似决策 | any-hit shader | 当前 shader 观察 candidate 并 confirm | 当前 shader 观察软件 state 并 confirm |
| callable shader | `OpExecuteCallableKHR` | 无 | 无 |
| SBT / 递归 | 需要/可用 | 无 | 无 |

`Proceed` 返回 candidate 只是一个可恢复状态机接口，不是一次 shader 调用。执行 `Proceed` 后，仍然是原来的 compute/fragment invocation 在运行。

例如，应用侧的语义：

```glsl
while (rayQueryProceedEXT(q)) {
    if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
        if (acceptCandidate(q))
            rayQueryConfirmIntersectionEXT(q);
    }
}
```

layer 改写后的语义：

```glsl
while (sw_ray_query_proceed(q)) {
    if (sw_get_candidate_type(q) == TRIANGLE) {
        if (acceptCandidate(q))
            sw_ray_query_confirm(q);
    }
}
```

两段 `acceptCandidate` 都是原 shader 的普通控制流；没有跳转到新 pipeline。

## 4. SPIR-V 中“调用其他工作”的边界

| 机制 | 能力 | 为什么不用它实现通用 Ray Query |
|---|---|---|
| `OpFunctionCall` | 同一 module、同一 invocation 内的普通函数调用 | **采用**；可直接承载软件 traversal |
| `OpTraceRayKHR` / `OpExecuteCallableKHR` | RT pipeline 创建/切换 shader invocation | 需要 RT pipeline，本项目排除 |
| `VK_AMDX_shader_enqueue` / execution graph | compute node 可 enqueue 其他 node | 是异步 payload 调度，无逐 invocation 的同步 call/return；扩展支持也不通用 |
| OpenCL `OpEnqueueKernel` | device-side enqueue | 不属于 Vulkan SPIR-V execution environment |

因此所谓“traversal shader”只能是源码组织方式：可以把 traversal 写在独立的 GLSL/HLSL/SPIR-V library 中，但 layer 必须在创建 shader module 时把它链接/注入进原 module，形成 `OpFunctionCall`。驱动最终只看到一个已变换的 compute shader。

## 5. 总体架构

```text
Application
  |  KHR AS API + 含 RayQuery 指令的 SPIR-V
  v
Software RT Layer
  |-- virtual AS registry
  |-- API/descriptor/pipeline shadow translation
  |-- layer-owned GPU BVH build compute pipelines
  |-- SPIR-V Ray Query lowering pass + traversal function library
  v
普通 Vulkan ICD
  |  buffer / compute / device address / synchronization
  v
GPU
```

底层设备只需要普通 Vulkan compute 能力；优先要求 Vulkan 1.2、storage buffer 和 buffer device address。不要求暴露 RT 扩展。

## 6. 两种部署模式

### 6.1 Bootstrap 模式

layer 放在已支持 Ray Query 的 ICD 上方。它截获 AS build 和 shader module，替换成软件路径，但可用底层原生 Ray Query 作为结果对照。

优点：最适合调试 shader lowering 和语义测试。

### 6.2 Virtual-extension 模式

layer 自己提供受限的 `VK_KHR_acceleration_structure` 与 `VK_KHR_ray_query` 语义；在创建下层 device 前移除这些扩展名，并提供所有已虚拟化的 entry points。

优点：可运行在无 RT Core 的 Vulkan GPU。

这才是最终目标，但应在 Bootstrap 模式语义跑通后做。

## 7. 虚拟 Acceleration Structure

应用看到的是 `VkAccelerationStructureKHR`，但该 handle 由 layer 管理，绝不传给下层 ICD。

```cpp
struct VirtualAS {
    VkAccelerationStructureTypeKHR type; // BLAS / TLAS
    VkBuffer backingBuffer;              // 自定义 BVH 的最终存储
    VkDeviceSize offset;
    VkDeviceSize size;
    VkDeviceAddress headerAddress;
    VkDeviceAddress nodeAddress;
    VkDeviceAddress leafAddress;
    uint32_t buildGeneration;
};
```

layer 拦截以下 API：

| API | 行为 |
|---|---|
| `vkCreate/DestroyAccelerationStructureKHR` | 创建/删除 `VirtualAS` 记录 |
| `vkGetAccelerationStructureBuildSizesKHR` | 返回自定义 layout、scratch 的大小 |
| `vkGetAccelerationStructureDeviceAddressKHR` | 返回自定义 `BvhHeader` 地址 |
| `vkCmdBuildAccelerationStructuresKHR` | 记录自定义 compute builder，不调用下层 AS build |
| `vkCmdBuildAccelerationStructuresIndirectKHR` | v1 拒绝；后续做 GPU-driven builder |
| copy/compact/serialize/update | v1 明确拒绝或仅支持重建 |

## 8. Buffer 与 descriptor 翻译

应用 AS storage buffer 原本通常只有 `ACCELERATION_STRUCTURE_STORAGE` usage。layer 向下层创建的实际 buffer 额外加入：

- `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`；
- `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`；
- 调试/拷贝需要时加入 `TRANSFER_SRC/DST`。

内存分配中相应加入 device-address flag。

对于 `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`：

1. layer 创建 shadow pipeline layout；
2. 将对应 binding 的下层 descriptor type 改为 storage buffer；
3. 拦截 `vkUpdateDescriptorSets`，把 virtual AS 写入转换为指向 `BvhHeader` 的 buffer descriptor；
4. SPIR-V lowering 保留原 `(set, binding, array element)` 编号，只替换其类型和访问方式。

## 9. 自定义 BVH 格式

初版采用 GPU 友好的 BVH4，内部节点采用 SoA bounds：

```cpp
struct BvhHeader {
    uint32_t magic, version, nodeCount, rootIndex;
    uint64_t nodeAddress, leafAddress, primitiveAddress;
};

struct Bvh4Node {
    float minX[4], minY[4], minZ[4];
    float maxX[4], maxY[4], maxZ[4];
    uint32_t childRef[4]; // 高位标记 leaf/internal
};
```

leaf 引用连续 primitive range。若 builder 改变三角形排序，必须保存 `sortedPrimitive -> originalPrimitiveID` remap；因为 Ray Query 的 primitive ID 对应用可见。

TLAS 复用同类节点布局。instance leaf 保存 BLAS header address、object/world transform、mask、custom index 和 flags。

## 10. GPU Build 路径

`vkCmdBuildAccelerationStructuresKHR` 被拦截后，layer 在**同一个 command buffer**插入：

```text
Morton code
  → radix sort
  → binary radix tree
  → bottom-up bounds
  → collapse to BVH4
  → emit header / leaf / primitive remap
  → compute-write → shader-read barrier
```

初版算法选择 LBVH，是为了先获得全 GPU、可预测大小、易验证的 baseline；后续 builder backend 可以切换 PLOC、HPLOC、binned-SAH 或 SBVH，而 traversal ABI 不变。

内部 compute bind 会污染 Vulkan command-buffer state，所以 layer 需要追踪并恢复应用的 pipeline、descriptor set、push constant 和 dynamic state。v1 限制 build 命令在 render-pass scope 外录制。

## 11. Ray Query SPIR-V lowering

### 11.1 输入与输出

输入：含 `OpTypeAccelerationStructureKHR`、`OpTypeRayQueryKHR`、`OpRayQuery*KHR` 的 compute SPIR-V。

输出：不含 KHR AS/RayQuery 类型与指令的普通 compute SPIR-V；它包含 layer 注入的 traversal 函数和 custom-BVH buffer 访问。

遇到 `OpTraceRayKHR`、raygen/hit/miss/callable execution model 时直接报 unsupported，而不是试图下发到下层。

### 11.2 Query state

每个 `OpTypeRayQueryKHR` 局部对象被改写为私有状态：

```cpp
struct RayQueryState {
    Ray ray;
    float tMin, tMax;
    Hit candidate;
    Hit committed;
    uint32_t stack[SOFTWARE_RT_STACK_SIZE];
    uint32_t stackSize;
    bool terminated;
};
```

`OpRayQueryInitializeKHR` 初始化 state；`OpRayQueryProceedKHR` 变为：

```text
OpFunctionCall sw_ray_query_proceed(state)
```

该函数在**当前 invocation**内从 state stack 恢复 traversal，直到：

- 找到一个可报告的 candidate，返回 `true`；或
- traversal 耗尽，返回 `false`。

之后原 shader 自己运行 candidate 相关分支，并通过 `Confirm`、`Generate` 或 `Terminate` 更新 state。下次 `Proceed` 从保存状态继续。

### 11.3 Opaque 与 candidate 语义

对 opaque triangle，可直接更新 committed hit，不必将它作为可拒绝 candidate 暴露给 shader。

对 non-opaque triangle/AABB，必须实现 candidate-resume 状态机；不能把 `Proceed` 错误简化为“一次求最近命中”，因为应用可能读取 candidate、拒绝它、confirm 它或生成自定义交点。

v1 可先仅支持 opaque triangle + committed result；完整 Ray Query 兼容版本再加入 non-opaque/AABB candidate。

## 12. 同步与正确性

builder 最后一 pass 后插入：

```text
srcStage  = COMPUTE_SHADER
srcAccess = SHADER_WRITE
dstStage  = 后续使用 transformed query shader 的 stage
dstAccess = SHADER_READ
```

layer 不改变原应用的资源生命周期与跨 command-buffer 同步责任。若应用本就允许同一 AS 的 build 与 traversal 数据竞争，virtual AS 同样不保证结果。

## 13. 验证计划

1. 单三角形：miss/front/back hit。
2. 多深度三角形：candidate 顺序、confirm、最近 committed hit。
3. 排序后的 primitive remap：验证 primitive ID 不变。
4. TLAS instance：transform、mask、custom index。
5. 随机射线：同 CPU reference BVH 比较 `t`、barycentrics、ID、front-face。
6. Bootstrap 模式：与原生 Ray Query 逐项对照。
7. Virtual-extension 模式：在无 RT GPU 上运行同一受控 shader。

记录 build time、node visits、triangle tests、stack depth、bytes/primitive、rays/s 与 active-lane divergence。

## 14. 里程碑

| 阶段 | 交付物 |
|---|---|
| M0 | layer dispatch 框架；shader module 拦截；不支持路径 fail-fast |
| M1 | 单 BLAS、CPU 生成调试 BVH、compute shader 内 `sw_ray_query_proceed` |
| M2 | GPU LBVH builder、同 command-buffer barrier、shadow descriptor |
| M3 | TLAS、instance transform/mask、primitive remap |
| M4 | 完整 candidate-resume、non-opaque triangle、AABB procedural geometry |
| M5 | PLOC/HPLOC/SBVH builder backend、refit、graphics-stage query |
| M6 | virtual-extension 模式、无 RT GPU 验证 |

## 15. 设计决策

layer 是验证“可移植的软件 Ray Query ABI”的好入口：它能让受控应用保持 KHR 风格 API，并把 build/traversal 算法独立于厂商 RT 格式。

但它不应伪装成“可在 shader 内同步调用新 compute shader”的机制。若未来想把 traversal 拆成多 kernel，只能把 renderer 改写成 ray queue / wavefront 多 pass：生成 ray queue → traversal pass → hit/miss queue → shading pass。这是另一种 renderer 架构，而不是对任意 Ray Query 的透明 layer 替换。
