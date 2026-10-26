# vkrt_layer — 自定义 GPU BVH 实现状态

原始实现记录：2026-09-24。设计：`rayquery_software_rt_layer_design.md`。

VEL 导入说明：本目录由独立的 Ray Query 补丁导入，与 RT pipeline fallback
分别构建、分别启用，AS 格式不能混用。构建不依赖私有 Mesa checkout；依赖和
独立构建方法见 [README.md](../README.md)。当前代码采用下述 BVHIR v2；本文件保留的
Windows / RTX 3080 验证记录属于原始工作及当时的格式版本，不能视为本次导入或
当前 v2 GPU 链路已经重新验证。当前导入的实际检查结果应与历史记录分开报告。

本次 VEL 集成验证（Ubuntu 24.04）：standalone Release 全目标编译、builder/traversal
SPIR-V 编译与 `spirv-val`、4 项 offline CTest 均通过。使用 Vulkan headers 1.3.275、
glslc 2023.8 和 SPIRV-Tools 2025.1。生成的 manifest 指向实际存在的 layer 库。
本次未运行 GPU/native RT 测试，未在 Windows 执行脚本。

## 1. 当前可用范围

自定义 **GPU LBVH 构建 → BVHIR → 软件 Ray Query 遍历** 已接通，不再使用
CPU 构建数据冒充 GPU 构建验证。运行时的 AS build 不转发给硬件 RT builder。
当前仍是 bootstrap layer：设备需要原生 RT 扩展供应用创建和原生对照，
尚未实现无 RT GPU 的扩展虚拟化。

这里的“去除下层 RT 依赖”主要指 API 能力暴露和设备创建，不是当前已观察到的
驱动 shader 编译失败。支持路径的 Ray Query 指令在交给驱动前已改写为软件遍历；
但 `vkrt_create_device_with_features` 仍保留应用请求的 RT extension 名称和 feature chain。
无 RT 驱动可能在应用能力检查或 `vkCreateDevice` 阶段就被挡住，尚未进入 shader 编译。
M6 需要由 layer 暴露已实现的能力，并在下传时过滤/转换 RT 请求、资源标志和相关 API，
同时保留 BDA、整数类型、memory model 等软件后端真实需要的非 RT 功能。

| 部分 | 状态 |
|---|---|
| M0 / M1：layer、描述符改写、SPIR-V 注入、CPU 调试 fixture | 已验证 |
| M2：GPU LBVH、size/scratch 布局、pipeline 生命周期、状态恢复 | 已接通并验证 |
| M3：BLAS/TLAS、实例变换/掩码、原始 primitive/geometry/instance ID | 已验证 |
| M4 子集：非 opaque triangle confirm、AABB generate、candidate resume、对象空间 getter | 已验证 |
| graphics-stage query | `ray_queries` sample 的 vertex / fragment 改写和渲染已跑通 |
| M5：PLOC/HPLOC/SBVH 后端、refit | 不在本次 LBVH 路径范围内；尚未接入 |
| M6：virtual-extension / 无 RT GPU | 未实现、未验证 |

## 2. 实现概要

构建路径：

```text
vkCmdBuildAccelerationStructuresKHR
  → init（header / counters）
  → leaf（各 geometry、活动图元计数）
  → Morton sort（含跨 workgroup 合并）
  → LBVH topology
  → bottom-up bounds / root
  → software traversal
```

- 5 个 compute pipeline 按 device 创建和销毁；构建/遍历 SPIR-V 内嵌 DLL，
  无需从应用工作目录寻找 `sw_ray_query.spv`。
- 设备创建查询并启用 BDA、scalar layout、int8/16/64、storage8/16、
  Vulkan memory model / DeviceScope；复制应用 feature chain，不修改调用者内存。
- CB 映射直接关联 owning device；跟踪 compute pipeline / push constants，
  内部构建结束后恢复。BDA-only pass 不绑定描述符，也不改变 graphics 状态。
- 内部 pass 插入保守内存屏障；应用 Barrier/Barrier2 与 QueueSubmit/Submit2
  的 AS-build stage / AS access 扩展为 compute / shader access，含 KHR 别名。
