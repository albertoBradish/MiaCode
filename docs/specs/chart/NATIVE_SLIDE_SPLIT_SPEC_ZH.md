# 拆成独立 slide（原生结构编辑）

状态：2026-10-22 已通过主代理独立验收。完整 MiaCode Release 构建成功，10/10 项相关 CTest 全部通过（5.50 秒）。此命令对应 v0.2 已确认的原生结构拆分，与“拆分手序区间”分开。本轮未启动主 GUI，未使用真实谱面。

## 命令与边界

`hand_order.split_native_slide` 的默认快捷键为 `Ctrl+Alt+5`，在 ShortcutRegistry 中可重绑并持久化。工具栏“手序”、调整菜单与编辑器右键由同一 `HandOrderCommands` 表生成。它与 `hand_order.split_annotation` / `Alt+T` 完全独立。

无选区时禁用。键盘编辑要求源编辑器焦点，设置输入框、只读预览字段和快捷键录制期间不执行谱面编辑；已有平台菜单独占快捷键的机制继续沿用，不重复注册第二套触发。

只转换完整选中链式 slide 中已有的原生段。普通 Tap、Hold、Touch 等非 slide 对象忽略并计数；部分 token 跳过并提示；任一完整 slide 目标为单段、Wifi、非法标注、缺少几何或不能可靠重建时，整批不可应用。不能从一种曲线中任意截取不可表示的中间点，也不拆 Wifi 的展开路径。用户可取消并重新选择可转换的链；精细染色仍可使用手序区间拆分。

## 确定变换与严格复读

核心 `planNativeSlideSplit(text, begin, end, metadata)` 是不读写文件的 chart 纯变换，返回选区 replacement、全文 UTF-16 patch、逐 token before/after 草案、诊断与阻止原因。不可应用的计划保留原 replacement、清空 patches；可靠项的草案仍可用于解释，但不能部分写入。

原生运动区间取解析器最终的相邻 shoot / 最终 end 之差，包含真实时长钳制结果；不把源签名中的名义段时长直接当作实际时长。转换后严格复读并逐段核对形状、位置、实际开始/结束时刻、HS、头/轨迹修饰和手序，不额外添加默认的一拍等待。核对以源 emit 顺序的唯一 `parseOrder` 索引建立对应，不把 parser 按 second/lane 重排后的展示数组位置当作身份；混合同刻、反引号和部分目标因此不会误配。

首段保留原头与原等待。同头多分支的首段仍保留 `*` 与共享头，后续各段通过 `/` 成为无新增可打头的独立 slide，用 `?` / `!` 和显式 `[等待秒##持续秒]` 表示，以原 token 的名义时刻为锚重建解析所得绝对运动开始时刻。原始的头材质、Break、EX、Mine、无头方式和各轨迹修饰须按解析结果保留，不能在组尾文本上盲目统一追加。

原手序进度先映射为原全轨绝对运动时刻，再裁切到每个新段。新段进度 0 继承该时刻的原手别；仅复制严格落在段内的后续节点，并映射为 `u=(t-s)/(e-s)`。精确边界按左闭右开规则继承新手，不输出非法 `u=1`；保留可往返浮点精度，最终由严格 parser 验证。没有标注不会产生新的标注，`m` 继续表示任选一只手。

全谱原生语法错误可能影响时序，阻止转换；选中 slide 的非法手序也阻止。选区外既有非法标注不能被转换吞掉或新增，严格复读按 patch 长度变化映射原 UTF-16 诊断位置，逐项核对原诊断保留。

## 差异预览与应用

命令只准备计划并打开 `NativeSlideSplitDialog`，不会立即编辑。每项显示原始行列、处理状态、阻止原因、可复制的只读 before/after 草案，以及实际轨迹物件数、实际可打头数和增加的独立判定边界。部分选中、非目标忽略和不支持分别计数，不以一个“失败”数字混淆。

预览逐项明示：独立判定边界增加；slide 物件数量按真实复读计数增加；判定起效/提前接触关系、计分、Each、轨迹出现与消失、音效和 Muri 可能改变。拆开的原生 Break 段继续保留 Break，因此 Break 物量与计分也可能改变。出现/消失提示包括无头轨迹的显示变化。

