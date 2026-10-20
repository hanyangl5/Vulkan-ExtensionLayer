# Shader module identifier 缓存路径修复

修复前在当前无降级版本上重测 `hlsl_tessellation.test`，仍得到 QualityWarning；预热阶段转换了 4 个 module-backed PSO。修复后，64 个选中的 shader-module-identifier 用例全部通过，且开启了 Validation。

## 原因与修改

应用使用 shader module 预热时，layer 创建的是 GPL libraries 和最终链接结果。此前 identifier-only 请求及 FAIL_ON_PIPELINE_COMPILE_REQUIRED 标志被拦在转换路径外，后续改为原生单体创建。相同 VkPipelineCache 中的 GPL 编译结果没有满足该单体请求。

现在单个 identifier 请求也走相同的 GPL 拆分路径，应用传入的 VkPipelineCache 会传给每次内部创建和最终链接。执行控制标志 FAIL_ON_PIPELINE_COMPILE_REQUIRED 也传给所有内部调用；缓存未命中时保留原生 compile-required 结果，不通过去掉标志来触发编译。

Layer 的 library 缓存按 identifier 的长度和内容构建 shader key，不按指针地址。零长度 identifier 被忽略。Identifier 与 module-backed library 保留不同的缓存身份，因为前者在后续链接时有额外的 compile-control 要求；shader 编译结果的复用由驱动 VkPipelineCache 处理。

失败的 library 构建不再永久留在 layer 缓存中。一次冷缓存 miss 不会阻止应用补充缓存后的重试。带依赖或编译控制的批量请求仍保留原生批量处理。

这一阶段未长期保存应用 SPIR-V，也没有额外启用 shader-module-identifier feature。后续的[跨对象内容复用](content-reuse.md)会缓存每份唯一的已知内容定义。

## 验证

- 修复前：`hlsl_tessellation.test` 为 QualityWarning。
- 修复后：shader-module-identifier 组 64 Pass；成功转换 53 个 PSO，原来为 42。
- 主机回归：验证所有内部调用使用应用 cache、转发 compile-control、identifier 字节/地址的缓存身份、零长度 identifier，以及冷缓存失败后可重试。
- Debug / Release 构建及两组主机测试通过。此前移除转换数量限制的行为保持有效。

原始日志位于仓库 `build/cts-identifier-20261020/`，其中 `before/`、`after/` 保存单例复现和初步验证，`final-full/` 保存最终 Release DLL 的完整选中用例复测。此目录的 `run_full.py` 是复现脚本。

Identifier 本身不承诺每次命中；miss 时返回 compile-required 是允许的。参见 [官方扩展说明](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_shader_module_identifier.html#_should_there_be_stronger_guarantees_on_when_pipeline_compilation_with_identifier_must_succeed)。

## 最终完整选中用例复测

最终 Release DLL 对 56 组、3,042 个用例开启 Validation：2,581 Pass、443 NotSupported、4 Fail、14 InternalError，零 QualityWarning。

4 个 Fail 仍是无 layer 原始基线也失败的多采样 storage-image 用例。14 个 InternalError 在无 GPL layer、开启相同 Validation 的对照中全部复现，VUID 集合一致，没有 layer 独有 VUID。涉及 CTS 的 render-pass / dynamic-rendering 混用、图像布局、空 descriptor set layout 和 legacy vertex input 等检查；原始 3,042 用例基线未启用 Validation，不能直接混用其状态统计。

因此，按相同 Validation 条件对照，未发现本次修复引入的新回归。不能把这次结果表述为全套零错误。

本轮共成功转换 4,292 / 7,216 个已记录 graphics pipeline 创建请求（59.48%，包含原生 GPL 对照组）。其中 identifier 组 64 Pass、53 次转换，原来的警告已消失。

- [逐项结果 CSV](identifier-full-cases.csv)
- [分组转换数量、比例与 Validation 对照 JSON](identifier-full-summary.json)

最终 DLL SHA256：`7b03fbabdd81ace4eb99731ee68af3ddc75fe4cca94cae37aec00fa78b31a500`。