- 不支持的 build mode / geometry extension / format、容量或 dispatch 超限，
  在 `vkEndCommandBuffer` 返回错误，不把未构建对象交给驱动。
- builder/traversal shader 在构建时执行 `spirv-val`；每个改写结果在进入驱动前
  由 SPIRV-Tools 校验，失败返回诊断。

BVHIR ABI（builder、遍历、CPU fixture 共享）：

```text
[0, 108)     Mesa vk_ir_header
[108, 128)   v2 metadata：magic / version / leaf_capacity / geometry_type / build_flags
[128, ...)   按输入容量预留的 leaves，随后紧接 internal boxes
```

byte 80 保留 `dst_leaf_node_offset` 的编码器字段含义，输入初始化为 0，不存根句柄。
节点 ID 相对 byte 128；叶区按输入槽容量 N 预留，box 区起点为 `N * leaf_stride`。
根是最后一个实际 box：`(N * leaf_stride + (I - 1) * 40) | 1`，其中 I 来自
`header.ir_internal_node_count`。空树保留一个无有效 child 的根 box。
header bounds 使用 Mesa emulated float；不再有 byte 128 的额外 float bounds 副本。
完整契约见 [IR_CONTRACT_V2.md](IR_CONTRACT_V2.md)。旧 256 B 容器不再接受。

句柄为字节偏移 OR 类型 tag：triangle=0、internal=1、instance=2、AABB=3；
无效句柄为 0xFFFFFFFF。triangle / AABB / instance / box stride 分别为
68 / 32 / 96 / 40 bytes。查询 size 与实际 dispatch 共用布局计算。
TLAS 由原 AS descriptor 改写的 SSBO 读取，BLAS 由实例中的设备地址读取。

## 3. 验证与复现

以下为原始补丁保留的历史验证记录；当前 v2 的 GPU 验证界限见本文件开头和
`IR_CONTRACT_V2.md`，不能由这些旧记录推断本次导入的通过情况。

历史验证设备：Windows、RTX 3080、NVIDIA 610.74、Vulkan SDK 1.4.350.0。
软件 GPU 测试启用 Khronos validation + synchronization validation 后也通过；
日志无 Validation Error / VUID / SYNC-HAZARD（验证器不能完整追踪所有 BDA 访问，
因此不能代替数值差分和越界哨兵测试）。

当前 CMake 定义 7 个 CTest（4 个 offline、3 个 GPU）。源码中的用例覆盖：

- BVHIR v2 容器、布局和 builder callback 参数契约。
- 宿主转换回归：AS 描述符数组逐元素改写、混合 writes/copies、null descriptor，
  以及已有 memory flags 时补齐 BDA、保留 deviceMask / 其他 flags / pNext，且不修改调用者数据。
- SPIR-V 操作数回归：只替换 ID 引用，不改常量、binding、offset、LocalSize、
  switch literal 或字符串；解析失败不留下部分改写。
- 离线 SPIR-V 改写：去除 RayQuery/AS 类型与指令，并执行 validator。
- CPU BVHIR fixture：最近命中、候选恢复、实例变换、阴影、mask。
- GPU 软件路径和原生路径使用同一测试程序、同一解析期望值：
  0 / 1 / 2 / 257 / 1025 triangles，随机斜射线、miss、inactive primitives、
  索引/非索引、多个 geometry、ppGeometries、各种输入 byte offset。
- 变换、barycentrics、geometry/primitive/instance/custom ID、opacity/culling flags；
  AABB procedural intersection、实例指针数组、null BLAS、mask=0、
  多实例候选恢复、object ray / object-to-world / world-to-object 一致性。
- 原位重复 BUILD、应用先绑定 compute pipeline/descriptor/push constants 再 build；
  分离 BLAS/TLAS command buffer，以 semaphore 和 Submit/Submit2 连接。
- 软件 AS / scratch 越界哨兵、活动叶计数、节点可达性、环检查和父子 bounds；
  UPDATE / 过小输出的拒绝路径、command-buffer reset 后错误状态清理。

