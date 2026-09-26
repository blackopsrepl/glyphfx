#!/usr/bin/env python3
"""Render glyphfx frames into GIFs for the README.

Runs glyphfx with --parity-dump to capture the exact frame strings, rasterizes
the ANSI SGR subset glyphfx emits with PIL, and writes one GIF per effect.
"""
import os
import re
import struct
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BIN = os.environ.get("GLYPHFX_BIN", os.path.join(ROOT, "build/glyphfx"))
FONT_DIR = "/usr/share/fonts/truetype"
BANNER = os.path.join(ROOT, "tools/demo/banner.txt")
OUT_DIR = os.path.join(ROOT, "docs/effects")

CANVAS_W, CANVAS_H = 72, 18
FONT_SIZE = 13
DEFAULT_FG = (198, 198, 198)
DEFAULT_BG = (0, 0, 0)
FRAME_MS = 60


def load_palette():
    palette = []
    path = os.path.join(ROOT, "src/utils/hexterm_table.h")
    for line in open(path):
        m = re.match(r'\s*"([0-9a-f]{6})",\s*//\s*(\d+)', line)
        if m:
            h, n = m.group(1), int(m.group(2))
            palette.append((int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)))
    assert len(palette) == 256, len(palette)
    return palette


def load_fonts():
    def f(name, size):
        return ImageFont.truetype(os.path.join(FONT_DIR, name), size)
    return {
        "r": f("LiberationMono-Regular.ttf", FONT_SIZE),
        "b": f("LiberationMono-Bold.ttf", FONT_SIZE),
        "i": f("LiberationMono-Italic.ttf", FONT_SIZE),
        "bi": f("LiberationMono-BoldItalic.ttf", FONT_SIZE),
    }


def capture(effect, effect_args, seed, max_frames, cols, rows):
    env = dict(os.environ)
    env["COLUMNS"] = str(CANVAS_W)
    env["LINES"] = str(CANVAS_H)
    cmd = [BIN, "--seed", str(seed), "--parity-dump", "--max-frames", str(max_frames), "--virtual-clock",
           "--canvas-width", str(cols), "--canvas-height", str(rows), "--anchor-canvas", "c",
           "--anchor-text", "c", effect] + effect_args
    out = subprocess.run(cmd, input=open(BANNER, "rb").read(), stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, env=env).stdout
    frames = []
    i = 0
    while i < len(out):
        nl = out.index(b"\n", i)
        length = int(out[i:nl])
        data = out[nl + 1:nl + 1 + length]
        frames.append(data.decode("utf-8", "replace"))
        i = nl + 1 + length + 1
    return frames


class State:
    def __init__(self, palette):
        self.palette = palette
        self.reset()

    def reset(self):
        self.fg = DEFAULT_FG
        self.bg = DEFAULT_BG
        self.bold = self.italic = self.underline = self.strike = False
        self.reverse = self.hidden = False

    def apply_sgr(self, params):
        if not params:
            params = [0]
        i = 0
        while i < len(params):
            p = params[i]
            if p == 0:
                self.reset()
            elif p == 1:
                self.bold = True
            elif p == 3:
                self.italic = True
            elif p == 4:
                self.underline = True
            elif p == 7:
                self.reverse = True
            elif p == 8:
                self.hidden = True
            elif p == 9:
                self.strike = True
            elif p == 22:
                self.bold = False
            elif p == 23:
                self.italic = False
            elif p == 24:
                self.underline = False
            elif p == 27:
                self.reverse = False
            elif p == 28:
                self.hidden = False
            elif p == 29:
                self.strike = False
            elif p == 39:
                self.fg = DEFAULT_FG
            elif p == 49:
                self.bg = DEFAULT_BG
            elif 30 <= p <= 37:
                self.fg = self.palette[p - 30]
            elif 90 <= p <= 97:
                self.fg = self.palette[p - 90 + 8]
            elif 40 <= p <= 47:
                self.bg = self.palette[p - 40]
            elif 100 <= p <= 107:
                self.bg = self.palette[p - 100 + 8]
            elif p in (38, 48):
                if i + 1 >= len(params):
                    break
                mode = params[i + 1]
                if mode == 5 and i + 2 < len(params):
                    color = self.palette[params[i + 2] & 0xFF]
                    i += 2
                elif mode == 2 and i + 4 < len(params):
                    color = (params[i + 2] & 0xFF, params[i + 3] & 0xFF, params[i + 4] & 0xFF)
                    i += 4
                else:
                    break
                if p == 38:
                    self.fg = color
                else:
                    self.bg = color
            i += 1


