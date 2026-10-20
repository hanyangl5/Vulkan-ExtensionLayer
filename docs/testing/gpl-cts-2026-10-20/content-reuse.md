# 相同内容跨对象复用（2026-10-20）

已让内容相同、句柄不同的 shader module、descriptor set layout、pipeline layout，以及定义完全相同的 render pass 复用 GPL library。Sampler 的基础创建状态也纳入 immutable-sampler 身份。应用仍分别拥有原始 Vulkan 对象；没有返回或复用应用句柄。

对象创建成功后序列化完整的已知字段和数组，用完整字符串比较分配设备内短 ID。对象销毁只删除句柄映射；相同内容重建继续取得相同 ID，句柄被用于不同内容则取得不同 ID。未知扩展不合并，保留逐生命周期隔离。

## 效果

同一 56 组、3,042 用例的 Validation 复测中，转换数仍为 4,292；新建 PR/FS library 从 8,502 降到 2,832，减少 **66.69%**。平均新建数从 1.981 降到 0.660 个/PSO。

此统计只包含 PR/FS 新建库，含合并的 VI+PR；不包含独立 VI/FO，也不是唯一 shader 数或精确命中率。没有用整组墙钟时间推算编译加速比例。

| 测试组 | 成功转换 PSO（前后相同） | PR/FS 库：修改前 | 修改后 | 减少 |
|---|---:|---:|---:|---:|
| cache | 575 | 1150 | 8 | 99.30% |
| bind_point | 32 | 64 | 4 | 93.75% |
| color_write_enable | 77 | 154 | 2 | 98.70% |
| input_assembly | 220 | 439 | 31 | 92.94% |
| extended_dynamic_state | 686 | 1372 | 276 | 79.88% |
| shader_module_identifier | 53 | 97 | 74 | 23.71% |

## 正确性与边界

- Shader 按创建 flags 和完整 SPIR-V 内容区分，inline SPIR-V 与等内容 module 共用身份；entry point、stage、specialization 等仍在 library key 中。Identifier 创建的 library 继续保留独立类别和 compile-control 语义。
- Descriptor set layout 包含 binding、类型、数量、stage、binding flags、mutable 类型列表及 immutable sampler 身份；pipeline layout 包含 set 顺序、layout flags 和 push-constant ranges。
- Render pass 仅复用同一创建 API 下序列化定义相同的对象，不扩大为所有 Vulkan 兼容 render pass。已知的 multiview、input aspect、stencil layout、depth/stencil resolve、shading-rate、sync2 结构按值处理；未知链不合并。
- 带扩展链的 sampler 暂按对象生命周期区分；基础 sampler 内容相同则可跨句柄识别。
- 扩大复用暴露并修复了 FS/FO 多采样状态问题：动态/被忽略字段在实际 library create info 中同步规范化；静态 NULL sample mask 与显式全 1 mask 不再共用哨兵编码。
- 内容表保留每种唯一已知定义的一份副本，包括 SPIR-V，直到 device 销毁。这换取跨对象销毁重建后的精确识别，也会占用额外 CPU 内存；pipeline key 本身只存短 ID。

## 验证

- Debug / Release 构建通过，warnings-as-errors 开启；两组主机测试通过。
- 主机测试覆盖跨 shader/layout/render-pass 句柄的 library 复用、immutable sampler、binding/range 顺序、inline module、句柄重用后内容变化、未知扩展隔离、创建失败，以及 MSAA 状态规范化和 mask 编码。
- 3,042 个用例逐项状态与修改前一致：2,581 Pass、443 NotSupported、4 Fail、14 InternalError，零 QualityWarning。
- 4 个 Fail 和 14 个 InternalError 都是此前已确认的基线问题；没有新增状态回归或 VUID。Identifier 组保持 64/64 Pass。

[分组对比 CSV](content-reuse-groups.csv) · [逐用例对比 CSV](content-reuse-cases.csv) · [完整机器可读记录](content-reuse-summary.json)

原始日志及最终 DLL 位于仓库 `build/cts-content-reuse-20261020/final-full-v3/`；`run.py final-full-v3` 为复现脚本，`final-source/` 保存本轮源文件副本。`final-full/`、`final-full-v2/` 是诊断中间版本，不作为最终结果。

最终 DLL SHA256：`29e469903a4e717063a380c554f98cc9afa29017dc0573496de3563e077e1ec9`。
