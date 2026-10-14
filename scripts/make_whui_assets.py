# -*- coding: utf-8 -*-
"""仓库UI（H3RndNew 第十二轮）素材生成器。

按钮底图沿用 H3MegaDesc 的金框母版 bv_frmL.pcx；底纹（round-18 起）直裁自
whui_src/HA_bg_crop.pcx（取自 H3Auto HA_bg.pcx 图形正中 66x32，用户指定
“需要多少截多少”），四态 = 底图明暗（STATE_TINT）+ 文字明暗（INK_*）+ 按压
文字下移 1px；页签/页码两态沿用 round-12 系数。标题不再烘焙PCX，生产代码
直接使用原生背包的 H3DlgText 参数。

  whui/btn_open{0..3}.pcx    66x32  "仓库"
  whui/btn_close{0..3}.pcx   24x24  顶部小X
  whui/btn_take{0..3}.pcx    66x32  "一键取出"
  whui/btn_put{0..3}.pcx     66x32  "一键放入"
  whui/tab{00..11}{n,s}.pcx  36x22  12 个分类页签
  whui/arrow_{prev,next}{0,1}.pcx 18x22 滚动箭头
  whui/pager_{n,s}.pcx       28x22  两位数定宽页码底图（无文字，数字运行时叠加）
  whui/tidy{0..3}.pcx        36x22  "整理"按钮（四态，与主按钮同一管线）

共 50 张 PCX：btn 16 + tab 24 + arrow 4 + pager 2 + tidy 4。

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

BTN_FRAME_KEY = (0, 255, 255)      # bv_frmL.pcx 透明青色
BTN_W, BTN_H = 66, 32
CLOSE_W, CLOSE_H = 24, 24
# 两字页签和整理按钮保持 36x22；页码按一般两位数另用定宽底板。
TAB_W, TAB_H = 36, 22
ARROW_W, ARROW_H = 18, 22
PAGER_W, PAGER_H = 28, 22
TIDY_W, TIDY_H = 36, 22
INK_NORMAL = (255, 231, 160)
INK_HIGHLIGHT = (255, 248, 214)
INK_PRESSED = (222, 190, 122)
INK_DISABLED = (152, 140, 122)
INK_SHADOW = (28, 18, 8)

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simsun.ttc",
]

# 出图按 TabKind 枚举规范序（固定：常规,卷轴,装备,头部,肩部,项链,武器,盾牌,胸甲,
# 戒指,靴子,杂物），tab{枚举值}.pcx；页签显示顺序是运行期配置（[WarehouseUI]
# TabOrder，默认 常规,卷轴,杂物,靴子,戒指,胸甲,盾牌,武器,项链,肩部,头部,装备），
# 与素材解耦——换序不用重新出图。
TABS = [
    "常规", "卷轴", "装备", "头部", "肩部", "项链", "武器",
    "盾牌", "胸甲", "戒指", "靴子", "杂物",
]

TIDY_TEXT = "整理"


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


# ---- 底纹纹理源（round-18 用户裁定：直裁，不再平铺） ----
# round-17 用 bv_bg 木纹镜像平铺，用户仍不满意（“现在这样也不行”），指定：
# “需要多少的底图，就去 D:\GitHub\H3\H3Auto\img\HA_bg.pcx 这个图中间截取所需大小；
# 四态主要是底图+文字的明暗度变化，和位置变化”。
# 据此把 HA_bg.pcx（680x548）图形正中的 66x32 直裁存为 whui_src/HA_bg_crop.pcx：
# 所有控件尺寸（66x32/36x22/28x22/24x24/18x22）都不超过它，小尺寸从它正中再裁，
# 全程无平铺、无缩放。四态 = 底图明暗（STATE_TINT）+ 文字明暗（INK_*）+ 按压
# 文字下移 1px；页签/页码两态沿用 TAB_TINT。
# 实测该块：lum 52.4±6.0、饱和 0.654（按钮目标 0.648）——饱和已天然匹配，
# 无需再去饱和；金色像素 0（自证，非 0 直接 raise）。
TEXTURE_SOURCE = "HA_bg_crop.pcx"   # whui_src 内 66x32，取自 H3Auto HA_bg.pcx 图形正中
# 实测“排除金色像素后的均值比”：state1/state0=0.845、state3/state0=0.853；
# highlight 取 1.35（与页签选中态同款；bv_ok2 的白描边会把比值虚高到 1.53，直接用会过曝）。
# normal=1.0：HA_bg 本身就是用户想要的观感，不再做亮度校准。
STATE_TINT = {
    "normal": 1.0,
    "pressed": 0.845,
    "highlight": 1.35,
    "disabled": 0.853,
}
TAB_TINT = {"n": 0.92, "s": 1.35}      # 沿用 round-12 两态系数（只换底纹源）


def is_gold_pixel(px):
    """参考图勾号的金色判定（亮黄、蓝通道明显低）。"""
    r, g, b = px
    return r > 120 and g > 95 and b < 0.72 * g and (r + g) - 2 * b > 120


def texture_source():
    """底纹源：HA_bg_crop.pcx（自证 66x32 且无金色像素——换源时当场拦住标记残留）。"""
    block = load_rgb(TEXTURE_SOURCE)
    if block.size != (66, 32):
        raise ValueError("底纹源必须是 66x32，实际 %s" % (block.size,))
    gold = sum(1 for px in block.getdata() if is_gold_pixel(px))
    if gold:
        raise ValueError("底纹源含 %d 个金色像素（参考图标记未除净）" % gold)
    return block


def base_crop(w, h):
    """按需从源块正中直裁（round-18 用户裁定：需要多少截多少，不平铺、不缩放）。"""
    src = texture_source()
    if w > src.size[0] or h > src.size[1]:
        raise ValueError("控件 %dx%d 超过底纹源 %dx%d，禁止平铺/缩放"
                         % (w, h, src.size[0], src.size[1]))
    x0 = (src.size[0] - w) // 2
    y0 = (src.size[1] - h) // 2
    return src.crop((x0, y0, x0 + w, y0 + h))


def state_texture(w, h, state):
    """按钮四态底纹：底图 + 明暗度变化（位置变化由文字层按压 +1px 负责）。"""
    k = STATE_TINT[state]
    return scale_color(base_crop(w, h), (k, k, k))


def tab_texture(w, h, selected):
    """页签/页码/箭头底纹（选中/未选中两态，沿用 round-12 系数）。"""
    k = TAB_TINT["s" if selected else "n"]
    return scale_color(base_crop(w, h), (k, k, k))


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

# round-17 删除 clean_ok_base/nine_slice（bv_ok 清勾取样带 = 金色残留 + 5px 竖条来源）；
# round-18 删除 mirror_tile/tinted_texture（bv_bg 木纹镜像平铺，用户仍不满意）——
# 底纹统一由 base_crop() 从 HA_bg 中块按需直裁生成。


def apply_reference_frame(canvas):
    """套用原始 1px 金框，不缩放窄化原图边缘。"""
    frame = load_rgb("bv_frmL.pcx")
    if frame.size != (50, 42):
        raise ValueError("bv_frmL must be 50x42, got %s" % (frame.size,))
    width, height = canvas.size
    frame_px = frame.load()
    out_px = canvas.load()
    for x in range(width):
        source_x = x * (frame.width - 1) // max(width - 1, 1)
        out_px[x, 0] = frame_px[source_x, 0]
        out_px[x, height - 1] = frame_px[source_x, frame.height - 1]
    for y in range(1, height - 1):
        source_y = y * (frame.height - 1) // max(height - 1, 1)
        out_px[0, y] = frame_px[0, source_y]
        out_px[width - 1, y] = frame_px[frame.width - 1, source_y]
    return canvas


def make_button(text, state):
    canvas = apply_reference_frame(state_texture(BTN_W, BTN_H, state))
    ink = {"normal": INK_NORMAL, "highlight": INK_HIGHLIGHT,
           "pressed": INK_PRESSED, "disabled": INK_DISABLED}[state]
    size = 16 if len(text) <= 2 else 15
    stamp_text(canvas, text, size, ink, BTN_W - 12,
               dy=1 if state == "pressed" else 0)
    return canvas


def make_close(state):
    canvas = apply_reference_frame(state_texture(CLOSE_W, CLOSE_H, state))
    stamp_text(canvas, "X",
               14, {"normal": INK_NORMAL, "highlight": INK_HIGHLIGHT,
                    "pressed": INK_PRESSED, "disabled": INK_DISABLED}[state],
               CLOSE_W - 4, dy=1 if state == "pressed" else 0)
    return canvas


def make_panel_canvas(w, h, selected):
    """页签/页码共用底板：统一木纹底纹 + tab n/s 同源的两态亮度差异。"""
    fill = tab_texture(w, h, selected)
    canvas = Image.new("RGB", (w, h))
    canvas.paste(fill, (1, 1))
    return canvas


def make_tab(text, selected):
    canvas = make_panel_canvas(TAB_W, TAB_H, selected)
    ink = INK_HIGHLIGHT if selected else INK_NORMAL
    stamp_text(canvas, text, 13, ink, TAB_W - 6, dy=-1)
    gold_border(canvas)
    return canvas


def make_pager(selected):
    """分页页码按钮底图：沿用页签构建路径（同纹理、同两态差异、同金框），
    round-12 起固定 28x22 两位数定宽并跳过文字绘制——页码数字由运行时 H3DlgText
    叠加，32x32 原版控制面板没有此控件。"""
    canvas = make_panel_canvas(PAGER_W, PAGER_H, selected)
    gold_border(canvas)
    return canvas


def make_tidy(state):
    """整理按钮（round-13 用户裁定）：与“一键取出/放入”完全同一管线——同木纹四态
    底纹 + 参考金框 + 状态墨色 + 按压态文字下移 1px，只是尺寸 36x22 放进分页栏。
    文案「整理」烘焙进 PCX（游戏字体不保证中文字形）。"""
    canvas = apply_reference_frame(state_texture(TIDY_W, TIDY_H, state))
    ink = {"normal": INK_NORMAL, "highlight": INK_HIGHLIGHT,
           "pressed": INK_PRESSED, "disabled": INK_DISABLED}[state]
    stamp_text(canvas, TIDY_TEXT, 13, ink, TIDY_W - 6,
               dy=1 if state == "pressed" else 0)
    return canvas


def make_arrow(glyph, enabled):
    w, h = ARROW_W, ARROW_H
    if enabled:
        fill = tab_texture(w, h, True)
    else:
        # 禁用态：更暗 + 强去饱和（沿用旧观感 0.6 亮度 / 0.6 去饱和）。
        k = TAB_TINT["n"] * 0.6
        fill = scale_color(base_crop(w, h), (k, k, k), desaturate=0.6)
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
    print("texture source=%s %s tints=%s tab_tints=%s"
          % (TEXTURE_SOURCE, texture_source().size, STATE_TINT, TAB_TINT))

    preview_cells = []
    buttons = [("open", "仓库"), ("take", "一键取出"), ("put", "一键放入")]
    for key, text in buttons:
        for index, state in enumerate(STATE_ORDER):
            im = make_button(text, state)
            save_pcx(im, "btn_%s%d.pcx" % (key, index))
            preview_cells.append(im)
    for index, state in enumerate(STATE_ORDER):
        im = make_close(state)
        save_pcx(im, "btn_close%d.pcx" % index)
        preview_cells.append(im)

    # 整理四态与主按钮同块摆放（preview 前排每行 4 列都是按钮态）。
    for index, state in enumerate(STATE_ORDER):
        im = make_tidy(state)
        save_pcx(im, "tidy%d.pcx" % index)
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

    for suffix, selected in (("n", False), ("s", True)):
        im = make_pager(selected)
        save_pcx(im, "pager_%s.pcx" % suffix)
        preview_cells.append(im)

    # 所有按钮统一每行4列（normal/pressed/highlight/disabled）；非按钮素材
    # （页签/箭头/页码）从按钮块整行数之后继续 4 列排，避免覆盖最后一行按钮。
    scale = 3
    cell_w = (BTN_W + 8) * scale
    cell_h = (BTN_H + 8) * scale
    button_count = (len(buttons) + 2) * len(STATE_ORDER)  # 文本按钮+关闭+整理
    button_rows = (button_count + 3) // 4
    sheet_rows = button_rows + (len(preview_cells) - button_count + 3) // 4
    sheet = Image.new("RGB", (cell_w * 4, cell_h * sheet_rows), (48, 48, 66))
    for i, im in enumerate(preview_cells):
        big = im.resize((im.size[0] * scale, im.size[1] * scale), Image.NEAREST)
        if i < button_count:
            col, row = i % 4, i // 4
        else:
            col, row = (i - button_count) % 4, button_rows + (i - button_count) // 4
        sheet.paste(big, (col * cell_w + 4, row * cell_h + 4))
    preview = os.path.join(OUT, "preview.png")
    sheet.save(preview)
    print("preview -> %s (%sx%s)" % (preview, sheet.size[0], sheet.size[1]))
    print("total pcx = %d" % len(preview_cells))
    return 0


if __name__ == "__main__":
    sys.exit(main())
