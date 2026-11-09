# MiaCode HandOrder

An independent hand order annotation extension based on [MiaCode dev](https://github.com/Team-MiaCode/MiaCode/commit/be494a567869b407fd17acad33482af6ad1c664d), developed by [@albertoBradish](https://github.com/albertoBradish). This is not an official Team-MiaCode release.

Current development version: **2.0.1-handorder.27.dev.3**.

[中文](README.md) · [Download this fork](https://github.com/albertoBradish/MiaCode/releases) · [Upstream MiaCode](https://github.com/Team-MiaCode/MiaCode)

## Public features

- Manually mark notes with `~l` (left), `~r` (right), or `~m` (either single hand). `~m` does not mean both hands at once.
- Assign a hand to complete selected notes, split a star path into hand order intervals, and edit handoff points. Wifi supports separate sectors and explicit single hand mode.
- Show hand colors in preview, timeline, and video exports. The preview and video footer identifies the extension and version.
- Keep configuration, cache, sessions, and logs in the portable package, separate from the official MiaCode installation.

Experimental automatic hand order decomposition is not part of this public version. Authors make the annotations and viewers should assess the suggested technique themselves.

## Editing and export

- Synchronize note selection across source, timeline, and preview, with Shift selection, non-contiguous editing, and independent Slide head and track selection.
- Continuously zoom the timeline, resize note symbols, choose four lane orders, and use horizontal, falling, or rising layouts. The default bottom tabs group Timeline, Syntax, Muri. Each panel can dock or float independently.
- The activity rail stays in the main window while the outline panel can dock or float independently. The Workspace menu controls panel visibility, preview side, sidebar style, timeline layout, and named layouts. Press the backtick key to maximize or restore the current panel.
- Edit advanced chart information, including song and designer IDs, version, level constants, demo ranges, and creator comments.
- Recommend Drop regions from audio and chart analysis, adjust them manually or from a source selection, and save them in maidata for preview and export.
- Click or drag on the pressure heatmap to seek. Pressure is zero during rests, while the tooltip still shows fatigue recovery and left/right details from manual annotations.
- Normal preview always hides judgement markers and touch trails. Muri and black-background Debug keep their own switches. Measure lines, judgement skeletons, and Slide judgement timing share the display menu. BPM, CPS, and other HUD elements support arbitrary aspect ratios.
- Export chart-preview intros, multiple covers, 30/100/200 MB presets, or a custom bitrate, with an independent PV switch.
- Save and exchange video templates as `.miatemplate`, with legacy JSON compatibility. Cover text supports outlines and shadows.
- Import, apply, and export BPM data, and calculate DX score loss and SSS/SSS+ allowances.
- Use automatic chart formatting with backups, a spacebar guard, empty-difficulty ZIP warnings, and a low-performance mode.
- The fox closes its eyes while jumping to the beat during Drop regions. It blinks naturally with gentler head and tail motion elsewhere.

## Shortcuts

Focus the chart source and select complete notes first. Shortcuts can be changed in Preferences. With the default binding, Alt+W marks notes as left-handed when the source editor has an editable selection and opens Workspace elsewhere.

| Action | Default |
| --- | --- |
| Left hand | Alt+W |
| Right hand | Alt+E |
| Split star hand order intervals | Alt+T |
| Either hand | Alt+Y |
| Split into independent slides | Ctrl+Alt+5 |

Splitting hand order intervals changes annotations only. Splitting into independent slides changes native chart objects and may affect judgments and scoring; review its diff before applying.

## Download and use

Download the Windows x64 portable package from [this fork's Releases](https://github.com/albertoBradish/MiaCode/releases). Extract the entire package to a writable directory and start the root `MiaCode.exe`. Keep `app/`, `assets/`, and `HandOrderPackage.marker`. Personal data stays in `runtime/`; exports default to `exports/`.

See the [annotation specification](docs/specs/chart/HAND_ORDER_ANNOTATION_SPEC_ZH.md), [manual tools](docs/specs/chart/HAND_ORDER_MANUAL_TOOLS_SPEC_ZH.md), and [build scripts](scripts/README_EN.md). Building from source requires Qt 6.10+, CMake, MSVC, and FFmpeg development libraries.

## Credits and license

This fork follows MiaCode's [GPLv3 license](LICENSE). Team-MiaCode created the original application and artwork; @albertoBradish maintains the extension, hand order assets, and its releases. Report extension issues in [this fork](https://github.com/albertoBradish/MiaCode/issues).
