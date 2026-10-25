# MiaCode HandOrder

An independent hand order annotation extension based on [MiaCode v2.0.1](https://github.com/Team-MiaCode/MiaCode/releases/tag/2.0.1), developed by [@albertoBradish](https://github.com/albertoBradish). This is not an official Team-MiaCode release.

[中文](README.md) · [Download this fork](https://github.com/albertoBradish/MiaCode/releases) · [Upstream MiaCode](https://github.com/Team-MiaCode/MiaCode)

## Public features

- Manually mark notes with `~l` (left), `~r` (right), or `~m` (either single hand). `~m` does not mean both hands at once.
- Assign a hand to complete selected notes, split a star path into hand order intervals, and edit handoff points. Wifi supports separate sectors and explicit single hand mode.
- Show hand colors in preview, timeline, and video exports. The preview and video footer identifies the extension and version.
- Keep configuration, cache, sessions, and logs in the portable package, separate from the official MiaCode installation.

Experimental automatic hand order decomposition is not part of this public version. Authors make the annotations and viewers should assess the suggested technique themselves.

## Shortcuts

Focus the chart source and select complete notes first. Shortcuts can be changed in Preferences.

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
