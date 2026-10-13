# -*- coding: utf-8 -*-
"""仓库UI（H3RndNew 第五轮）按钮/Tab/标题素材生成器。

参考 H3MegaDesc 确认按钮做法：原生金框(bv_frmL) + 原生底纹(bv_ok*) + 四态亮暗比例。
本脚本用同一批原生素材程序化合成以下 PCX（24bit RGB，运行时量化到游戏调色板）：

  whui/btn_open{0..3}.pcx    66x32  "仓库"     0普通 1按下 2高亮 3禁用
  whui/btn_close{0..3}.pcx   66x32  "关闭"
  whui/btn_take{0..3}.pcx    66x32  "一键取出"
  whui/btn_put{0..3}.pcx     66x32  "一键放入"
  whui/tab{00..13}{n,s}.pcx  44x22  14 个分类页签（n=普通 s=选中）
  whui/arrow_{prev,next}{0,1}.pcx 18x22 滚动箭头（0=可用 1=禁用）
  whui/title.pcx             72x26  "仓库"标题（洋红 255,0,255 为透明键）

用法：
    pythonw scripts/make_whui_assets.py        # 在 H3RndNew 目录下执行
输出同时写 whui/preview.png（3 倍放大拼版，供人眼核对）。
"""

import hashlib
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "whui_src")
OUT = os.path.join(ROOT, "whui")

TITLE_KEY = (255, 0, 255)          # 透明键（运行时映射为调色板索引 0）
INK_NORMAL = (255, 231, 160)
INK_HIGHLIGHT = (255, 248, 214)
INK_PRESSED = (222, 190, 122)
INK_DISABLED = (152, 140, 122)
INK_SHADOW = (28, 18, 8)

BTN_W, BTN_H = 66, 32
TAB_W, TAB_H = 44, 22
ARROW_W, ARROW_H = 18, 22
TITLE_W, TITLE_H = 72, 26

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simsun.ttc",
]

TABS = [
    "常规", "卷轴", "装备", "头部", "肩部", "项链", "武器",
    "盾牌", "胸甲", "戒指", "靴子", "杂物", "机械", "魔法书",
]


def load_font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    raise RuntimeError("找不到可用的中文字体")


def load_rgb(name):
    path = os.path.join(SRC, name)
    with Image.open(path) as im:
        return im.convert("RGB")


def mean_of(im, box):
    patch = im.crop(box)
    px = list(patch.getdata())
    n = len(px)
    return tuple(sum(p[i] for p in px) / n for i in range(3))


def ratio(a, b):
    return tuple(a[i] / max(b[i], 1e-6) for i in range(3))


def measure_states():
    """四态亮度比例：取各 bv_ok 左上 8x8（无对勾区域）实测均值相对普通态。"""
    base = mean_of(load_rgb("bv_ok0.bmp"), (0, 0, 8, 8))
    high = mean_of(load_rgb("bv_ok2.bmp"), (0, 0, 8, 8))
    press = mean_of(load_rgb("bv_ok1.bmp"), (0, 0, 8, 8))
    disabled_full0 = mean_of(load_rgb("bv_ok0.bmp"), (0, 0, 48, 40))
    disabled_full3 = mean_of(load_rgb("bv_ok3.bmp"), (0, 0, 48, 40))
    return {
        "base": base,
        "highlight": ratio(high, base),
        "pressed": ratio(press, base),
        "disabled": ratio(disabled_full3, disabled_full0),
    }


def texture_patch(w, h):
    """确认按钮内部 2px 边距后的真实底纹（避开1px金框），镜像平铺。"""
    return load_rgb("bv_ok0.bmp").crop((2, 2, 8, 8))


