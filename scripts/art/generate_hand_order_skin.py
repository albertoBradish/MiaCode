"""Build hand-order note art from the original SD/DX PNGs.

Requires Pillow and NumPy. Run from any directory:
    python scripts/art/generate_hand_order_skin.py

The output is kept beside each original in hand_order/{l,r,m}/. Geometry and
alpha are copied exactly; only the visible RGB of note bodies changes.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image


SKIN_ROOT = Path(__file__).resolve().parents[2] / "assets" / "skin"
HANDS = {
    "l": (0x38, 0x8C, 0xFF),
    "r": (0xFF, 0x45, 0x54),
    "m": (0xDB, 0xE0, 0xEB),
}


def note_files(skin_dir: Path) -> list[Path]:
    names = [
        "tap", "tap_each", "tap_break", "tap_mine",
        "hold", "hold_on", "hold_off", "hold_each", "hold_each_on",
        "hold_break", "hold_break_on", "hold_mine",
        "star", "star_double", "star_each", "star_each_double",
        "star_break", "star_break_double", "star_mine", "star_mine_double",
        "slide", "slide_each", "slide_break", "slide_mine",
        "touch", "touch_each", "touch_break", "touch_mine",
        "touch_point", "touch_point_each", "touch_break_point", "touch_point_mine",
    ]
    for prefix in ("wifi", "wifi_each", "wifi_break", "wifi_mine"):
        names.extend(f"{prefix}_{index}" for index in range(11))
    for prefix in ("touchhold", "touchhold_break"):
        names.extend(f"{prefix}_{index}" for index in range(4))
    names.extend(f"touchhold_{index}_mine" for index in range(4))
    return [skin_dir / f"{name}.png" for name in names]


def recolor(pixels: np.ndarray, name: str, target: tuple[int, int, int]) -> np.ndarray:
    result = pixels.copy()
    rgb = pixels[:, :, :3].astype(np.float32) / 255.0
    maximum = np.max(rgb, axis=2)
    minimum = np.min(rgb, axis=2)
    saturation = np.divide(maximum - minimum, maximum, out=np.zeros_like(maximum), where=maximum > 0)

    # Neutral white linework and the dark shape stay in the source palette.
    chroma_mask = np.clip((saturation - 0.08) / 0.18, 0.0, 1.0)
    dark_mask = np.clip((maximum - 0.09) / 0.17, 0.0, 1.0)
    if "mine" in name or name == "hold_off":
        # Mine and inactive Hold art are gray, so their midtones need a
        # separate mask.
        midtone_mask = np.clip((maximum - 0.17) / 0.20, 0.0, 1.0)
        midtone_mask *= 1.0 - np.clip((maximum - 0.78) / 0.16, 0.0, 1.0)
        chroma_mask = np.maximum(chroma_mask, midtone_mask)

    category_strength = 1.0 if target == HANDS["m"] else 0.96
    if "mine" in name or name == "hold_off":
        category_strength = 0.86  # Keep its neutral Mine shading.
    strength = chroma_mask * dark_mask * category_strength

    if "break" in name or "each" in name:
        # Keep a narrow strip of the original gold at the top of the visible
        # sprite. The rest still reads as the assigned hand, even for a solid
        # yellow Each track with no natural color separation.
        visible_rows = np.flatnonzero(np.any(pixels[:, :, 3] > 32, axis=1))
        if visible_rows.size:
            top, bottom = visible_rows[0], visible_rows[-1]
            row = np.arange(pixels.shape[0], dtype=np.float32)[:, None]
            relative_y = (row - top) / max(1, bottom - top)
            accent = 1.0 - np.clip((relative_y - 0.12) / 0.09, 0.0, 1.0)
            strength *= 1.0 - 0.87 * accent
    strength = strength[:, :, None]

    target_rgb = np.asarray(target, dtype=np.float32) / 255.0
    # The source brightness supplies the bevel and gradient of each PNG.
    shaded_target = target_rgb[None, None, :] * (0.20 + 0.80 * maximum[:, :, None])
    result[:, :, :3] = np.rint(np.clip(rgb * (1.0 - strength) + shaded_target * strength, 0.0, 1.0) * 255).astype(np.uint8)
    return result


def main() -> None:
    total = 0
    for skin_name in ("skinSD", "skinDX"):
        skin_dir = SKIN_ROOT / skin_name
        for source in note_files(skin_dir):
            if not source.is_file():
                raise FileNotFoundError(source)
            with Image.open(source) as image:
                original = np.asarray(image.convert("RGBA"), dtype=np.uint8)
            for hand, target in HANDS.items():
                output = skin_dir / "hand_order" / hand / source.name
                output.parent.mkdir(parents=True, exist_ok=True)
                Image.fromarray(recolor(original, source.stem, target), "RGBA").save(output, optimize=True)
                total += 1
    print(f"Generated {total} hand-order PNGs from SD/DX originals.")


if __name__ == "__main__":
    main()
