#!/usr/bin/env python3
"""Retouch Otto emoji GIFs: blue rounded-square eyes with per-emotion styling."""

from __future__ import annotations

import math
import shutil
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageSequence

EMOTION_STYLES: dict[str, dict] = {
    "staticstate": {
        "color": (126, 200, 227),
        "scale_w": 0.92,
        "scale_h": 0.88,
        "radius_ratio": 0.32,
        "min_radius": 8,
        "max_radius": 13,
    },
    "happy": {
        "color": (80, 195, 255),
        "scale_w": 1.08,
        "scale_h": 1.04,
        "radius_ratio": 0.38,
        "min_radius": 10,
        "max_radius": 17,
    },
    "sad": {
        "color": (88, 155, 190),
        "scale_w": 0.90,
        "scale_h": 0.68,
        "radius_ratio": 0.28,
        "min_radius": 7,
        "max_radius": 12,
    },
    "anger": {
        "color": (60, 130, 220),
        "scale_w": 1.06,
        "scale_h": 0.56,
        "radius_ratio": 0.22,
        "min_radius": 5,
        "max_radius": 9,
    },
    "scare": {
        "color": (150, 220, 255),
        "scale_w": 1.22,
        "scale_h": 1.24,
        "radius_ratio": 0.36,
        "min_radius": 11,
        "max_radius": 18,
    },
    "buxue": {
        "color": (100, 175, 210),
        "scale_w": 0.92,
        "scale_h": 0.80,
        "radius_ratio": 0.28,
        "min_radius": 7,
        "max_radius": 13,
    },
}

WHITE_SUM_MIN = 470
WHITE_CH_MIN = 175
MIN_EYE_PIXELS = 40
# Expand strict white bbox so gray anti-alias ring (old round eye) is included
EYE_BBOX_HALO_PAD = 12
# Wider than old round eye so the white/gray halo is fully cleared
ERASE_EXPAND = 1.55
ERASE_MIN_PAD = 18
# Global eye size multiplier (1.0 = previous size)
EYE_SIZE_BOOST = 1.18
# Push eyes outward from face center (pixels per side)
EYE_SPREAD_PX = 7
# Extend erase region toward face center so gray halo between the eyes is cleared
CENTER_BRIDGE_PX = 16
# Minimum horizontal gap between inner edges of left/right eyes (after symmetrize + boost)
EYE_MIN_BRIDGE_PX = 6

# --light: fewer frames + smaller palette + optimize (same look, less CPU/PSRAM bandwidth on ESP32)
LIGHT_FRAME_STEP = 2          # keep every Nth frame (2 ≈ half decode load)
LIGHT_DURATION_MULT = 2       # stretch timing so animation speed stays similar
LIGHT_PALETTE_COLORS = 64     # smaller palette → faster decode + smaller flash
LIGHT_MIN_FRAME_MS = 100      # floor per frame when light mode (avoids very fast decode loops)
# Decode/render cost scales with pixel count; 120² uses ~4× less CPU than 240² on ESP32.
LIGHT_OUTPUT_SIZE = 120       # 0 = keep source size; 120 + LVGL 2× scale → full 240 LCD


def emotion_from_name(filename: str) -> str:
    return Path(filename).stem.lower()


def frame_style(emotion: str, frame_idx: int, frame_count: int) -> dict:
    base = dict(EMOTION_STYLES[emotion])
    t = frame_idx / max(frame_count - 1, 1)
    phase = math.sin(t * math.pi * 2)

    if emotion == "happy":
        base["scale_w"] *= 1.0 + 0.06 * math.sin(t * math.pi * 4)
        base["scale_h"] *= 0.92 + 0.10 * abs(math.sin(t * math.pi * 3))
    elif emotion == "sad":
        base["scale_h"] *= 0.92 - 0.08 * t
        base["color"] = (
            base["color"][0],
            int(base["color"][1] * (0.95 - 0.05 * t)),
            int(base["color"][2] * (0.92 - 0.04 * t)),
        )
    elif emotion == "anger":
        base["scale_h"] *= 0.95 + 0.08 * (1 if phase > 0.3 else 0)
        base["color"] = (int(base["color"][0] * 0.95), base["color"][1], base["color"][2])
    elif emotion == "scare":
        base["scale_w"] *= 1.0 + 0.10 * max(0, math.sin(t * math.pi * 2))
        base["scale_h"] *= 1.0 + 0.12 * max(0, math.sin(t * math.pi * 2))
    elif emotion == "buxue":
        base["_left_w"] = 0.95 if frame_idx % 2 else 0.88
        base["_right_w"] = 0.88 if frame_idx % 2 else 0.95
    elif emotion == "staticstate":
        # Idle: intentional smaller than other emotions; 0.88 = rõ to hơn bản 0.80 (~85px)
        base["scale_w"] *= 0.88
        base["scale_h"] *= 0.88
        base["scale_h"] *= 0.94 + 0.06 * abs(math.sin(t * math.pi * 2))

    return base