def mirror_tile(patch, w, h):
    """镜像平铺，避免整块复制出现明显接缝。"""
    out = Image.new("RGB", (w, h))
    pw, ph = patch.size
    for oy in range(0, h, ph):
        for ox in range(0, w, pw):
            tile = patch
            if (ox // pw) % 2:
                tile = tile.transpose(Image.FLIP_LEFT_RIGHT)
            if (oy // ph) % 2:
                tile = tile.transpose(Image.FLIP_TOP_BOTTOM)
            out.paste(tile, (ox, oy))
    return out


def scale_color(im, factors, desaturate=0.0):
    px = im.load()
    out = Image.new("RGB", im.size)
    op = out.load()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            r, g, b = px[x, y]
            if desaturate > 0.0:
                lum = 0.299 * r + 0.587 * g + 0.114 * b
                r = r + (lum - r) * desaturate
                g = g + (lum - g) * desaturate
                b = b + (lum - b) * desaturate
            op[x, y] = (
                max(0, min(255, int(r * factors[0] + 0.5))),
                max(0, min(255, int(g * factors[1] + 0.5))),
                max(0, min(255, int(b * factors[2] + 0.5))),
            )
    return out


def text_mask(text, size, max_w):
    font = load_font(size)
    box = font.getbbox(text)
    tw = max(1, box[2] - box[0])
    th = max(1, box[3] - box[1])
    mask = Image.new("L", (tw + 6, th + 8), 0)
    draw = ImageDraw.Draw(mask)
    draw.text((3 - box[0], 3 - box[1]), text, font=font, fill=255)
    if mask.size[0] > max_w:
        mask = mask.resize((max_w, mask.size[1]), Image.LANCZOS)
    return mask


def stamp_text(canvas, text, size, color, max_w, dy=0):
    mask = text_mask(text, size, max_w)
    w, h = canvas.size
    x = (w - mask.size[0]) // 2
    y = (h - mask.size[1]) // 2 + dy
    shadow = Image.new("RGB", mask.size, INK_SHADOW)
    canvas.paste(shadow, (x, y + 1), mask)
    ink = Image.new("RGB", mask.size, color)
    canvas.paste(ink, (x, y), mask)


def gold_border(im):
    w, h = im.size
    px = im.load()
    for x in range(w):
        px[x, 0] = (168, 141, 68)
        px[x, h - 1] = (108, 88, 40)
    for y in range(h):
        px[0, y] = (150, 124, 58)
        px[w - 1, y] = (108, 88, 40)


STATE_ORDER = ("normal", "pressed", "highlight", "disabled")


def make_button(text, state, ratios):
    w, h = BTN_W, BTN_H
    fill = mirror_tile(texture_patch(w - 2, h - 2), w - 2, h - 2)
    if state == "highlight":
        fill = scale_color(fill, ratios["highlight"])
        ink = INK_HIGHLIGHT
    elif state == "pressed":
        fill = scale_color(fill, ratios["pressed"])
        ink = INK_PRESSED
    elif state == "disabled":
        fill = scale_color(fill, ratios["disabled"], desaturate=0.55)
        ink = INK_DISABLED
    else:
        ink = INK_NORMAL
    canvas = Image.new("RGB", (w, h))
    canvas.paste(fill, (1, 1))
    size = 16 if len(text) <= 2 else 15
    stamp_text(canvas, text, size, ink, w - 12, dy=1 if state == "pressed" else 0)
    gold_border(canvas)
    return canvas


def make_tab(text, selected):
    w, h = TAB_W, TAB_H
    fill = mirror_tile(texture_patch(w - 2, h - 2), w - 2, h - 2)
    if selected:
        fill = scale_color(fill, (1.35, 1.32, 1.28))
        ink = INK_HIGHLIGHT
    else:
        fill = scale_color(fill, (0.92, 0.92, 0.92))
        ink = INK_NORMAL
    canvas = Image.new("RGB", (w, h))
    canvas.paste(fill, (1, 1))
    stamp_text(canvas, text, 13, ink, w - 6, dy=-1)
    gold_border(canvas)
    return canvas


def make_arrow(glyph, enabled):
    w, h = ARROW_W, ARROW_H
    fill = mirror_tile(texture_patch(w - 2, h - 2), w - 2, h - 2)
    if not enabled:
        fill = scale_color(fill, (0.6, 0.6, 0.6), desaturate=0.6)
    canvas = Image.new("RGB", (w, h))
    canvas.paste(fill, (1, 1))
    font = load_font(14)
    draw = ImageDraw.Draw(canvas)
    ink = INK_HIGHLIGHT if enabled else INK_DISABLED
    box = font.getbbox(glyph)
    draw.text(((w - (box[2] - box[0])) // 2 - box[0],
               (h - (box[3] - box[1])) // 2 - box[1] + 1),
              glyph, font=font, fill=INK_SHADOW)
    draw.text(((w - (box[2] - box[0])) // 2 - box[0],
               (h - (box[3] - box[1])) // 2 - box[1]),
              glyph, font=font, fill=ink)
    gold_border(canvas)
    return canvas


def make_title():
    canvas = Image.new("RGB", (TITLE_W, TITLE_H), TITLE_KEY)
    mask = text_mask("仓库", 19, TITLE_W - 8)
    # Keep anti-aliased text edges opaque; magenta is the transparency key.
    binary = mask.point(lambda value: 255 if value >= 128 else 0)
    x = (TITLE_W - binary.size[0]) // 2
    y = (TITLE_H - binary.size[1]) // 2
    shadow = Image.new("RGB", binary.size, (24, 14, 4))
    canvas.paste(shadow, (x, y + 1), binary)
    ink = Image.new("RGB", binary.size, (255, 233, 165))
    canvas.paste(ink, (x, y), binary)
    return canvas


def save_pcx(im, name):
    path = os.path.join(OUT, name)
    im.save(path)
    with open(path, "rb") as fh:
        digest = hashlib.sha256(fh.read()).hexdigest()
    print("%-22s %sx%s  sha256=%s" % (name, im.size[0], im.size[1], digest[:16]))
    return path


def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    ratios = measure_states()
    print("state ratios: highlight=%s pressed=%s disabled=%s"
          % tuple(tuple(round(v, 3) for v in ratios[k])
                  for k in ("highlight", "pressed", "disabled")))

    preview_cells = []
    buttons = [("open", "仓库"), ("close", "关闭"),
               ("take", "一键取出"), ("put", "一键放入")]
    for key, text in buttons:
        for index, state in enumerate(STATE_ORDER):
            im = make_button(text, state, ratios)
            save_pcx(im, "btn_%s%d.pcx" % (key, index))
            preview_cells.append(im)

    for index, title in enumerate(TABS):
        for suffix, selected in (("n", False), ("s", True)):
            im = make_tab(title, selected)
            save_pcx(im, "tab%02d%s.pcx" % (index, suffix))
            preview_cells.append(im)

    for key, glyph in (("prev", "<"), ("next", ">")):
        for disabled in (0, 1):
            im = make_arrow(glyph, not disabled)
            save_pcx(im, "arrow_%s%d.pcx" % (key, disabled))
            preview_cells.append(im)

    title = make_title()
    save_pcx(title, "title.pcx")

    # 3 倍放大拼版，便于人工核对纹理与文字。
    scale = 3
    cell_w = (max(im.size[0] for im in preview_cells) + 8) * scale
    cell_h = (max(im.size[1] for im in preview_cells) + 8) * scale
    cols = 6
    rows = (len(preview_cells) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell_w, (rows + 1) * cell_h), (48, 48, 66))
    for i, im in enumerate(preview_cells):
        big = im.resize((im.size[0] * scale, im.size[1] * scale), Image.NEAREST)
        sheet.paste(big, ((i % cols) * cell_w + 4, (i // cols) * cell_h + 4))
    sheet.paste(title.resize((title.size[0] * scale, title.size[1] * scale),
                             Image.NEAREST),
                (4, rows * cell_h + 4))
    preview = os.path.join(OUT, "preview.png")
    sheet.save(preview)
    print("preview -> %s (%sx%s)" % (preview, sheet.size[0], sheet.size[1]))
    print("total pcx = %d" % (len(preview_cells) + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
