# MiaCode HandOrder

基于 [MiaCode v2.0.1](https://github.com/Team-MiaCode/MiaCode/releases/tag/2.0.1) 的独立手序标注扩展，由 [@albertoBradish](https://github.com/albertoBradish) 开发。此项目不是 Team-MiaCode 的官方发行版。

[English](README_EN.md) · [下载公开版](https://github.com/albertoBradish/MiaCode/releases) · [上游 MiaCode](https://github.com/Team-MiaCode/MiaCode)

## 公开版功能

- 在谱面源码中人工标注 `~l`（左手）、`~r`（右手）、`~m`（任选一只手）；`~m` 不表示两手同时使用。
- 批量给完整选中音符赋手别；拆分星星轨迹的手序区间，并编辑换手节点。Wifi 支持双扇区和显式单手模式。
- 预览、时间轴及视频导出显示手序颜色；视频与预览底部显示扩展署名和版本号。
- 便携包使用独立的配置、缓存、会话和日志目录，不读取或迁移官方版个人数据。

实验性的自动手序拆解不包含在此公开版本中。手序是作者的人工标注，观看者仍需自行判断打法。

## 快捷键

先聚焦谱面源码并选中完整音符；可在首选项中重绑。

| 功能 | 默认快捷键 |
| --- | --- |
| 左手 | Alt+W |
| 右手 | Alt+E |
| 拆分星星手序区间 | Alt+T |
| 左右手均可 | Alt+Y |
| 拆成独立 slide | Ctrl+Alt+5 |

“拆分星星手序区间”只修改标注。“拆成独立 slide”会改变原生谱面对象，可能影响判定、物量和计分，请先查看应用前的差异预览。

## 下载与使用

从本 fork 的 [Releases](https://github.com/albertoBradish/MiaCode/releases) 下载 Windows x64 便携包，完整解压到可写目录，从包根目录的 `MiaCode.exe` 启动。保留 `app/`、`assets/` 和 `HandOrderPackage.marker`。配置保存在该目录的 `runtime/`，默认导出位于 `exports/`。

手序语法见 [规格](docs/specs/chart/HAND_ORDER_ANNOTATION_SPEC_ZH.md)，人工编辑见 [工具说明](docs/specs/chart/HAND_ORDER_MANUAL_TOOLS_SPEC_ZH.md)。源码构建需要 Qt 6.10+、CMake、MSVC 和 FFmpeg 开发库；Windows 工具链入口见 [脚本说明](scripts/README.md)。

## 来源与许可

本项目继承 MiaCode 的 GPLv3 许可，详见 [LICENSE](LICENSE)。原版功能与美术来自 Team-MiaCode；扩展代码、手序素材与发布包维护由 @albertoBradish 负责。上游问题请使用[上游仓库](https://github.com/Team-MiaCode/MiaCode)，本扩展问题请在[此 fork](https://github.com/albertoBradish/MiaCode/issues)反馈。