def find_eye_bbox(im: Image.Image, side: str) -> tuple[int, int, int, int] | None:
    w, h = im.size
    px = im.load()
    x_split = w // 2
    pts: list[tuple[int, int]] = []

    if side == "left":
        x_range = range(0, x_split)
    else:
        x_range = range(x_split, w)

    for y in range(h):
        for x in x_range:
            r, g, b, a = px[x, y]
            if a < 100:
                continue
            if r + g + b >= WHITE_SUM_MIN and min(r, g, b) >= WHITE_CH_MIN - 30:
                pts.append((x, y))

    if len(pts) < MIN_EYE_PIXELS:
        return None

    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0 = max(0, min(xs) - EYE_BBOX_HALO_PAD)
    y0 = max(0, min(ys) - EYE_BBOX_HALO_PAD)
    x1 = min(w - 1, max(xs) + EYE_BBOX_HALO_PAD)
    y1 = min(h - 1, max(ys) + EYE_BBOX_HALO_PAD)
    return x0, y0, x1, y1


def expand_bbox(
    bbox: tuple[int, int, int, int],
    size: tuple[int, int],
    scale: float = ERASE_EXPAND,
    min_pad: int = ERASE_MIN_PAD,
) -> tuple[int, int, int, int]:
    x0, y0, x1, y1 = bbox
    cx = (x0 + x1) / 2
    cy = (y0 + y1) / 2
    bw = max(x1 - x0, 1)
    bh = max(y1 - y0, 1)
    ew = max(int(bw * scale), bw + min_pad * 2)
    eh = max(int(bh * scale), bh + min_pad * 2)
    w, h = size
    nx0 = max(0, int(cx - ew / 2))
    ny0 = max(0, int(cy - eh / 2))
    nx1 = min(w - 1, int(cx + ew / 2))
    ny1 = min(h - 1, int(cy + eh / 2))
    return nx0, ny0, nx1, ny1


def is_blue_pixel(r: int, g: int, b: int, blue: tuple[int, int, int], tol: int = 55) -> bool:
    # Gray halo (e.g. 138,138,138) can fall inside the per-channel L∞ box around `blue`
    # but must still be erased; real drawn blue is chromatic (B leads R/G).
    spread = max(r, g, b) - min(r, g, b)
    if spread < 22:
        return False
    return (
        abs(r - blue[0]) <= tol
        and abs(g - blue[1]) <= tol
        and abs(b - blue[2]) <= tol
        and b >= r
    )


def should_erase_old_eye_pixel(r: int, g: int, b: int, a: int, blue: tuple[int, int, int]) -> bool:
    if a < 60:
        return False
    if is_blue_pixel(r, g, b, blue):
        return False
    total = r + g + b
    # White / near-white (old eye fill + highlight)
    if total >= 300:
        return True
    if r > 140 and g > 140 and b > 140:
        return True
    # Gray anti-alias ring around the old circle (quantized GIF may use 100–140 RGB)
    if total >= 160 and max(r, g, b) - min(r, g, b) <= 55:
        return True
    return False


def erase_old_eye_area(
    im: Image.Image,
    bbox: tuple[int, int, int, int],
    blue: tuple[int, int, int],
    side: str,
    w: int,
) -> None:
    px = im.load()
    region = expand_bbox(bbox, im.size)
    x0, y0, x1, y1 = region
    if side == "left":
        x1 = min(w - 1, x1 + CENTER_BRIDGE_PX)
    else:
        x0 = max(0, x0 - CENTER_BRIDGE_PX)

    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            r, g, b, a = px[x, y]
            if should_erase_old_eye_pixel(r, g, b, a, blue):
                px[x, y] = (0, 0, 0, 255)