Windows 一键测试（在仓库根目录执行）：

```powershell
# 独立编译 Release layer 并运行全部 CTest
.\layers\raytracing\run-tests.bat
# 可选：测试后启动外部 Vulkan-Samples；第二参数是其资产工作目录
.\layers\raytracing\run-tests.bat "C:\Vulkan-Samples\build\app\bin\relwithdebinfo\AMD64\vulkan_samples.exe" "C:\Vulkan-Samples"
```

需要 CMake、Ninja、Python 3、Vulkan SDK（含匹配的 `vk.xml`、SPIRV-Tools 库）
和 x64 C/C++ 编译器。
脚本独立配置 `build/vkrt_layer` Ninja 构建；需在编译器可用的终端运行。
任一步失败都会返回非零状态。外部 sample 需要事先构建，可用首个参数或
`VKRT_SAMPLE_EXE` 指定路径，用第二参数或 `VKRT_SAMPLE_DIR` 指定工作目录。
脚本不传帧数限制，sample 是交互运行；无参数时不运行 sample。
脚本通过 `setlocal` 临时设置环境变量；原生 GPU test 由 CTest 自动关闭本 layer。

手工命令：

```powershell
cmake -S layers/raytracing -B build/vkrt_layer -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/vkrt_layer
ctest --test-dir build/vkrt_layer --output-on-failure

# 仅离线测试（无需 GPU）
ctest --test-dir build/vkrt_layer -L offline --output-on-failure

# 手工运行 GPU 测试
Push-Location build/vkrt_layer
$env:VK_LAYER_PATH="$PWD/layers"
$env:VK_INSTANCE_LAYERS='VK_LAYER_VKRT_SOFTWARE_RAY_QUERY'
./test_gpu_build.exe
$env:VK_INSTANCE_LAYERS=''
./test_gpu_build.exe
Pop-Location
```

`test_single_tri` 的 CPU 格式 fixture 仅用于软件路径；
原生 GPU 对照使用 `test_gpu_build`。测试 shader 仍由测试工作目录加载。

`ray_queries` sample 已用软件/原生路径各运行 3 帧并输出截图：
`output/images/vkrt-ray-queries-checked.png`、
`output/images/native-ray-queries-checked.png`。软件路径构建约 26 万三角形。
图像没有作为严格逐像素通过依据：sample 灯光使用墙钟时间，
TerminateOnFirstHit 的遍历顺序也可能不同。数值正确性以确定性射线测试为准。

### 2026-09-24 review 修复与复测

- review 第 1、2、5 项已修复：AS descriptor 数组转换越界；SPIR-V 将字面量误当 ID
  替换（含类型去重与 entry-point 名称处理）；已有 allocation flags 时漏补 BDA 位。
- 新增 `test_host_translation` 和 `test_spirv_ids`；完整 CTest 6/6 通过（约 133 秒）。
  补充截断 SPIR-V、未知 pNext 前缀拒绝用例后，重新构建和离线测试 3/3 通过。
- 最新软件 layer 的 `ray_queries` sample 复测 3 帧，平均 4.66 FPS，正常退出；
  日志报告设备内存泄漏 0 bytes，截图更新为 `output/images/vkrt-ray-queries-checked.png`。
  此次是短冒烟测试，不是新的 10 秒稳定性测试，也未重跑 validation 配置或原生 sample 对照。
- review 第 3 项（部分不支持命令只记录日志、部分 host AS API 仍可能下传）和
  第 4 项（AABB opacity 未完整应用 ray/instance override）尚未修复。
- 下层 RT 依赖去除仅记录现状与后续方向，本次没有实现 M6。

## 4. 边界与后续工作

- 不是完整 Vulkan RT 实现：UPDATE/refit、copy/compact/serialization/indirect、
  RT pipeline、micromap、motion geometry、TLAS 套 TLAS 不支持。
- 构建需要 compute-capable queue，且必须在 render pass 外。
  32-bit node addressing、dispatch X 上限会限制单次构建规模。
