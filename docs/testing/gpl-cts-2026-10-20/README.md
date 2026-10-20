# GPL CTS 逐项测试记录（2026-10-20）

环境：NVIDIA RTX 3080，驱动 596.49，Clang 20.1.3 Release。CTS 提交 `d18e4c68543103371715845cd914c71e5859963e`；对应 layer 修复提交 `8d0d4ef`。测试未修改 CTS 源码。

共 56 组、3,042 个用例：2,594 Pass、443 NotSupported、4 Fail、1 QualityWarning。26 个此前由 layer 引入的失败已修复，无新增失败。4 个 Fail 在无 layer 的基线也失败；QualityWarning 为 shader module identifier 的缓存行为差异。

记录到 **3,491** 个成功转换请求、**2,935** 个直通请求，共 **6,426** 个 graphics pipeline 创建请求；**已记录请求的转换比例为 54.33%**。另创建 6,913 个 shader libraries（PR/FS 构建计数，含合并 VI+PR；不含独立 VI/FO）。

## 统计口径

- 转换比例 = 成功转换请求数 /（成功转换请求数 + 已记录直通请求数）。批量直通按 `count` 累加 PSO 数，不按 API 调用次数计数。失败后重试、重复创建也计数；不是唯一 PSO 数。
- `cache`、`depth`、`extended_dynamic_state` 触发自动降级。降级后的快速直通没有逐条日志，分母不完整，表中以 `*` 标注；全局 54.33% 也仅代表已记录请求，不能当作完整转换率。
- Compute pipeline 不属于统计对象。NotSupported 和未创建 graphics pipeline 的测试不会贡献分母；分母为零时显示 N/A。
- 原生 GPL 对照组包含本来就由应用创建的 library/link 请求，按兼容性直通处理。它们与单体 PSO 转换的覆盖率应分别理解。
- 现有 QPA 用例结果和 layer 日志分别写入不同文件，没有 case ID 关联。可精确提供每个测试组的转换数量；不能可靠分配到单个用例。`cases.csv` 的逐用例转换字段留空，组级字段明确以 `group_` 为前缀。
- 表中耗时是整组测试进程墙钟时间，包含 shader 编译、执行及日志开销，不是 PSO 编译耗时。

## 分开观察单体 PSO 与原生 GPL

| 范围 | 组数 | 用例 | 转换 PSO | 已记录请求 | 已记录转换比例 |
|---|---:|---:|---:|---:|---:|
| monolithic/API | 53 | 2856 | 3450 | 3737 | 92.32% |
| native GPL control | 3 | 186 | 41 | 2689 | 1.52% |

单体/API 范围包含上述 3 个降级组，92.32% 同样不是完整转换率；原生 GPL 组的低比例反映 library/link 请求按兼容性直通，不等同于转换失败。

## 各组结果

P = Pass，NS = NotSupported，F = Fail，Q = QualityWarning；“直通”“请求”“比例”均按已记录日志统计。