def compute_eye_size(
    bbox: tuple[int, int, int, int],
    style: dict,
    side: str,
    sym_wh: tuple[int, int] | None,
) -> tuple[float, float, int, int]:
    x0, y0, x1, y1 = bbox
    cx = (x0 + x1) / 2 + (-EYE_SPREAD_PX if side == "left" else EYE_SPREAD_PX)
    cy = (y0 + y1) / 2
    if sym_wh is not None:
        bw, bh = sym_wh
    else:
        bw = max(x1 - x0, 8)
        bh = max(y1 - y0, 6)

    sw = style.get("_left_w" if side == "left" else "_right_w", 1.0)
    ew = int(bw * style["scale_w"] * sw * EYE_SIZE_BOOST)
    eh = int(bh * style["scale_h"] * EYE_SIZE_BOOST)
    ew = max(ew, 10)
    eh = max(eh, 6)
    return cx, cy, ew, eh


def draw_eye_rect(
    draw: ImageDraw.ImageDraw,
    style: dict,
    cx: float,
    cy: float,
    ew: int,
    eh: int,
) -> None:
    radius = int(min(ew, eh) * style["radius_ratio"])
    radius = max(style["min_radius"], min(style["max_radius"], radius))

    left = int(cx - ew / 2)
    top = int(cy - eh / 2)
    right = left + ew
    bottom = top + eh

    color = (*style["color"], 255)
    draw.rounded_rectangle([left, top, right, bottom], radius=radius, fill=color)


def narrow_eyes_for_bridge(
    cx_l: float,
    cx_r: float,
    ew_l: int,
    ew_r: int,
) -> tuple[int, int]:
    """Shrink both eye widths equally so inner edges keep at least EYE_MIN_BRIDGE_PX gap."""
    span = cx_r - cx_l
    half_sum = (ew_l + ew_r) / 2.0
    gap = span - half_sum
    if gap >= EYE_MIN_BRIDGE_PX or ew_l + ew_r <= 0:
        return ew_l, ew_r
    # span - s * half_sum >= EYE_MIN_BRIDGE_PX  =>  s <= (span - g) / half_sum
    s = 2.0 * (span - float(EYE_MIN_BRIDGE_PX)) / float(ew_l + ew_r)
    if s >= 1.0:
        return ew_l, ew_r
    s = max(s, 0.25)
    return max(int(ew_l * s), 8), max(int(ew_r * s), 8)


def symmetrize_eye_wh(
    left: tuple[int, int, int, int] | None,
    right: tuple[int, int, int, int] | None,
) -> tuple[int, int] | None:
    """Use the larger of the two white bboxes so both eyes share the same base size."""
    boxes = [b for b in (left, right) if b is not None]
    if not boxes:
        return None
    bws = [max(b[2] - b[0], 8) for b in boxes]
    bhs = [max(b[3] - b[1], 6) for b in boxes]
    return max(bws), max(bhs)


def process_frame(src: Image.Image, emotion: str, frame_idx: int, frame_count: int) -> Image.Image:
    out = src.convert("RGBA")
    style = frame_style(emotion, frame_idx, frame_count)
    blue = style["color"]
    draw = ImageDraw.Draw(out)

    left_bbox = find_eye_bbox(out, "left")
    right_bbox = find_eye_bbox(out, "right")
    sym_wh = symmetrize_eye_wh(left_bbox, right_bbox)

    for side, bbox in (("left", left_bbox), ("right", right_bbox)):
        if bbox is None:
            continue
        erase_old_eye_area(out, bbox, blue, side, out.size[0])

    if left_bbox and right_bbox:
        cx_l, cy_l, ew_l, eh_l = compute_eye_size(left_bbox, style, "left", sym_wh)
        cx_r, cy_r, ew_r, eh_r = compute_eye_size(right_bbox, style, "right", sym_wh)
        ew_l, ew_r = narrow_eyes_for_bridge(cx_l, cx_r, ew_l, ew_r)
        draw_eye_rect(draw, style, cx_l, cy_l, ew_l, eh_l)
        draw_eye_rect(draw, style, cx_r, cy_r, ew_r, eh_r)
    else:
        for side, bbox in (("left", left_bbox), ("right", right_bbox)):
            if bbox is None:
                continue
            cx, cy, ew, eh = compute_eye_size(bbox, style, side, sym_wh)
            draw_eye_rect(draw, style, cx, cy, ew, eh)

    return out


