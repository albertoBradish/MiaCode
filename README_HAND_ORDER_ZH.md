# MiaCode HandOrder 2.0.1-handorder（Windows x64）

本包由 @albertoBradish 基于 Team-MiaCode 的 MiaCode v2.0.1 开发，不是官方 MiaCode 发行版。完整解压到可写目录，从包根目录的 `MiaCode.exe` 启动。请保留 `app/`、`assets/` 与 `HandOrderPackage.marker`。

公开版提供人工手序标注：`~l` 为左手蓝色、`~r` 为右手红色、`~m` 为左右任选的灰白色。可以对完整选区批量赋值，也可以拆分星星轨迹的标注区间并编辑换手。Wifi 支持双扇区和显式单手模式。预览、时间轴和视频导出可以显示手序颜色；预览与视频底部显示扩展署名和版本号。实验性的自动手序拆解不在此包中。

| 功能 | 默认快捷键 |
| --- | --- |
| 左手 | Alt+W |
| 右手 | Alt+E |
| 拆分星星手序区间 | Alt+T |
| 左右手均可 | Alt+Y |
| 拆成独立 slide | Ctrl+Alt+5 |

先点击谱面源码并选中完整音符。可在首选项里重绑快捷键。“拆成独立 slide”会改变原生对象，可能影响判定和计分；应用前先检查差异预览。

配置、缓存、会话和日志保存在本包 `runtime/`，默认导出保存在 `exports/`。此包不读取或迁移正式版的偏好、自动恢复和最近文件。请勿只复制 `app/MiaCode.exe` 运行。

本包附带合成谱面的[人工验收清单](acceptance/MANUAL_CHECKLIST_ZH.md)。未勾选项目仍待人工确认；Windows 可执行文件未签名。