| 测试组 | 用例 | P | NS | F | Q | 转换 PSO | 直通 PSO | 请求总数 | 转换比例 | shader libraries | 整组秒 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| api_pipeline | 11 | 11 | 0 | 0 | 0 | 4 | 0 | 4 | 100.00% | 7 | 3.237 |
| dynamic_state | 61 | 61 | 0 | 0 | 0 | 83 | 0 | 83 | 100.00% | 166 | 3.021 |
| attachment_feedback_loop_layout | 48 | 36 | 12 | 0 | 0 | 37 | 0 | 37 | 100.00% | 74 | 3.772 |
| bind_buffers_2 | 48 | 48 | 0 | 0 | 0 | 48 | 0 | 48 | 100.00% | 96 | 4.816 |
| bind_point | 48 | 48 | 0 | 0 | 0 | 32 | 0 | 32 | 100.00% | 64 | 3.117 |
| blend | 96 | 66 | 30 | 0 | 0 | 207 | 0 | 207 | 100.00% | 414 | 3.572 |
| blend_operation_advanced | 48 | 35 | 13 | 0 | 0 | 39 | 0 | 39 | 100.00% | 78 | 2.309 |
| cache | 128 | 128 | 0 | 0 | 0 | 256 | 0 | 256 | 100.00%* | 512 | 13.636 |
| color_write_enable | 48 | 48 | 0 | 0 | 0 | 77 | 0 | 77 | 100.00% | 154 | 3.241 |
| color_write_enable_maxa | 24 | 24 | 0 | 0 | 0 | 96 | 0 | 96 | 100.00% | 192 | 10.491 |
| creation_cache_control | 18 | 18 | 0 | 0 | 0 | 3 | 19 | 22 | 13.64% | 3 | 2.266 |
| creation_feedback | 13 | 13 | 0 | 0 | 0 | 0 | 30 | 30 | 0.00% | 0 | 2.413 |
| depth | 96 | 79 | 17 | 0 | 0 | 256 | 0 | 256 | 100.00%* | 510 | 2.661 |
| depth_range_unrestricted | 48 | 48 | 0 | 0 | 0 | 60 | 0 | 60 | 100.00% | 120 | 2.287 |
| derivative | 3 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | N/A | 0 | 1.717 |
| descriptor_limits | 48 | 48 | 0 | 0 | 0 | 24 | 0 | 24 | 100.00% | 48 | 2.959 |
| dynamic_control_points | 3 | 3 | 0 | 0 | 0 | 6 | 0 | 6 | 100.00% | 12 | 2.692 |
| dynamic_offset | 48 | 48 | 0 | 0 | 0 | 38 | 0 | 38 | 100.00% | 76 | 4.674 |
| dynamic_vertex_attribute | 1 | 1 | 0 | 0 | 0 | 2 | 0 | 2 | 100.00% | 4 | 2.402 |
| early_destroy | 9 | 9 | 0 | 0 | 0 | 11 | 0 | 11 | 100.00% | 11 | 2.703 |
| empty_fs | 8 | 8 | 0 | 0 | 0 | 8 | 0 | 8 | 100.00% | 16 | 2.616 |
| executable_properties | 16 | 16 | 0 | 0 | 0 | 9 | 15 | 24 | 37.50% | 18 | 2.461 |
| extended_dynamic_state | 384 | 371 | 13 | 0 | 0 | 265 | 2 | 267 | 99.25%* | 530 | 15.406 |
| framebuffer_attachment | 48 | 48 | 0 | 0 | 0 | 148 | 0 | 148 | 100.00% | 296 | 2.365 |
| input_assembly | 198 | 198 | 0 | 0 | 0 | 220 | 0 | 220 | 100.00% | 439 | 14.438 |
| input_attribute_offset | 48 | 48 | 0 | 0 | 0 | 48 | 0 | 48 | 100.00% | 96 | 2.478 |
| interface_matching | 96 | 83 | 13 | 0 | 0 | 83 | 0 | 83 | 100.00% | 166 | 3.811 |
| logic_op | 48 | 30 | 18 | 0 | 0 | 30 | 0 | 30 | 100.00% | 60 | 1.953 |
| logic_op_na_formats | 48 | 23 | 25 | 0 | 0 | 23 | 0 | 23 | 100.00% | 46 | 1.815 |
| matched_attachments | 2 | 2 | 0 | 0 | 0 | 2 | 0 | 2 | 100.00% | 4 | 1.716 |
| max_varyings | 6 | 3 | 3 | 0 | 0 | 3 | 0 | 3 | 100.00% | 6 | 2.489 |
| misc | 13 | 13 | 0 | 0 | 0 | 13 | 0 | 13 | 100.00% | 26 | 2.933 |
| multisample | 96 | 54 | 41 | 1 | 0 | 137 | 16 | 153 | 89.54% | 274 | 3.690 |
| multisample_interpolation | 48 | 25 | 23 | 0 | 0 | 25 | 0 | 25 | 100.00% | 50 | 2.280 |
| multisample_shader_builtin | 48 | 24 | 21 | 3 | 0 | 114 | 0 | 114 | 100.00% | 228 | 3.292 |
| multisample_with_fragment_shading_rate | 48 | 21 | 27 | 0 | 0 | 55 | 0 | 55 | 100.00% | 110 | 3.320 |
| no_position | 48 | 48 | 0 | 0 | 0 | 0 | 48 | 48 | 0.00% | 0 | 4.287 |
| pipeline_binary | 34 | 30 | 4 | 0 | 0 | 1 | 43 | 44 | 2.27% | 2 | 4.232 |
| pipeline_cache | 14 | 14 | 0 | 0 | 0 | 16 | 0 | 16 | 100.00% | 32 | 3.197 |
| primitive_restart_index | 48 | 0 | 48 | 0 | 0 | 0 | 0 | 0 | N/A | 0 | 2.414 |
| push_constant | 48 | 44 | 4 | 0 | 0 | 50 | 0 | 50 | 100.00% | 100 | 2.436 |
| push_descriptor | 48 | 48 | 0 | 0 | 0 | 27 | 0 | 27 | 100.00% | 54 | 11.068 |
| shader_module_identifier | 64 | 63 | 0 | 0 | 1 | 42 | 21 | 63 | 66.67% | 78 | 8.006 |
| shader_stencil_export | 6 | 0 | 6 | 0 | 0 | 0 | 0 | 0 | N/A | 0 | 2.010 |
| spec_constant | 64 | 64 | 0 | 0 | 0 | 32 | 0 | 32 | 100.00% | 64 | 3.720 |
| stencil | 96 | 67 | 29 | 0 | 0 | 220 | 0 | 220 | 100.00% | 440 | 3.673 |
| vertex_input | 96 | 48 | 48 | 0 | 0 | 52 | 0 | 52 | 100.00% | 103 | 4.142 |
| native_fast_gpl_dynamic | 61 | 61 | 0 | 0 | 0 | 17 | 310 | 327 | 5.20% | 34 | 3.270 |
| native_gpl_graphics_library | 64 | 64 | 0 | 0 | 0 | 7 | 2028 | 2035 | 0.34% | 14 | 13.529 |
| native_lto_gpl_dynamic | 61 | 61 | 0 | 0 | 0 | 17 | 310 | 327 | 5.20% | 34 | 2.989 |
| dynamic_rendering | 64 | 43 | 21 | 0 | 0 | 73 | 82 | 155 | 47.10% | 131 | 3.864 |
| renderpass2 | 64 | 39 | 25 | 0 | 0 | 46 | 0 | 46 | 100.00% | 92 | 2.366 |
| multiview_dynamic_rendering | 32 | 32 | 0 | 0 | 0 | 168 | 0 | 168 | 100.00% | 308 | 15.054 |
| multiview_renderpass2 | 32 | 32 | 0 | 0 | 0 | 167 | 0 | 167 | 100.00% | 334 | 2.124 |
| mesh_ext | 64 | 63 | 1 | 0 | 0 | 63 | 11 | 74 | 85.14% | 125 | 2.535 |
| mesh_nv | 32 | 31 | 1 | 0 | 0 | 31 | 0 | 31 | 100.00% | 62 | 3.162 |

