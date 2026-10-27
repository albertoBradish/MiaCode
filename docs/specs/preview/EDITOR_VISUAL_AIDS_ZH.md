# 编辑定位、选区试听与预览辅助

手序公开版移植 Better Miacode 的编辑与画面辅助功能，保留手序版身份、版本及运行目录。
本文描述已接入代码的行为；构建、规格与 GUI 验收结果应另行记录。

- 单击源码行号先暂停，再定位至解析器记录的行首时间；空行、注释行可定位，书签边栏保持原操作。
- 画面选项可显示小节线，默认关闭。完整灰白圆环按 tap 基础流速进场，到拍位判定圆后消失。
  首选项支持谱面 `clock_count`（无效时 4 拍）、自定义正整数拍数和源码行首（同时间去重）；BPM 改变不重置累计拍位。
- 源码选区按各音符的实际区间匹配。部分 token 选中完整音符，同拍音符独立，链式 slide 整体；手序后缀属于对应音符。
  时间轴与预览使用淡黄色细轮廓标出头、身体、轨迹及移动星星。此状态不写入谱面，也不进入导出。
- 编辑器右键菜单与调整菜单共用“播放选区”命令，无音符时禁用。起播时固定范围，保留进场预卷及原谱上下文，
  完整长音符和效果结束后暂停。更改选择不改变边界；修改文本、换谱、手动 seek 或停止会取消范围。
- 画面选项可显示判定骨骼，默认关闭。按真实判定区中心画青白细直线，保留候选组和 Wifi 分路；
  随轨迹出现、随判定消退。无需启用无理分析或自动手序；缺少可靠判定数据时不推测路径。

小节线与判定骨骼使用共享场景和显式帧时间，导出可独立开关，默认继承预览。
圆环按实际帧像素抗锯齿；预览轮廓约为一屏幕像素，时间轴轮廓约为一 DIP，并减少封闭内孔与密集箭头的遮挡。

## 验证入口

启用 `MIACODE_BUILD_DEV_TOOLS=ON`，使用 Release、并发最多 4。聚焦目标：

`qml_scintilla_editor_spec`、`editor_sync_controller_spec`、`playback_coordinator_spec`、
`timeline_model_spec`、`preview_head_layer_spec`、`preview_visual_aids_spec`、`video_export_visual_aids_spec`。

手序兼容检查使用 `timeline_hand_order_skin_spec` 与 `hand_order_runtime_paths_spec`。
仅用合成谱面检查反向选择、中文注释、手序后缀、链式/Wifi、变速、偏移、范围取消、预览与导出同帧。
独立离屏工具 `miacode_measure_ring_render --guide <assets/noteguide/Normal.png> --output <工作区输出目录>`
可比较原版参考线与小节线。
