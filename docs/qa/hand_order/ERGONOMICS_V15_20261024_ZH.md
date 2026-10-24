# 动作连贯性与 DX 验收（算法 v15）

本轮依据用户给出的 FFT 局部配置和截图调整通用动作模型。真实谱面与音频只保存在工作区
`exports/`，不提交、不装入程序包。截图用于说明期望手法，其中既有颜色不作为自动推断的锁定答案。

## 判定资料与保持时间

官方 [maimai DX 基本玩法](https://maimai.sega.com/play/howto/) 要求 HOLD 按至结束，但没有公布精确免检毫秒数。
开发参考另核对固定提交 `f2427c2e6e2a3ef3adec1408e44b136836b3985f` 的 MajdataPlay：

- [NoteDrop 常量](https://github.com/TeamMajdata/MajdataPlay/blob/f2427c2e6e2a3ef3adec1408e44b136836b3985f/Assets/Scripts/Scenes/Game/NoteBehaviours/NoteDrop.cs)：普通 HOLD 头 6/60 秒、尾 12/60 秒；TOUCH HOLD 头 15/60 秒、尾 12/60 秒。
- [HoldDrop](https://github.com/TeamMajdata/MajdataPlay/blob/f2427c2e6e2a3ef3adec1408e44b136836b3985f/Assets/Scripts/Scenes/Game/NoteBehaviours/HoldDrop.cs)：短于头尾免检总长时没有有效按压段；较长时检查中段，EX/BREAK 不跳过该段。
- [TouchHoldDrop](https://github.com/TeamMajdata/MajdataPlay/blob/f2427c2e6e2a3ef3adec1408e44b136836b3985f/Assets/Scripts/Scenes/Game/NoteBehaviours/TouchHoldDrop.cs)：使用 TOUCH HOLD 自己的头部长度。

因此当前工程模型分别在约 300ms/450ms 判断短 HOLD/TOUCH HOLD 是否可提供轻触模板。
这两条是社区实现参考，不能当作官方内部规格。本轮没有改变既有完整判定 profile；其释放消抖与另一模拟器
并非逐项相同。候选不利用消抖制造中段空按，非空按压段必须连续覆盖，释放另加两判定 tick 保护。
完整保持仍是第一模板，只有确实帮助后续动作时才展开释放分支；评分增加提前松手代价。

## 可复现的聚焦验证

`hand_order_ergonomics_spec` 对用户的六段具体配置和一个 DX TOUCH 变体，测试综合、硬抗、分页、拆谱，
以及镜像；六段按键配置再加 EX/BREAK 变体，共 104 个组合。要求原生正文逐字不变、关键手别正确、每行完整回放通过：

- 三扫的 1→8→7H 由左手连续处理，镜像及 8→1→2H 对称。
- 同头弧线与直线按两分支的空间位置分手，避免用压力较低掩盖手臂倒置。
- 一笔画起点前的 4 及对应 A4 TOUCH 交给左手，保持准备滑条的右手；镜像相反。
- 4/8 双押允许浅出张并保留邻接手，避免从 5 飞到 8 的无谓整屏位移。
- 连续同头 2 起滑均由右手处理，后面的 1→8→7→6 连扫由左手处理；镜像相反。

`hand_order_hold_candidates_spec` 单独检查普通与 TOUCH HOLD 的短/长边界，包含 EX、BREAK HOLD；有效中段未覆盖
时不能通过完整回放。已有 Touchgroup、WiFi、实际输入时序、占手、绕手、局部窗口和异步失效验证继续运行。
另修复完整八键快圈恰低于默认部分扫阈值时被拆散的边界，156 BPM 十六分圈及镜像保留单手。

新前臂约束以屏幕下方两个固定肩点向实际手掌中心连线，检查持续动作期间交叉；同头分支还比较轨迹的左右展开。
它们是工程启发式，不能证明所有人体姿态是否可行。实际占手与完整判定仍独立检查。

本机 Release 构建通过，并发上限 4；上述 12 项聚焦 Spec 全部通过（其中新增动作组合 104 个，另含已标注浅出张保持）。
构建与 CTest 日志位于工作区 `logs/ergonomic_v15_final_build.log`、`logs/ergonomic_v15_final_ctest.log`。

## 人工语料与 FFT 留档口径

五份已标注语料均重新计算全部目标难度，清除旧手序作为答案先验；原生解析逐 marker 核对类型、几何、时间和修饰符。
对照保留四预设输出、逐 marker 差异和未验证行数。不存在一组参数在所有曲目上都最接近人工稿；人工差异也不等于
物理错误数。本轮维持四组预设数值，优先满足用户明确指出的动作约束，不根据某张谱名选择隐藏参数。

工作区 `exports/ergonomic_v15/corpus_final/summary.json` 保存完整对照。FFT Master/Re:Master 的基础排布、进一步 Sticking、
输入/输出哈希、参数和视频留在 `exports/FFT_version_comparison/v15_20261024/`；最终运行根目录启动器的程序身份及
视频检查结果以该目录最终 manifest 为准。源码文档不把准备中的视频称为已通过人工验收。

下表为同一原生 marker 口径的不同手别数量，分支共用头仍按原生 marker 计数；不能与旧文档的源 token 行差异直接混算。
每组预算 3 亿，关闭额外反事实搜索，未决补色开启。四组分别按各自参数重新搜索，不锁定人工答案。

| 曲目 | 综合 | 硬抗 | 分页 | 拆谱 | 每组已验证/源行 | 未验证行 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Deep in Abyss | 25 | 23 | 25 | 23 | 379/379 | 0 |
| プリズム△▽リズム | 87 | 85 | 85 | 89 | 568/568 | 0 |
| DATAERR0R | 94 | 120 | 76 | 76 | 628/628 | 0 |
| Destined Marionette | 109 | 110 | 102 | 112 | 888/894 | 6 |
| Synthesis | 120 | 126 | 100 | 106 | 985/1025 | 40 |

分页在 DATAERR0R、Synthesis、Destined Marionette 更接近这几份人工稿；硬抗/拆谱在 Deep in Abyss 的本轮差异较少。
差异只作为建议复核位置，不自动替换默认风格。旧 v14 综合输出按本表口径为 15、60、75、110、114：本轮前面三首的
人工一致性回退已保留在 `regression.json`，不能宣称全面提升。既有正确标注不应无条件覆盖，新增动作规则也需要后续真人复核。
Synthesis 的部分 Touch 扫责任手和连续 Hold 前后分工仍有差异；两张复杂谱中未验证行明确保留，不计作回放通过。
