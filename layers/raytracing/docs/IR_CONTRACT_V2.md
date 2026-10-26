# XM / VKRT 公共 BVH IR v2

更新日期：2026-09-24。本文描述本轮实现的输入协议。它替代 XM 原来的 `[header108][nodes]` 和 VKRT 原来的 256 B 头区协议；v1 不再自动识别或转换。

## 1. 共同边界

应用、高校 builder 和 VKRT 内置 builder 输出同一种容器。XM 将它编码为 Mali AS；VKRT 直接遍历其中的二叉树。公共 builder 在 [layer/bvh/](layer/bvh/) 中；XM DDK 的 `samples/custom_bvh/portable/` 导出同一份源码，该目录的 `check_sources.py` 校验 12 个公共文件及 HAL 容器常量完全一致。

公共构建入口是 `bvh_ir_record_build()`。它只依赖 Vulkan 类型和调用者提供的 dispatch callback，不依赖 VKRT layer 状态、XM 驱动内部函数、旧格式探测或失败后的其他构建路径。算法私有 Morton/拓扑/扫描 scratch 不进入消费协议。

Mesa 对照基线为 `d1a2854aec69179f2b80d1c7368771041181cda5`。`vk_ir_header`、triangle、instance、box 等记录 ABI 不变；Mesa 没有规定一个可直接持久化的完整 IR buffer 容器。

## 2. 布局

| buffer 字节偏移 | 内容 | 语义 |
| ---: | --- | --- |
| 0–107 | `vk_ir_header` | 保留 Mesa 108 B 记录 |
| 108 | `uint32_t magic` | `0x58424856` |
| 112 | `uint32_t version` | `2` |
| 116 | `uint32_t leaf_capacity` | 输入 primitive 总槽数 N，包括 inactive 槽 |
| 120 | `uint32_t geometry_type` | Vulkan triangles=0、AABBs=1、instances=2；一棵树内类型一致 |
| 124 | `uint32_t build_flags` | v2 当前支持 0，即基础叶格式；不接受预配对 quad / ALWAYS_ACTIVE 输入 |
| 128 | 预留叶区 | `N * leaf_stride`，按槽位寻址 |
| `128 + N * leaf_stride` | box 区 | 实际 box 在前，children-before-parents，根是最后一个实际 box |

底层分配可有额外尾部 padding；叶区和 box 区之间没有额外 padding。输入地址需满足 XM 当前 64 B 对齐要求。`bvh_ir_build_layout()` 给出 GPU builder 的最坏情况 IR 与私有 scratch 分配量。

```text
N = leaf_capacity                    // build ranges 的 primitive 总数
A = header.active_leaf_count         // 实际进入最终树的叶数
I = header.ir_internal_node_count    // 当前基础二叉路径为 max(A,2)-1
T = leaf_stride                     // triangle 68 / AABB 32 / instance 96

node_base       = buffer + 128
box_base_offset = N * T             // node-id 空间
root_id         = (N*T + (I-1)*40) | vk_ir_node_internal
node_address    = node_base + (id & ~3u)
minimum_bytes   = 128 + N*T + I*40
capacity_bytes  = 128 + N*T + (max(N,2)-1)*40
```

例如 N=4、A=2 的三角形树：I=1，root id=273，根物理地址为 buffer+400。不能按 A*68 寻找 box 区，也不能按预留 box 数找根。单三角形 N=A=1：leaf id=0、root id=69，实际地址分别是 buffer+128 和 buffer+196。

## 3. 与 Mesa 对齐的字段及活动图元

| 对比项 | XM v2 / VKRT v2（统一） | Mesa 公共默认 BUILD |
| --- | --- | --- |
| 记录 ABI | 108 B header，triangle 68 B、box 40 B、instance 96 B | 相同 |
| 完整容器 | `[header108][metadata20][leaves][boxes]` | 不规定持久化容器；header 和 IR 基址由构建调度传入 |
| node ID 基准 | `buffer + 128` | `ir_bvh` / `bvh` 节点区域基址 |
| box 区起点 | `node_base + N*T`，无额外对齐 | 相同容量计算 |
| 根定位 | 最后一个实际 box：`N*T + (I-1)*40` | 默认最终二叉 IR 相同 |
| header byte 80 | `dst_leaf_node_offset`，交付编码前为 0 | 编码器状态字段，不存 root handle |
| inactive 槽位 | N 包含 inactive；A 只计入树叶；未引用槽位不读取 | 相同 |
| 空树 | I=1，双 invalid child，+inf/-inf 空 bounds | 相同 |
| Morton/临时拓扑/扫描 | builder 私有 scratch，不交付消费端 | 后端可能复用分配空间，但不等于复用这些算法结果；后端编码工作区另行初始化 |