- 目前测试是同一 queue 的跨 command buffer 提交；
  多 queue ownership transfer、event-based AS 同步、多设备并发尚未验证。
  Event 命令尚未做 AS→compute stage/access 转换，不应使用它们同步软件 AS。
- 目前 SPIR-V 注入针对单一 AS binding；descriptor indexing/多 AS 绑定、
  额外 RayQuery 扩展指令不等于完整支持；改写后校验失败会拒绝 shader。
  宿主侧已支持 AS descriptor 数组更新，但不代表 shader 侧数组查询已支持。
- 运行时未做性能调优：pipeline/shader cache、较细屏障、PLOC、
  更快遍历、全面 feature/limit 虚拟化属于后续。
- `super_resolution` sample、无 RT 设备、跨厂商设备尚未验证。
- CPU debug builder / oracle 仍仅用于调试，不是 robust edge/vertex 命中的完整硬件 oracle。
- `vkAllocateMemory` 对所有分配注入 device-address flag 的适用性、全局对象表初始化并发、
  shader-object/push-constant2 等新 API 路径仍需后续兼容性审计。

---

## 5. 踩过的坑(避免再犯)

以下记录保留实现中容易复现的问题。

### 构建 / 工具链
- **必须 `CMAKE_BUILD_TYPE=Release`** —— SDK 的 SPIRV-Tools 是 release-CRT;无类型构建混 CRT,
  二进制在 `main` 之前静默崩(exit 3,无输出)。
- **`VKAPI_ATTR` 在 Windows 上是空的** —— `vk_layer.h` 声明了 negotiate 入口但不导出它。
  用 **`.def` 文件**导出(`vkrt_layer.def` 放进 `target_sources`)。加 `__declspec(dllexport)`
  会触发 `-Wdll-attribute-on-redeclaration`。
- `-Wl,-Bsymbolic-functions` 是 GNU ld 专用,lld-link 会报错。

### SPIR-V / glslang
- **ray-query 操作码不连续**:核心 4472–4479,`GetIntersectionTriangleVertexPositions` = 5340,
  而 **多个 RayQuery getter 在 6016–6032**。
  用 `4472..4490` 会漏掉 sample 真正调用的那些。以 `spirv/unified1/spirv.hpp` 为准。
- **glslang 的函数 ABI:所有参数都按 `Function` 指针传**(`uint`/`float`/`vec3` 都是)。
  所以每个 `sw_*` 入口只收 `inout q`,其余实参由改写侧写进 `q` 的字段;调用点用
  `Function` 局部临时做 load/store 复制(只能放在所在函数的**第一个 block**)。
- **`rayQueryEXT q` 是 `Private` 全局**,不是 Function 局部 —— 和 `inout` 参数的 `Function`
  指针类型对不上。用 `OpBitcast` 也不行(结果不是 memory object declaration)。
- **GLSL 的交点枚举两组共用 0/1/2**(`CandidateIntersectionTriangle=0`,
  `CommittedIntersectionTriangle=1`),不是 SPIR-V 的返回码(1/2/3/4)。
- **glslang 会 DCE 掉 `main` 不可达的函数** —— 库的 dummy `main` 必须调用全部 `sw_*`。
- **不要给 glslc 加 `-O`** —— `spirv-opt` 剥掉 `OpName`,而改写靠名字找符号。
- **`spvtools::Link` 只保留导出符号** —— 每个 `sw_*` 要 `OpDecorate LinkageAttributes Export`
  (glslc 不会生成),链接后再把这装饰删掉(Vulkan 禁止 `Linkage` 能力)。
- **`spvtools::Link` 拒绝混合寻址模型** —— 先把用户模块改成 `PhysicalStorageBuffer64`(超集)。
- **`OpFunctionCall` 即使 void 也带 Result Type + Result id**,长度 = `4 + noperands`,且必须在写
  word0 **之前**算好。少一个字会静默截断指令流。
- **`OpAccessChain` 的索引必须是常量 id**,不能塞字面量。
- **`OpTypeForwardPointer` (39) 没有 result id**,进不了 `defs`/`need` 表,会被落下 —— 必须排在
  它声明的 `OpTypePointer` 之前。