def save_gif(
    frames_rgba: list[Image.Image],
    out_path: Path,
    duration: int,
    loop: int,
    *,
    light: bool = False,
    output_size: int = 0,
) -> None:
    palette_colors = LIGHT_PALETTE_COLORS if light else 256
    frame_duration = max(int(duration), LIGHT_MIN_FRAME_MS if light else 1)
    if light and output_size <= 0:
        output_size = LIGHT_OUTPUT_SIZE

    p_frames: list[Image.Image] = []
    for fr in frames_rgba:
        if output_size > 0 and fr.size[0] != output_size:
            fr = fr.resize((output_size, output_size), Image.Resampling.LANCZOS)
        bg = Image.new("RGBA", fr.size, (0, 0, 0, 255))
        bg.alpha_composite(fr)
        p = bg.convert("RGB").quantize(colors=palette_colors, method=Image.Quantize.MEDIANCUT)
        p_frames.append(p.convert("RGBA"))

    p_frames[0].save(
        out_path,
        save_all=True,
        append_images=p_frames[1:],
        duration=frame_duration,
        loop=loop,
        disposal=2,
        optimize=light,
    )


def process_gif(
    path: Path,
    source_path: Path | None = None,
    *,
    light: bool = False,
    output_size: int = 0,
) -> None:
    emotion = emotion_from_name(path.name)
    if emotion not in EMOTION_STYLES:
        print(f"Skip unknown emotion: {path.name}")
        return

    read_path = source_path if source_path and source_path.exists() else path
    im = Image.open(read_path)
    duration = int(im.info.get("duration", 80) or 80)
    loop = im.info.get("loop", 0)
    src_frames = [f.copy() for f in ImageSequence.Iterator(im)]

    if light and LIGHT_FRAME_STEP > 1 and len(src_frames) > 1:
        src_frames = src_frames[::LIGHT_FRAME_STEP]
        duration = max(duration * LIGHT_DURATION_MULT, LIGHT_MIN_FRAME_MS)

    out_frames = [
        process_frame(f, emotion, i, len(src_frames)) for i, f in enumerate(src_frames)
    ]
    save_gif(out_frames, path, duration, loop, light=light, output_size=output_size)
    mode = "light" if light else "full"
    sz = output_size if (light and output_size > 0) else (output_size or out_frames[0].size[0])
    if light and output_size <= 0 and LIGHT_OUTPUT_SIZE > 0:
        sz = LIGHT_OUTPUT_SIZE
    print(
        f"OK {path.name}: {len(out_frames)} frames, {duration}ms/frame, {sz}px [{mode}] ({read_path.name})"
    )


def main() -> int:
    argv = sys.argv[1:]
    light = "--light" in argv
    output_size = 0
    positional: list[str] = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--light":
            i += 1
            continue
        if a == "--size" and i + 1 < len(argv):
            output_size = int(argv[i + 1])
            i += 2
            continue
        if a.startswith("--size="):
            output_size = int(a.split("=", 1)[1])
            i += 1
            continue
        positional.append(a)
        i += 1

    if positional:
        gif_dir = Path(positional[0])
    else:
        root = Path(__file__).resolve().parents[1]
        gif_dir = root / "components" / "txp666__otto-emoji-gif-component" / "gifs"

    if not gif_dir.is_dir():
        print(f"GIF directory not found: {gif_dir}")
        return 1

    backup = gif_dir / "_backup_original"
    backup.mkdir(exist_ok=True)
    for gif in gif_dir.glob("*.gif"):
        dst = backup / gif.name
        if not dst.exists():
            shutil.copy2(gif, dst)

    for gif in sorted(gif_dir.glob("*.gif")):
        src = backup / gif.name
        process_gif(gif, src if src.exists() else None, light=light, output_size=output_size)

    print(f"Done. Originals in {backup}")
    if light:
        sz = output_size or LIGHT_OUTPUT_SIZE
        print(
            f"Light mode: ~half frames, {LIGHT_PALETTE_COLORS}-color palette, "
            f"{sz}px canvas (LVGL scales to 240 LCD). Rebuild: idf.py build flash"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