- **byte 80 是 `dst_leaf_node_offset`。** 它是编码器的叶区偏移/分配字段，输入初始化为 0；绝不存根句柄。byte 76 的 `dst_node_offset` 同样是编码状态。
- **实际根由实际 box 数定位。** 叶区仍按输入容量 N 预留，与 Mesa 的 `leaf_node_count * ir_leaf_node_size` 加 `ir_internal_node_count - 1` 定位方式一致。
- **A 可以小于 N。** inactive 叶槽不必初始化，树不得引用它们；活动叶保留原 primitive/geometry/instance ID。不要把活动叶数等同于预留容量。
- 默认 BUILD 按 Mesa 的规则过滤含 NaN 的三角形、null BLAS 实例和 mask=0 实例。公共 builder 的 AABB 路径也过滤含 NaN 的 AABB。无有效 bounds 的空 BLAS 实例不会进入 TLAS。
- **A=0 仍有一个空根 box。** 两个 child 都为 `0xffffffff`，AABB 为 min=+inf / max=-inf；header bounds 是对应的 emulated float。遍历得到 miss。
- header bounds 使用 Mesa emulated float，节点 bounds 和顶点使用 IEEE float。dispatch Y/Z 为 1。
- `sync_data` 与 `tmp_active_leaf_count` 属于 builder 过程状态，XM 不要求它们为零，也不据此定位节点。`box.bvh_offset` 初值为 `VK_UNKNOWN_BVH_OFFSET`。
- 容器身份信息已经移出 `driver_internal[]`。公共 producer 初始化六个私有字为 0；XM 仍使用 `driver_internal[2]`（byte 92）回传状态，VKRT 不将该字段用作树结构信息。

XM GPU validator 分两次 dispatch：先检查 header/box 和引用并统计入度，再检查实际被引用的叶及父节点唯一性。由此避免扫描 inactive 槽位；同时检查实际计数、children-before-parents、根无父节点和其他 box 唯一父节点。错误引用检查失败后不再解引用；encode 在 status 非 OK 时退出。这些检查尚不等于全部几何正确性和硬件输出都已验证。

## 4. 上层应用接线

公共组件接收显式 `output_ir`、`scratch`，不把 Vulkan `dstAccelerationStructure` 当作 IR 的唯一存储位置。应用为五个 shader 建立 pipeline，并在 callback 中按 stage 绑定 pipeline、推送参数、dispatch、建立存储可见性依赖。pipeline 参数和文件列表见 DDK `samples/custom_bvh/portable/README.md`。

- **XM：** 公共 builder 写入应用 IR buffer；设置 `buildInfo.scratchData.deviceAddress = output_ir`，`dstAccelerationStructure` 指向单独的硬件 AS，再调用 `vkCmdBuildAccelerationStructuresXM`。
- **VKRT：** 公共 builder 写入软件 AS 的 backing buffer；同一 IR 可由软件遍历直接消费。VKRT 拦截 KHR build 的实现也调用这个公共记录函数，因而没有另一套输出封装。
- **比较算法：** 固定格式、场景和消费配置，允许树拓扑不同；额外格式导出成本计入构建。每次编码从未编码的 IR 状态开始。

## 5. TLAS 资源引用与必要边界

`instance.base_ptr` 保留 Mesa 的含义：消费后端所使用的 BLAS 设备地址。XM 需要 Mali 硬件 BLAS 地址，VKRT 需要软件 BLAS IR 地址。两者的物理地址不可能靠统一记录布局自动相等；这属于资源绑定，不能将软件 BLAS 地址直接交给 RTU。

公共 leaf builder 不再读取某个硬件私有 header 的固定 AABB 偏移。它从公共 BLAS IR 的 header 读取 bounds：应用可通过 `blas_ir_addresses` 显式提供每个实例的 BLAS IR 地址数组，同时在 instance 输入中提供目标后端的 BLAS 地址；零数组地址的约定是 instance 引用本身就是公共 BLAS IR 地址。两种绑定都读取同一个 v2 格式，不进行格式识别后尝试其他布局。

因此，同一个应用构建实现可用于两种后端；若要求同一个 TLAS buffer 的地址字段也逐字节相同、同时指向两个不同 BLAS 对象，则当前直接指针协议无法满足，需要另设计公共句柄表及后端解析接口。本轮没有引入这种新句柄 ABI。

## 6. 能力边界与验证

三个指定的语义——根定位、byte 80、默认 BUILD 活动图元处理——均已向 Mesa 对齐，没有必须保留旧差异的硬件理由。

完整 scratch 容器不与 Mesa 相同：Mesa 通过外部参数传叶容量、类型和 IR 基址，软件遍历则需要从持久化 AS 中取得这些信息，因此增加了 20 B 描述区。该区在 Mesa header 外，且 XM/VKRT 完全一致。

当前公共基线不覆盖 Mesa 的全部可选模式。XM 的 AABB/CGEO 编码尚未实现，UPDATE 也未实现，故不能声明其能力与 VKRT 或 Mesa 全集一致；这不是记录 ABI 不兼容，也不是硬件证明不可能。预配对 quad、ALWAYS_ACTIVE、空间分裂/图元复制等需要单独协商和实现，不能静默降级为本基线。

本轮已验证：公共 builder/软件遍历 SPIR-V 编译、XM 内部 validate/encode 编译、HAL 编译和 `libGLES_mali.so` 链接；既有主机测试及 VKRT CPU producer → XM 契约的 dense/sparse/empty BLAS/TLAS 交叉测试。机器仅有缺少 BDA/scalar layout/Vulkan memory model 的旧 llvmpipe，未运行 v2 GPU 链路。XM 仍有独立的硬件编码缺陷（保留节点分配、硬件 leaf 地址布局等），见 DDK `agents.md`。格式统一不等同于硬件 PoC 已验收。