## 直通原因（已记录请求）

| 原因 | PSO 数 |
|---|---:|
| unsupported top-level pNext | 2762 |
| derivative flag | 85 |
| dependency/control batch | 31 |
| unhandled create flag | 28 |
| capture flag | 21 |
| unsupported shader stage/code | 6 |
| unsupported rasterization pNext | 2 |

`conversion/link failed` 为 0。直通原因不代表用例失败；例如 derivative、capture 和应用原生 GPL library/link 请求需要保留原始创建行为。

## 剩余非通过用例

| 用例 | 无 layer | 修复后 | 结果说明 |
|---|---|---|---|
| `dEQP-VK.pipeline.monolithic.multisample.storage_image.64x64_4.r8g8b8a8_unorm.samples_8` | Fail | Fail | Rendered images are not correct |
| `dEQP-VK.pipeline.monolithic.multisample_shader_builtin.image_write_sample.2_samples` | Fail | Fail | Invalid result at pixel (0, 0, 0); check error mask for more details |
| `dEQP-VK.pipeline.monolithic.multisample_shader_builtin.image_write_sample.4_samples` | Fail | Fail | Invalid result at pixel (0, 0, 0); check error mask for more details |
| `dEQP-VK.pipeline.monolithic.multisample_shader_builtin.image_write_sample.8_samples` | Fail | Fail | Invalid result at pixel (0, 0, 0); check error mask for more details |
| `dEQP-VK.pipeline.monolithic.shader_module_identifier.hlsl_tessellation.test` | Pass | QualityWarning | PipelineCompileRequiredError received despite using pipeline cache |

## 修复的 26 个回归