这些是结构编辑影响，不承诺玩法语义等价。轨迹/头数量是本次实际解析的计数；计分、Each、显示、SFX、Muri 的影响明确以“可能”说明，本预览没有逐谱模拟这些结果。不得把运动时间和几何保持误写成判定、音效或 Muri 保持不变。

只有全批可靠时“应用整批拆分”可用。点击应用后重新生成计划，在当前 parser 策略下要求 replacement 与每个 patch 的起止/文本都与预览相同，再通过现有 Scintilla 编辑桥提交一次原生事务。撤销一次恢复原文与标注；取消、关闭或阻止状态不创建编辑或空撤销记录。文档提交继续走 ChartWorkspace 现有刷新链，重新解析时间轴、预览、音效与分析。

## 身份与过期拒绝

EditorController 保存准备时的完整 `ChartWorkspaceSnapshot`、编辑器 scope、难度正文和时序元数据。准备前还核对完整 source 中的当前难度与编辑器正文、时序一致。

Bridge 在文档同步和正文修改时更新身份。应用重新比较完整 source、文件路径、revision、打开 generation、difficulty、origin、scope、正文和 timing；任何不匹配都不写。编辑后回到同一文本仍因 revision 不同而失效，重新打开同一 source 仍因 generation 不同而失效。全局允许负 HS 的 parser 策略也进入准备/应用检查；不能在策略变化后沿用旧计划的严格合法结论。

## 验收入口

运行时语言变化只重新呈现摘要、状态、原因、影响说明与物件计数，不修改 before/after、补丁或准备身份，不使当前可应用结果失效。取消/应用测试等待弹窗退出完成及计划最终状态，不能在退出动画期间假定 `onClosed` 已执行。

- `native_slide_split_spec`：真实 Simai 原生段重建、实际时刻与修饰、共享头/多分支、手序边界/极近节点、tiny clamp、BPM/HS/反引号、UTF-16/CRLF、原子阻止/部分跳过/非目标忽略、选区外既有非法手序；另用真实 SFX 与完整判定回放展示结构拆分确实可能改变结果。
- `hand_order_tools_spec`：Controller 完整身份与 parser 策略拒绝，实际工具栏命令、只读差异、鼠标取消/应用、阻止批次与 Scintilla 一次撤销。使用合成离屏宿主，不启动主 GUI。
- `qml_shortcut_binding_spec`：独立命令/默认绑定、真实 QML 一次触发、重绑、空选区/输入焦点和录制守卫。
- `shortcut_preference_persistence_spec`：第 5 条绑定的修改、规范化写入与重新创建注册表后的恢复。
- `hand_order_edit_spec`、`simai_parser_spec`：原有手序区间编辑与原生解析回归。

本轮最终 10 项为 `native_slide_split_spec`、`hand_order_tools_spec`、`qml_shortcut_binding_spec`、`shortcut_preference_persistence_spec`、`hand_order_edit_spec`、`simai_parser_spec`、`chart_batch_transform_spec`、`muri_spec`、`preview_sfx_timeline_spec`、`hand_order_runtime_paths_spec`，全部通过（5.50 秒）。实际工具栏点击、只读差异、取消、全批阻止、一次应用/撤销，以及完整身份、parser 策略、远端非法标注和可调快捷键均已通过合成验证。

核心对照另外展示了结构编辑的真实差异：Break 链的 SFX 数量由 1 变为 2；同一实际接触在原链达到目标判定、拆后首段却可能提前完成并降判。测试没有把这类变化伪装成玩法等价保证。最终未发现 QML Type/Reference/Binding errors，`git diff --check` 通过。

历史阶段未启动主 GUI、未使用真实谱面，不能据此宣称已验证真实谱面的玩法等价。主 GUI 待人工确认项见[清单](../../qa/hand_order/MANUAL_CHECKLIST_ZH.md)。所有文件、产物与合成测试资料仅写当前隔离工作区。