def render_frame(frame, palette, fonts, cell_w, cell_h, ascent):
    img = Image.new("RGB", (CANVAS_W * cell_w, CANVAS_H * cell_h), DEFAULT_BG)
    draw = ImageDraw.Draw(img)
    st = State(palette)
    col = row = 0
    i = 0
    n = len(frame)
    while i < n:
        c = frame[i]
        if c == "\x1b" and i + 1 < n and frame[i + 1] == "[":
            j = i + 2
            while j < n and not ("@" <= frame[j] <= "~"):
                j += 1
            if j < n and frame[j] == "m":
                body = frame[i + 2:j]
                params = [int(x) if x else 0 for x in body.split(";")] if body else [0]
                st.apply_sgr(params)
            i = j + 1
            continue
        if c == "\n":
            row += 1
            col = 0
            i += 1
            continue
        if row >= CANVAS_H or col >= CANVAS_W:
            i += 1
            continue
        x, y = col * cell_w, row * cell_h
        fg, bg = st.fg, st.bg
        if st.reverse:
            fg, bg = bg, fg
        if st.hidden:
            fg = bg
        if bg != DEFAULT_BG:
            draw.rectangle([x, y, x + cell_w, y + cell_h], fill=bg)
        if c != " ":
            font = fonts["bi" if (st.bold and st.italic) else "b" if st.bold else "i" if st.italic else "r"]
            draw.text((x, y + ascent), c, font=font, fill=fg, anchor="ls")
        if st.underline:
            draw.line([x, y + cell_h - 2, x + cell_w, y + cell_h - 2], fill=fg)
        if st.strike:
            draw.line([x, y + cell_h // 2, x + cell_w, y + cell_h // 2], fill=fg)
        col += 1
        i += 1
    return img


def save_gif(frames, path):
    if not frames:
        return False
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=FRAME_MS, loop=0,
                   optimize=True, disposal=1)
    return True


EFFECTS = [
    ("beams", [], 90), ("binarypath", [], 90), ("blackhole", [], 110), ("bouncyballs", [], 90),
    ("bubbles", [], 90), ("burn", [], 90), ("colorshift", [], 70), ("crumble", [], 110),
    ("decrypt", ["--typing-speed", "8"], 150), ("errorcorrect", [], 90), ("expand", [], 80),
    ("fireworks", [], 90), ("highlight", [], 90), ("laseretch", [], 90),
    ("matrix", ["--rain-time", "1"], 110), ("middleout", [], 80), ("orbittingvolley", [], 90),
    ("overflow", [], 90), ("pour", [], 80), ("print", ["--print-speed", "6"], 140),
    ("rain", [], 90), ("randomsequence", [], 90), ("rings", [], 90), ("scattered", [], 90),
    ("slice", [], 80), ("slide", [], 80), ("smoke", [], 100), ("spotlights", ["--search-duration", "60"], 110),
    ("spray", [], 90), ("swarm", [], 90), ("sweep", [], 90), ("synthgrid", [], 100),
    ("thunderstorm", ["--storm-time", "1"], 110), ("unstable", [], 90),
    ("vhstape", ["--total-glitch-time", "300"], 110), ("waves", [], 90), ("wipe", [], 120),
]

# The README hero: a large, legible render of one effect.
HERO = ("wipe", [], 140)
HERO_W, HERO_H = 96, 24


def render_one(name, extra, max_frames, cols, rows, palette, fonts, cell_w, cell_h, ascent, path):
    frames = capture(name, extra, seed=7, max_frames=max_frames, cols=cols, rows=rows)
    images = [render_frame(f, palette, fonts, cell_w, cell_h, ascent) for f in frames]
    if not frames:
        print(f"{name}: no frames")
        return
    save_gif(images, path)
    print(f"{name}: {len(frames)} frames -> {path}")


def main():
    only = sys.argv[1:] or None
    palette = load_palette()
    fonts = load_fonts()
    cell_w = int(round(fonts["r"].getlength("M")))
    ascent, descent = fonts["r"].getmetrics()
    cell_h = ascent + descent
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, extra, max_frames in EFFECTS:
        if only and name not in only:
            continue
        render_one(name, extra, max_frames, CANVAS_W, CANVAS_H, palette, fonts, cell_w, cell_h, ascent,
                   os.path.join(OUT_DIR, name + ".gif"))
    if not only or "hero" in only:
        width, height = CANVAS_W, CANVAS_H
        globals()["CANVAS_W"], globals()["CANVAS_H"] = HERO_W, HERO_H
        render_one(HERO[0], HERO[1], HERO[2], HERO_W, HERO_H, palette, fonts, cell_w, cell_h, ascent,
                   os.path.join(OUT_DIR, "hero.gif"))
        globals()["CANVAS_W"], globals()["CANVAS_H"] = width, height


if __name__ == "__main__":
    main()