| 用例 | 无 layer | 修复前 | 修复后 |
|---|---|---|---|
| `dEQP-VK.dynamic_state.monolithic.line_width.dyna_static.list10_strip9` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.monolithic.line_width.dyna_static.list6_list5` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.monolithic.line_width.dyna_static.strip14_strip13` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.monolithic.line_width.dyna_static.strip2_list1` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.monolithic.line_width.static_dyna.list3_strip4` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.monolithic.line_width.static_dyna.list7_list8` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.monolithic.line_width.static_dyna.strip11_list12` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.monolithic.line_width.static_dyna.strip15_strip16` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.fast_linked_library.line_width.dyna_static.list10_strip9` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.fast_linked_library.line_width.dyna_static.list6_list5` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.fast_linked_library.line_width.dyna_static.strip14_strip13` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.fast_linked_library.line_width.dyna_static.strip2_list1` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.fast_linked_library.line_width.static_dyna.list3_strip4` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.fast_linked_library.line_width.static_dyna.list7_list8` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.fast_linked_library.line_width.static_dyna.strip11_list12` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.fast_linked_library.line_width.static_dyna.strip15_strip16` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.pipeline_library.line_width.dyna_static.list10_strip9` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.pipeline_library.line_width.dyna_static.list6_list5` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.pipeline_library.line_width.dyna_static.strip14_strip13` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.pipeline_library.line_width.dyna_static.strip2_list1` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.pipeline_library.line_width.static_dyna.list3_strip4` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.pipeline_library.line_width.static_dyna.list7_list8` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.pipeline_library.line_width.static_dyna.strip11_list12` | Pass | Fail | Pass |
| `dEQP-VK.dynamic_state.pipeline_library.line_width.static_dyna.strip15_strip16` | Pass | Fail | Pass |
| `dEQP-VK.renderpasses.dynamic_rendering.primary_cmd_buff.m10_feedback_loop.r8g8b8a8_unorm_samples_4_loop_NY_general_layout` | Pass | Fail | Pass |
| `dEQP-VK.renderpasses.dynamic_rendering.primary_cmd_buff.m10_feedback_loop.r8g8b8a8_unorm_samples_4_loop_YN_sample_2` | Pass | Fail | Pass |

## 原始记录与复现

- [56 组 CSV](groups.csv)：结果、转换数、已记录直通数、比例、原因和组级耗时。
- [3,042 个用例 CSV](cases.csv)：无 layer / 修复前 / 修复后结果、用例耗时和关联组。
- [机器可读汇总](summary.json)。
- 本地原始日志：仓库 `build/cts-fix-20261020/regression-suite/`，每组保留 `.layer.json`、`.layer.qpa`、`.layer.layer.log` 和 `.layer.stdout.txt`；计划及无 layer 基线位于 `build/cts-pso-20261020/`。这些构建产物未纳入 Git。
- 完整 3,042 用例数据来自修复后、最终格式化前的 Release DLL（SHA256 `2305aac3d57be17ba55619020f03dcf8d6dd378e77d59e7723170a050ad8b354`）。最终格式化后的 DLL（SHA256 `5bd886191b73e7237bc69b1b938d8cb8fda56457a9f635336a45e44903b216e3`）另完成 26/26 Validation 回归；进一步 Validation 抽测为 133 Pass、8 NotSupported。两轮结果没有合并累加到本表。
- 本报告从已有日志生成，没有重新运行测试。若需要每个用例的完整转换率，需增加 case 关联及快速直通计数后重新采集。

## 取消自动降级后的复测

此节是移除 256 次后的共享率降级判断后，开启 Validation 的独立复测；上面的原始 3,042 用例表及 CSV 保持不变。

| 测试组 | 用例结果 | 原成功转换数 | 新成功转换数 | 已记录转换比例 |
|---|---|---:|---:|---:|
| cache | 128 Pass | 256 | 575 | 100.00% |
| depth | 79 Pass / 17 NotSupported | 256 | 303 | 100.00% |
| extended_dynamic_state | 371 Pass / 13 NotSupported | 265 | 686 | 99.71%（686/688） |

合计 608 个用例：578 Pass、30 NotSupported、零 Fail，结果与无 layer 基线一致。
成功转换 1,564 个 PSO；仅 2 次因不支持的 rasterization pNext 直通，合计 1,564/1,566（99.87%）。
没有因转换数量或共享率停止转换。比例只针对这三个复测组，不代表完整 56 组的新结果。

[机器可读复测结果](no-gate-summary.json)。原始日志位于仓库 `build/cts-no-gate-20261020/tests/`，复现脚本为同目录上一级的 `run.py`。

## Identifier 缓存路径修复后的复测

后续已让单个 identifier 请求保持 GPL 创建路径，并保留 compile-required 控制。原来的 `hlsl_tessellation` QualityWarning 消失，identifier 组 64/64 Pass。完整 3,042 用例的 Validation 复测、原生对照和最新转换数见 [修复说明与结果](identifier-cache-fix.md)。本页早期快照的表与 CSV 保持不变。

## 相同内容跨对象复用后的复测

后续加入内容身份缓存后，相同 4,292 个 PSO 的 PR/FS 新建库数从 8,502 降至 2,832，减少 66.69%；3,042 个用例逐项结果不变。见 [实现范围、内存代价及分组对比](content-reuse.md)。
