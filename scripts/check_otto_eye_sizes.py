#!/usr/bin/env python3
"""Report eye ew/eh stats per emotion using the same logic as otto_gif_blue_eyes.process_frame."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

from PIL import Image, ImageSequence

SCRIPT_DIR = Path(__file__).resolve().parent
OG = None


def load_og():
    global OG
    spec = importlib.util.spec_from_file_location("og", SCRIPT_DIR / "otto_gif_blue_eyes.py")
    OG = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(OG)
    return OG


def stats_for_emotion(gif_path: Path, og) -> dict | None:
    emotion = gif_path.stem.lower()
    if emotion not in og.EMOTION_STYLES:
        return None
    im = Image.open(gif_path)
    frames = list(ImageSequence.Iterator(im))
    n = len(frames)
    ews_l: list[int] = []
    ehs_l: list[int] = []
    ews_r: list[int] = []
    ehs_r: list[int] = []
    for i, fr in enumerate(frames):
        fr = fr.convert("RGBA")
        style = og.frame_style(emotion, i, n)
        lb = og.find_eye_bbox(fr, "left")
        rb = og.find_eye_bbox(fr, "right")
        sym = og.symmetrize_eye_wh(lb, rb)
        if not (lb and rb and sym):
            continue
        cx_l, cy_l, ew_l, eh_l = og.compute_eye_size(lb, style, "left", sym)
        cx_r, cy_r, ew_r, eh_r = og.compute_eye_size(rb, style, "right", sym)
        ew_l, ew_r = og.narrow_eyes_for_bridge(cx_l, cx_r, ew_l, ew_r)
        ews_l.append(ew_l)
        ehs_l.append(eh_l)
        ews_r.append(ew_r)
        ehs_r.append(eh_r)
    if not ews_l:
        return {"emotion": emotion, "frames": n, "ok": False}

    def agg(xs: list[int]) -> tuple[int, int, float]:
        return min(xs), max(xs), sum(xs) / len(xs)

    return {
        "emotion": emotion,
        "frames": n,
        "ok": True,
        "left_ew": agg(ews_l),
        "left_eh": agg(ehs_l),
        "right_ew": agg(ews_r),
        "right_eh": agg(ehs_r),
    }


def main() -> int:
    og = load_og()
    root = SCRIPT_DIR.parent / "managed_components" / "txp666__otto-emoji-gif-component" / "gifs" / "_backup_original"
    if len(sys.argv) > 1:
        root = Path(sys.argv[1])
    if not root.is_dir():
        print(f"Not a directory: {root}", file=sys.stderr)
        return 1

    rows: list[dict] = []
    for gif in sorted(root.glob("*.gif")):
        r = stats_for_emotion(gif, og)
        if r:
            rows.append(r)

    print("=== Eye size after symmetrize + narrow bridge (source GIFs) ===")
    print("min-max (mean) pixels — L/R should match ew after bridge; buxue may differ slightly")
    for r in rows:
        if not r.get("ok"):
            print(f"{r['emotion']:12}  no bboxes")
            continue
        lew, leh = r["left_ew"], r["left_eh"]
        rew, reh = r["right_ew"], r["right_eh"]
        print(
            f"{r['emotion']:12}  n={r['frames']:2}  "
            f"L ew {lew[0]}-{lew[1]} ({lew[2]:.1f})  eh {leh[0]}-{leh[1]} ({leh[2]:.1f})  |  "
            f"R ew {rew[0]}-{rew[1]} ({rew[2]:.1f})  eh {reh[0]}-{reh[1]} ({reh[2]:.1f})"
        )

    print()
    print("=== Mean L-eye ew x eh — compare emotions ===")
    for r in rows:
        if not r.get("ok"):
            continue
        lew, leh = r["left_ew"], r["left_eh"]
        print(f"{r['emotion']:12}  {lew[2]:.1f} x {leh[2]:.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
