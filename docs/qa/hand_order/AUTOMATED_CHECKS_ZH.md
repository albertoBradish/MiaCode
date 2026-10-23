# 手序 v0.2 发布前自动化验证

本页保留首次 v0.2 验证记录；后续修补与基础排布的结果见 [局部求解验收](LOCAL_CONTEXT_FIX_20261022_ZH.md)。

日期：2026-10-22（UTC）。开发使用 GPT-6.1 Sol / XHigh，主代理执行独立源码审查、Release 构建和验证。用户要求尽量不用桌面自动操作，主 GUI 中尚需人工观察的项目见 [手动清单](MANUAL_CHECKLIST_ZH.md)。复选框没有被代勾。

## 本轮修复

- Qt 6.11 的编译翻译目录中，新增具名 context 的 174 条手序文案未按 ID 存储，真实 GUI 因而显示 `hand_order.title` 等原始键名。三语消息现归入 ID 查询使用的空 context，保留全部 1355 条消息及内容。测试直接加载与程序同源的内嵌 QM，逐 ID 验证三语翻译，并检查实际 QML 工具栏标题和提示。
- 自动分析摘要、行状态和编辑反馈使用既有 `LocalizedText` 延迟翻译；上层语言变化通知只刷新显示，不改变分析 sequence、参数、结果身份、正文或拆分补丁。真实翻译目录切换测试覆盖完成及运行中的分析、原生拆分预览和可应用状态。
- 离屏交互测试等待弹层和模型的最终状态，不再依赖短于退出动画的固定延时。仍严格要求一次编辑、正确标注和一次撤销恢复。
- Windows 启动脚本兼容 PowerShell 5.1 的参数 API，按 Windows CRT 规则引用参数，直接创建子进程而不经 shell。真实 Python argv 回归覆盖空参数、空格、引号、尾反斜杠、Unicode、制表符和 shell 特殊字符。
- 打包每次执行受限并发的增量构建检查，不再以“版本相同”推断源码最新；新包带本目录合成谱和清单，并记录该构建的 Git 身份。

## 已执行结果

完整 `MiaCode` / `MiaCodeLauncher` Release 构建成功；21/21 相关 CTest 通过，0 失败，17.17 秒。手序控件测试在修复后连续三次通过（9.28 秒）。

21 项为：`simai_parser_spec`、`chart_batch_transform_spec`、`muri_spec`、`simai_completion_catalog_spec`、`video_export_hud_font_snapshot_spec`、`ui_text_locale_spec`、`hand_order_edit_spec`、`native_slide_split_spec`、`basic_hand_order_spec`、`hand_order_touch_judge_spec`、`hand_order_full_replay_spec`、`basic_hand_order_replay_spec`、`wifi_hand_order_render_spec`、`qml_shortcut_binding_spec`、`hand_order_tools_spec`、`hand_order_runtime_paths_spec`、`shortcut_preference_persistence_spec`、`hand_order_analysis_service_spec`、`preview_head_layer_spec`、`preview_realtime_object_hot_path_spec`、`preview_sfx_timeline_spec`。

核心矩阵包含 608 个原生 slide 正例及 608 个截断负例、8 个 Wifi 方向、混合链与共享动作；结构拆分测试保留严格几何、时间、修饰、手序复读，以及音效和判定可能改变的反例。

`HandOrderPackagingScriptSpec.ps1` 在 PowerShell 5.1 和 7 均由主代理复验通过。5.1 的本机策略默认拒绝脚本，验证只对一次性测试子进程使用了进程级执行策略参数，没有改变系统级策略；普通使用推荐包根目录的原生启动器。

工作区证据：`logs/hand_order_release_precommit_build.log`、`hand_order_release_precommit_ctest.log`、`hand_order_release_precommit_verbose.log`、`gui_release_tools_repeat_final.log`、`packaging_script_ps51_final.log`、`packaging_script_ps7_final.log`。

## 证明范围

曾启动隔离版真实主窗口并加载合成谱面，发现了翻译问题；之后按用户要求停止桌面自动操作，使用源码、真实翻译资源、独立测试宿主及非 GUI 进程检查。**没有完成整个主 GUI 的端到端人工验收**，尤其是实际编辑焦点、进度浮层、主题/DPI、小窗口布局、视觉和重开体验，仍需按清单确认。

只使用合成谱面与独立运行目录，不读取或迁移正式版偏好、最近文件、恢复内容或真实谱面。自动算法分数不是正确率或人体可玩性证明，原生结构拆分也不保证玩法等价。

最终包的 commit/dirty/channel 见包根目录 README 的 Build identity；程序和归档哈希、包内 worker 运行验证由工作区发布记录保存，不写回源码造成构建身份循环。