- **`OpString`(7)/`OpExtInstImport`(11) 的 result id 在 word 1**,不是 2。取错会把**字符串字面量**
  当 id,污染 `ids.next`(典型症状:垃圾 id `1935627353`)。
- **`OpModuleProcessed`(330) 会被 validator 当成 debug 段结束** —— 它后面的 `OpName` 全部报
  "in an invalid layout section"。直接删掉(只是 linker 标记)。
- **`OpVariable` 的 storage class 在 word 3**,不是 4。
- **`VkDescriptorPoolSize` 的成员叫 `type`**,不是 `descriptorType`(那个是 `VkWriteDescriptorSet` 的)。
- **不能把逻辑指针 `OpBitcast` 成 `PhysicalStorageBuffer`** —— "may not have a logical pointer operand"。
- **buffer_reference 类型不能做 Function 局部变量** —— 带 Offset/ArrayStride 显式布局,
  报 `[VUID-StandaloneSpirv-None-10684]`。逐字段读取(`blas_u32(base, word)`)。
- **类型/常量段要拓扑排序**,否则 `OpTypePointer` 会排在它指向的 `OpTypeStruct` 之前。
  但**排序/提升趟绝不能丢字节** —— 加 `if (out.size() != original_size) return;`,且解析中断处的
  尾部要原样保留(一个畸形指令会静默删掉它后面所有类型)。
- **`get_*` 类型查找必须直接扫模块**,不要索引一张会因 splice 变陈旧的偏移表 —— 陈旧查找会造出
  重复类型,之后所有字节数都对不上。
- **id 块分配要一次推进完**:`base = fresh()` 然后用 `base+k` 而不推进 `next`,会让后续每次
  `fresh()` 都撞 id。

### Vulkan / 层
- **层名 ≠ DLL 名**:`VK_INSTANCE_LAYERS` 要填 manifest 的 `name`(`VK_LAYER_...`),不是
  `VkLayer_...`。填错时报 "Layer was not found"(manifest 被读了但注册不上)。
- **Windows loader 不解析裸 `library_path`** —— 要绝对路径(manifest 所在目录不在 DLL 搜索路径)。
- **`vkGetBufferDeviceAddress` 要用 KHR 别名** `vkGetBufferDeviceAddressKHR` —— 设备是通过
  `VK_KHR_buffer_device_address` 启用的,核心名取不到(返回 NULL)。同样适用于层内的 dispatch 表,
  且**后面不能再用核心名覆盖它**。
- **`vkrt_CreateDevice` 拿到的是 `VkPhysicalDevice`** —— `vkrt_CreateInstance` 必须把所有物理设备
  映射到 instance data,否则报 "no instance data for physical device"。
- **`vkGetDeviceProcAddr` 对扩展函数**(`vkCreateAccelerationStructureKHR` 等)必须显式加载,
  它们不在 `vulkan-1.lib` 里。
- **BVHIR 需要 `scalarBlockLayout`** —— `vk_bvh_defines.h` 的 `TYPE()` 用 `scalar` 限定,
  `vk_aabb` 是两个 `vec3`(第二个从 offset 12 跨界)。`spirv-val` 要加 `--scalar-block-layout`。
  遍历 GLSL 还需要 `GL_EXT_buffer_reference2`(不只是 `buffer_reference`)。
- **`otw_matrix` 是 row-major 3×4**,不是 GLSL 的 `mat3x4`(那是 3 **列** × 4 行,差一个转置)。
  读成 12 个裸 float 再按 row-major 仿射作用。
- **`t` 在仿射变换下不变** —— 原点用 wto 变换、方向只用其线性部分,得到的 `t` 就是世界空间 `t`。
- **根节点句柄不能用固定偏移** —— v2 按输入叶容量定位 box 区，再按实际
  `ir_internal_node_count` 定位最后一个 box。旧版写入 byte 80 的方式已废弃；
  `vk_ir_header.dst_leaf_node_offset`(word 20) 保留编码器字段含义。
