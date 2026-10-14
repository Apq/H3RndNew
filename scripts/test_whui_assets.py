# -*- coding: utf-8 -*-
"""独立素材验收：不依赖构建或游戏部署。

整理按钮字形差分一项例外地导入生成器模块（仅重建“无字底板”，main() 有
__main__ 门不会出图），其余测试仍不导入生成器。"""
import ast
from pathlib import Path
import re
import struct
import unittest

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'whui'
SRC = ROOT / 'whui_src'
# 出图按 TabKind 枚举规范序（固定），显示顺序由运行期 [WarehouseUI] TabOrder 决定。
TABS = ['常规', '卷轴', '装备', '头部', '肩部', '项链', '武器',
        '盾牌', '胸甲', '戒指', '靴子', '杂物']
# round-12：页码按两位数定宽 28x22；两字页签与整理按钮仍是 36x22。
PAGER_WIDTH, PAGER_HEIGHT = 28, 22
TIDY_WIDTH, TIDY_HEIGHT = 36, 22
# 各控件画布与文字排版参数（必须与生成器调用点一致，文字覆盖集合按此复刻）。
BTN_W, BTN_H = 66, 32
CLOSE_W, CLOSE_H = 24, 24
TAB_W, TAB_H = 36, 22
ARROW_W, ARROW_H = 18, 22
STATE_ORDER = ('normal', 'pressed', 'highlight', 'disabled')
TIDY_TEXT = '整理'
EXPECTED = {
    **{f'btn_{button}{state}.pcx': ((24, 24) if button == 'close' else (66, 32))
       for button in ('open', 'close', 'take', 'put') for state in range(4)},
    # round-9：页签收窄到 36x22（两字标题）；round-12：页码两位数定宽 28x22。
    **{f'tab{tab:02d}{state}.pcx': (36, 22) for tab in range(12) for state in ('n', 's')},
    **{f'arrow_{direction}{state}.pcx': (18, 22)
       for direction in ('prev', 'next') for state in range(2)},
    'pager_n.pcx': (28, 22),
    'pager_s.pcx': (28, 22),
    # round-13：整理并入主按钮四态管线（tidy0..tidy3，36x22）。
    **{f'tidy{state}.pcx': (36, 22) for state in range(4)},
}

INK_NORMAL = (255, 231, 160)
INK_HIGHLIGHT = (255, 248, 214)
INK_SHADOW = (28, 18, 8)

# round-18：底纹源改为 whui_src/HA_bg_crop.pcx——按用户裁定从 H3Auto 的
# HA_bg.pcx（680x548）图形正中直裁 66x32，按需从这块正中再裁小尺寸，
# 全程不平铺、不缩放。旧实现（bv_ok 清勾取样带 / bv_bg 木纹镜像平铺）已废弃。
# 以下参数必须与 make_whui_assets.py 完全一致；
# test_texture_constants_match_generator 从生成器源码 AST 逐项核对防漂移。
TEXTURE_SOURCE = 'HA_bg_crop.pcx'        # whui_src 内 66x32，实测 0 金色像素
STATE_TINT = {'normal': 1.0, 'pressed': 0.845,
              'highlight': 1.35, 'disabled': 0.853}
TAB_TINT = {'n': 0.92, 's': 1.35}


def is_gold_pixel(px):
    """参考图勾号的金色判定（亮黄、蓝通道明显低），与生成器同规则。"""
    r, g, b = px
    return r > 120 and g > 95 and b < 0.72 * g and (r + g) - 2 * b > 120


def decode_pcx(data, size):
    """独立按扫描行解RLE，与Pillow回解交叉验证，不复用生产解码器。"""
    if len(data) < 128 or data[:4] != bytes((10, 5, 1, 8)) or data[65] != 3:
        raise ValueError('not 24-bit RGB RLE PCX')
    xmin, ymin, xmax, ymax = struct.unpack_from('<4H', data, 4)
    width, height = xmax - xmin + 1, ymax - ymin + 1
    stride = struct.unpack_from('<H', data, 66)[0]
    if (width, height) != size or stride < width:
        raise ValueError('dimensions or stride mismatch')
    rgb = bytearray()
    pos = 128
    for _ in range(height):
        row = bytearray()
        while len(row) < stride * 3:
            if pos >= len(data):
                raise ValueError('truncated payload')
            token = data[pos]
            pos += 1
            count = token & 63 if token >= 192 else 1
            if token >= 192:
                if not count or pos >= len(data):
                    raise ValueError('invalid RLE packet')
                token = data[pos]
                pos += 1
            if len(row) + count > stride * 3:
                raise ValueError('scanline overflow')
            row.extend([token] * count)
        for x in range(width):
            rgb.extend((row[x], row[stride + x], row[2 * stride + x]))
    if pos != len(data):
        raise ValueError('unexpected trailing data')
    return bytes(rgb)


def scale_tinted(image, factor, desaturate=0.0):
    """同生成器 scale_color：先去饱和再乘系数，int(v*f+0.5) 后钳制。

    不用 Python round（半值取偶会偏）；去饱和必须在乘系数之前，顺序反了结果不同。"""
    src = image.load()
    out = Image.new('RGB', image.size)
    dst = out.load()
    for y in range(image.height):
        for x in range(image.width):
            channels = list(src[x, y])
            if desaturate > 0.0:
                lum = 0.299 * channels[0] + 0.587 * channels[1] + 0.114 * channels[2]
                channels = [c + (lum - c) * desaturate for c in channels]
            dst[x, y] = tuple(max(0, min(255, int(c * factor + 0.5)))
                              for c in channels)
    return out


def texture_base(width, height):
    """底纹源：whui_src/HA_bg_crop.pcx 按需正中直裁（与生成器 base_crop 同一规则）。

    round-18 起不再平铺——用户裁定“需要多少的底图就从 HA_bg 中间截多少”。
    源块 66x32 覆盖所有控件；自证尺寸与 0 金色像素，换源时当场拦住标记残留。"""
    with Image.open(SRC / TEXTURE_SOURCE) as master:
        block = master.convert('RGB')
    if block.size != (66, 32):
        raise AssertionError('底纹源必须是 66x32，实际 %s' % (block.size,))
    if width > block.width or height > block.height:
        raise AssertionError('控件 %dx%d 超过底纹源，禁止平铺/缩放' % (width, height))
    base = block.crop(((block.width - width) // 2,
                       (block.height - height) // 2,
                       (block.width - width) // 2 + width,
                       (block.height - height) // 2 + height))
    gold = [px for px in base.getdata() if is_gold_pixel(px)]
    if gold:
        raise AssertionError('裁切块含 %d 个金色像素（HA_bg 标记未除净）' % len(gold))
    return base


def gold_border(image):
    """同生成器的 1px 金框：先行写满上下、再列写左右，角点最终由列循环覆盖。"""
    width, height = image.size
    px = image.load()
    for x in range(width):
        px[x, 0] = (168, 141, 68)
        px[x, height - 1] = (108, 88, 40)
    for y in range(height):
        px[0, y] = (150, 124, 58)
        px[width - 1, y] = (108, 88, 40)


def reference_frame(canvas):
    """同生成器 apply_reference_frame：1px 边框按比例取自 bv_frmL.pcx 采样。"""
    with Image.open(SRC / 'bv_frmL.pcx') as raw:
        frame = raw.convert('RGB')
    width, height = canvas.size
    frame_px, out_px = frame.load(), canvas.load()
    for x in range(width):
        source_x = x * (frame.width - 1) // max(width - 1, 1)
        out_px[x, 0] = frame_px[source_x, 0]
        out_px[x, height - 1] = frame_px[source_x, frame.height - 1]
    for y in range(1, height - 1):
        source_y = y * (frame.height - 1) // max(height - 1, 1)
        out_px[0, y] = frame_px[0, source_y]
        out_px[width - 1, y] = frame_px[frame.width - 1, source_y]
    return canvas


def tab_plate(width, height, factor, desaturate=0.0):
    """页签/页码/箭头一类底板（不导入生成器）：正中直裁 + 两态亮度 + 固定金框。

    生成器 make_panel_canvas 把整幅 (w,h) 裁块贴到 (1,1) 并被画布裁掉右下 1px，
    这里照抄同一构造。箭头禁用态另有 0.6 去饱和，故参数化。"""
    fill = scale_tinted(texture_base(width, height), factor, desaturate)
    canvas = Image.new('RGB', (width, height))
    canvas.paste(fill, (1, 1))
    gold_border(canvas)
    return canvas


def button_plate(width, height, factor):
    """按钮/整理一类底板（不导入生成器）：正中直裁 + 参考金框。"""
    return reference_frame(scale_tinted(texture_base(width, height), factor))


# 与生成器同序的字体候选：文字覆盖集合必须按同一字体度量复刻，否则会误比文字像素。
FONT_CANDIDATES = [
    r'C:\Windows\Fonts\simhei.ttf',
    r'C:\Windows\Fonts\msyhbd.ttc',
    r'C:\Windows\Fonts\msyh.ttc',
    r'C:\Windows\Fonts\simsun.ttc',
]


def load_font(size):
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    raise RuntimeError('找不到可用的中文字体')


def stamp_cover(text, size, max_w, canvas_size, dy):
    """复刻生成器 stamp_text 覆盖的像素（字形墨迹 + 1px 阴影 + 抗锯齿外扩 2px）。

    文字像素是墨色与底纹的混合（边缘半透明），不属于“底纹”，比对时必须跳过；
    这是按字体度量独立算出的精确集合，比按矩形粗跳更严——矩形会连带跳过一大片
    真正的底纹像素，反而放松了“底纹只来自 HA_bg 中块直裁”的不变量。"""
    font = load_font(size)
    box = font.getbbox(text)
    tw, th = max(1, box[2] - box[0]), max(1, box[3] - box[1])
    mask = Image.new('L', (tw + 6, th + 8), 0)
    ImageDraw.Draw(mask).text((3 - box[0], 3 - box[1]), text, font=font, fill=255)
    if mask.size[0] > max_w:
        mask = mask.resize((max_w, mask.size[1]), Image.LANCZOS)
    width, height = canvas_size
    x0 = (width - mask.size[0]) // 2
    y0 = (height - mask.size[1]) // 2 + dy
    px = mask.load()
    cover = set()
    for my in range(mask.size[1]):
        for mx in range(mask.size[0]):
            if px[mx, my] == 0:
                continue
            for ay in range(y0 + my - 2, y0 + my + 3):
                for ax in range(x0 + mx - 2, x0 + mx + 3):
                    if 1 <= ax < width - 1 and 1 <= ay < height - 1:
                        cover.add((ax, ay))
    return cover


def glyph_cover(text, size, canvas_size):
    """箭头字形覆盖集合：生成器对箭头直接用 draw.text（不走 stamp_text）。"""
    font = load_font(size)
    box = font.getbbox(text)
    gw, gh = box[2] - box[0], box[3] - box[1]
    width, height = canvas_size
    left = (width - gw) // 2 - box[0]
    top = (height - gh) // 2 - box[1]
    cover = set()
    for yy in range(top - 2, top + gh + 3):
        for xx in range(left - 2, left + gw + 3):
            if 1 <= xx < width - 1 and 1 <= yy < height - 1:
                cover.add((xx, yy))
    return cover


class WarehouseAssetsTest(unittest.TestCase):
    def test_exact_manifest(self):
        self.assertEqual(len(EXPECTED), 50)
        self.assertEqual({p.name for p in ASSETS.glob('*.pcx')}, set(EXPECTED))
        for obsolete in ('title.pcx', 'tab12n.pcx', 'tab12s.pcx', 'tab13n.pcx',
                         'tab13s.pcx', 'tidy_n.pcx', 'tidy_s.pcx'):
            self.assertFalse((ASSETS / obsolete).exists())

    def test_rgb_dimensions_and_independent_decode(self):
        for name, size in EXPECTED.items():
            with self.subTest(asset=name), Image.open(ASSETS / name) as image:
                self.assertEqual(image.mode, 'RGB')
                self.assertEqual(image.size, size)
                self.assertEqual(decode_pcx((ASSETS / name).read_bytes(), size), image.tobytes())
                self.assertGreater(len(set(image.getdata())), 16)

    def test_state_images_are_distinct(self):
        groups = [[f'btn_{key}{state}.pcx' for state in range(4)]
                  for key in ('open', 'close', 'take', 'put')]
        groups += [[f'tab{tab:02d}{state}.pcx' for state in ('n', 's')] for tab in range(12)]
        groups += [[f'arrow_{key}{state}.pcx' for state in range(2)] for key in ('prev', 'next')]
        groups += [['pager_n.pcx', 'pager_s.pcx'], [f'tidy{state}.pcx' for state in range(4)]]
        for names in groups:
            with self.subTest(group=names):
                self.assertEqual(len({(ASSETS / name).read_bytes() for name in names}), len(names))

    def test_generator_categories(self):
        source = (ROOT / 'scripts/make_whui_assets.py').read_text(encoding='utf-8-sig')
        tree = ast.parse(source)
        literals = {}
        for node in tree.body:
            if not isinstance(node, ast.Assign):
                continue
            for target in node.targets:
                names = []
                if isinstance(target, ast.Name):
                    names.append(target.id)
                elif isinstance(target, ast.Tuple):
                    names.extend(elt.id for elt in target.elts if isinstance(elt, ast.Name))
                for name in names:
                    literals[name] = node.value
        self.assertEqual(ast.literal_eval(literals['TABS']), TABS)
        # 新增整理素材：中文烘焙的两字文案；页码 28x22、整理 36x22 已解耦。
        self.assertEqual(ast.literal_eval(literals['TIDY_TEXT']), '整理')
        self.assertEqual(ast.literal_eval(literals['PAGER_W']), (28, 22))
        self.assertEqual(ast.literal_eval(literals['TIDY_W']), (36, 22))

        def called_names(function_name):
            function = next(node for node in tree.body
                            if isinstance(node, ast.FunctionDef) and node.name == function_name)
            return {call.func.id for call in ast.walk(function)
                    if isinstance(call, ast.Call) and isinstance(call.func, ast.Name)}

        self.assertNotIn('stamp_text', called_names('make_pager'))
        self.assertIn('stamp_text', called_names('make_tidy'))
        # round-17：整理与主按钮同一构建管线（无金色木纹四态底纹 + 参考框 + 文字）。
        self.assertEqual(called_names('make_tidy'),
                         {'state_texture', 'apply_reference_frame', 'stamp_text'})
        # round-17：make_button 只做底纹（state_texture）+ 参考框 + 文字；len 是排版用的内建调用。
        self.assertEqual(called_names('make_button') - {'len'},
                         {'state_texture', 'apply_reference_frame', 'stamp_text'})
        self.assertIn('"tidy%d.pcx" % index', source)
        self.assertIn('make_tidy(state)', source)
        # round-17 淘汰 bv_ok 清勾取样带路径；round-18 淘汰 bv_bg 木纹镜像平铺路径。
        for retired in ('clean_ok_base', 'nine_slice', 'texture_patch', 'measure_states',
                        'mirror_tile', 'tinted_texture'):
            self.assertNotIn('def %s(' % retired, source,
                             '%s 是已淘汰的旧底纹路径' % retired)

    def test_texture_constants_match_generator(self):
        """底纹常量必须与生成器逐项一致（测试自建底板否则会误判）。

        round-18 起底纹源 = whui_src/HA_bg_crop.pcx（HA_bg.pcx 图形正中直裁 66x32）
        + 四态/两态亮度系数；这些值同时写在测试与生成器里，必须 AST 核对防止只改一边。"""
        source = (ROOT / 'scripts/make_whui_assets.py').read_text(encoding='utf-8-sig')
        tree = ast.parse(source)
        literals = {}
        for node in tree.body:
            if isinstance(node, ast.Assign):
                for target in node.targets:
                    if isinstance(target, ast.Name):
                        literals[target.id] = node.value
        self.assertEqual(ast.literal_eval(literals['TEXTURE_SOURCE']), TEXTURE_SOURCE)
        self.assertEqual(ast.literal_eval(literals['STATE_TINT']), STATE_TINT)
        self.assertEqual(ast.literal_eval(literals['TAB_TINT']), TAB_TINT)

    def test_tab_order_config_matches_code_default(self):
        """[WarehouseUI] TabOrder 与代码内置默认必须逐字一致，且恰好是 12 个互异页签名。

        运行期解析失败会静默回退默认（只留一条日志），所以打包进包的 ini 与代码默认
        不一致、或有错别字/重复项，都属于“玩家改了没反应”的隐性故障，必须在离线回归里
        拦住。三处必须同时一致：default.ini、Catalog.hpp 的 kDefaultTabOrderText、
        素材生成器的 TABS（规范序）。"""
        ini = (ROOT / 'H3RndNew.default.ini').read_text(encoding='utf-8')
        section = None
        tab_order = None
        for raw_line in ini.splitlines():
            line = raw_line.strip()
            if line.startswith('[') and line.endswith(']'):
                section = line[1:-1]
                continue
            if section == 'WarehouseUI' and line.startswith('TabOrder'):
                key, _, value = line.partition('=')
                self.assertEqual(key.strip(), 'TabOrder')
                tab_order = value.strip()
        self.assertIsNotNone(tab_order, 'default.ini 的 [WarehouseUI] 必须带 TabOrder')

        header = (ROOT / 'modules/WarehouseUiCatalog.hpp').read_text(encoding='utf-8')
        match = re.search(r'kDefaultTabOrderText\s*=\s*\n?\s*"([^"]*)"', header)
        self.assertIsNotNone(match, 'Catalog.hpp 必须有 kDefaultTabOrderText 字面量')
        self.assertEqual(tab_order, match.group(1),
                         'default.ini 的 TabOrder 必须与代码内置默认逐字一致')

        names = [name.strip() for name in tab_order.split(',')]
        self.assertEqual(len(names), 12)
        self.assertEqual(len(set(names)), 12, '页签名不得重复')
        self.assertEqual(sorted(names), sorted(TABS),
                         '页签名必须与素材生成器的 12 个标题集合一致')

    def test_pager_row_gap_config_matches_code_default(self):
        """[WarehouseUI] PagerRowBottomGap 与代码内置默认必须一致（round-13）。

        与 TabOrder 同理：运行期读不到/读错键会静默用代码默认，若随包 ini 与
        代码默认不一致，玩家“改了没反应”只能靠猜；ini 键、Flow.hpp 的
        kPagerRowBottomGapDefault、钳制上下限三处同时核对。"""
        ini = (ROOT / 'H3RndNew.default.ini').read_text(encoding='utf-8')
        section, value = None, None
        for raw_line in ini.splitlines():
            line = raw_line.strip()
            if line.startswith('[') and line.endswith(']'):
                section = line[1:-1]
                continue
            if section == 'WarehouseUI' and line.startswith('PagerRowBottomGap'):
                value = int(line.partition('=')[2].strip())
        self.assertIsNotNone(value, 'default.ini 的 [WarehouseUI] 必须带 PagerRowBottomGap')

        header = (ROOT / 'modules/WarehouseUiFlow.hpp').read_text(encoding='utf-8')
        def literal(name):
            match = re.search(rf'{name}\s*=\s*(\d+)\s*;', header)
            self.assertIsNotNone(match, f'Flow.hpp 必须有 {name} 数字字面量')
            return int(match.group(1))
        self.assertEqual(value, literal('kPagerRowBottomGapDefault'),
                         'ini 默认必须与代码内置默认一致')
        self.assertLessEqual(literal('kPagerRowBottomGapMin'), value)
        self.assertGreaterEqual(literal('kPagerRowBottomGapMax'), value)

    def test_reconstructed_plate_matches_pager(self):
        """自建底板必须与生成器实际出图逐像素一致——否则整理差分参照不可信。

        页码仍是同一条 make_panel_canvas 路径，取 28x22 做交叉验证；通过即证明
        本文件独立重建的正中直裁/两态亮度/去饱和/金框规则与生产生成器等价，
        同时说明页码底纹里不存在任何标记残留（HA_bg 中块本身就是 0 金色像素）。"""
        for suffix, selected in (('n', False), ('s', True)):
            with self.subTest(state=suffix):
                with Image.open(ASSETS / f'pager_{suffix}.pcx') as pager:
                    produced = pager.convert('RGB')
                self.assertEqual(produced.size, (PAGER_WIDTH, PAGER_HEIGHT))
                expected = tab_plate(PAGER_WIDTH, PAGER_HEIGHT,
                                     TAB_TINT['s' if selected else 'n'])
                self.assertEqual(produced.tobytes(), expected.tobytes(),
                                 '独立重建的底板必须与生产页码出图完全一致')

    def assert_plate_matches(self, produced, plate, label, skip=()):
        """逐像素比对底纹，跳过两类非底纹像素。

        1) 1px 边框：金框/参考框由独立测试覆盖；
        2) 文字覆盖区：字形墨迹、1px 阴影与抗锯齿边缘都是墨色与底纹的混合（实测
           边缘会与底纹差 1 个单位），不属于底纹；覆盖集合由 stamp_cover/glyph_cover
           按生成器同款字体度量独立算出，比按矩形粗跳更严——矩形会连带跳过一大片
           真正的底纹像素，反而放松了“底纹只来自 HA_bg 中块直裁”的不变量。"""
        width, height = produced.size
        covered = set(skip)
        for y in range(1, height - 1):
            for x in range(1, width - 1):
                if (x, y) in covered:
                    continue
                self.assertEqual(
                    produced.getpixel((x, y)), plate.getpixel((x, y)),
                    '%s 底纹像素 (%d,%d) 与 HA_bg 中央裁块重建不符' % (label, x, y))

    def test_reconstructed_plates_match_textured_assets(self):
        """页签/箭头/页码底纹必须等于“HA_bg 中央裁块 × 状态系数”的独立重建。

        round-18 的核心不变量：底纹只能来自 HA_bg.pcx 图形正中的直裁块。一旦有人
        把底纹换回 bv_ok 清勾取样带、或 bv_bg 木纹镜像平铺，这里会逐像素失败。
        页签/箭头跳过文字覆盖像素；页码无文字，全部像素都要一致。"""
        cases = [(f'tab{index:02d}{suffix}.pcx', TAB_TINT[tint],
                  stamp_cover(TABS[index], 13, TAB_W - 6, (TAB_W, TAB_H), -1))
                 for index in range(12)
                 for suffix, tint in (('n', 'n'), ('s', 's'))]
        cases += [('arrow_prev0.pcx', TAB_TINT['s'],
                   glyph_cover('<', 14, (ARROW_W, ARROW_H))),
                  ('arrow_next0.pcx', TAB_TINT['s'],
                   glyph_cover('>', 14, (ARROW_W, ARROW_H))),
                  ('pager_n.pcx', TAB_TINT['n'], ()),
                  ('pager_s.pcx', TAB_TINT['s'], ())]
        for name, factor, cover in cases:
            with self.subTest(asset=name):
                with Image.open(ASSETS / name) as raw:
                    produced = raw.convert('RGB')
                plate = tab_plate(*produced.size, factor)
                self.assert_plate_matches(produced, plate, name, skip=cover)

    def test_arrow_disabled_state_matches_independent_rebuild(self):
        """禁用态箭头：同底纹 + 额外 0.6 亮度 / 0.6 去饱和（与 make_arrow 同规则）。

        箭头走“paste 到 (1,1) + 固定金框”这一条路径（不是按钮的整幅参考框），
        所以这里用 tab_plate 并传入禁用态去饱和量。"""
        for name, glyph in (('arrow_prev1.pcx', '<'), ('arrow_next1.pcx', '>')):
            with self.subTest(asset=name):
                with Image.open(ASSETS / name) as raw:
                    produced = raw.convert('RGB')
                plate = tab_plate(*produced.size, TAB_TINT['n'] * 0.6, 0.6)
                self.assert_plate_matches(produced, plate, name,
                                          skip=glyph_cover(glyph, 14, produced.size))

    def button_cases(self):
        """(素材名, 组, 状态, 文字, 字号, 可用宽度, 画布) —— 与生成器调用点一致。"""
        cases = []
        for button, text in (('open', '仓库'), ('take', '一键取出'), ('put', '一键放入')):
            for index, state in enumerate(STATE_ORDER):
                cases.append((f'btn_{button}{index}.pcx', f'btn_{button}', state,
                              text, 16 if len(text) <= 2 else 15, BTN_W - 12,
                              (BTN_W, BTN_H)))
        for index, state in enumerate(STATE_ORDER):
            cases.append((f'btn_close{index}.pcx', 'btn_close', state, 'X', 14,
                          CLOSE_W - 4, (CLOSE_W, CLOSE_H)))
            cases.append((f'tidy{index}.pcx', 'tidy', state, TIDY_TEXT, 13,
                          TIDY_WIDTH - 6, (TIDY_WIDTH, TIDY_HEIGHT)))
        return cases

    def test_button_plates_match_independent_rebuild(self):
        """按钮/关闭/整理四态底纹必须等于“HA_bg 中央裁块 × 状态系数”的独立重建。

        文字覆盖像素跳过（字形差分由 test_tidy_states_follow_button_pipeline 覆盖），
        并验证四态亮度序 highlight > normal > pressed/disabled。亮度均值先算后断言，
        避免某个素材断言失败导致后面的序检查报 KeyError 掩盖真实原因。"""
        plates, means = {}, {}
        for name, group, state, text, size, max_w, canvas in self.button_cases():
            with self.subTest(asset=name):
                with Image.open(ASSETS / name) as raw:
                    produced = raw.convert('RGB')
                self.assertEqual(produced.size, canvas)
                plate = button_plate(canvas[0], canvas[1], STATE_TINT[state])
                plates[name] = plate
                picked = [plate.getpixel((2, 2)),
                          plate.getpixel((canvas[0] - 3, canvas[1] - 3))]
                means[f'{group}:{state}'] = sum(sum(p) for p in picked) / len(picked)
        for name, group, state, text, size, max_w, canvas in self.button_cases():
            with self.subTest(asset=name):
                with Image.open(ASSETS / name) as raw:
                    produced = raw.convert('RGB')
                dy = 1 if state == 'pressed' else 0
                self.assert_plate_matches(
                    produced, plates[name], name,
                    skip=stamp_cover(text, size, max_w, canvas, dy))
        for group in [f'btn_{b}' for b in ('open', 'close', 'take', 'put')] + ['tidy']:
            normal = means[f'{group}:normal']
            self.assertGreater(means[f'{group}:highlight'], normal,
                               f'{group} 高亮态应比普通态亮')
            self.assertLess(means[f'{group}:pressed'], normal,
                            f'{group} 按压态应比普通态暗')
            self.assertLess(means[f'{group}:disabled'], normal,
                            f'{group} 禁用态应比普通态暗')

    def test_tidy_states_follow_button_pipeline(self):
        """整理四态必须与主按钮同一管线（round-13 用户裁定）。

        底板 = 对应态无金色木纹底纹 + 参考金框。此项例外地导入生成器模块
        重建“无字底板”（main() 有 __main__ 门，不会出图），验证的组合事实是：
        make_tidy 只在底板上叠“整理”两个字——差分区域必须小、居中、左右两半
        各有足量笔画、比底板亮；四态同字形，按压态文字整体下移 1px。"""

        import make_whui_assets as gen
        states = list(gen.STATE_ORDER)
        plates, images, regions = {}, {}, {}
        for index, state in enumerate(states):
            plates[state] = gen.apply_reference_frame(
                gen.state_texture(TIDY_WIDTH, TIDY_HEIGHT, state))
            with Image.open(ASSETS / f'tidy{index}.pcx') as raw:
                images[state] = raw.convert('RGB')
            regions[state] = frozenset(
                (x, y) for y in range(TIDY_HEIGHT) for x in range(TIDY_WIDTH)
                if images[state].getpixel((x, y)) != plates[state].getpixel((x, y)))

        for state in states:
            ink = regions[state]
            self.assertLess(len(ink), 36 * 22 // 2, f'{state} 差分必须只是文字区')
            self.assertGreaterEqual(len(ink), 250, f'{state} 两字笔画不足')
            left = min(p[0] for p in ink)
            right = max(p[0] for p in ink)
            top = min(p[1] for p in ink)
            bottom = max(p[1] for p in ink)
            self.assertGreaterEqual(left, 1)
            self.assertLessEqual(right, 34)
            self.assertGreaterEqual(top, 1)
            self.assertLessEqual(bottom, 20)
            self.assertGreaterEqual(right - left + 1, 24, f'{state} 至少占 24 列')
            middle = (left + right) // 2
            self.assertGreaterEqual(sum(1 for p in ink if p[0] <= middle), 80,
                                    f'{state} 左半字笔画不足')
            self.assertGreaterEqual(sum(1 for p in ink if p[0] > middle), 80,
                                    f'{state} 右半字笔画不足')
            image, plate = images[state], plates[state]
            self.assertGreater(
                sum(sum(image.getpixel(p)) for p in ink) / len(ink),
                sum(sum(plate.getpixel(p)) for p in ink) / len(ink),
                f'{state} 文字区应比底板更亮（浅黄墨色已烘焙）')
        # 同字形：非按压态覆盖区几乎逐像素一致；按压态 = 普通态整体下移 1px。
        base = set(regions['normal'])
        for state in ('highlight', 'disabled'):
            self.assertLessEqual(len(base ^ set(regions[state])), 24,
                                 f'{state} 与普通态必须同字形')
        pressed = {(x, y + 1) for (x, y) in regions['normal']}
        self.assertLessEqual(len(pressed ^ set(regions['pressed'])), 24,
                             '按压态文字应整体下移 1px（与主按钮同款）')

    def test_preview_matches_every_asset(self):
        names = [f'btn_{key}{state}.pcx' for key in ('open', 'take', 'put', 'close')
                 for state in range(4)]
        names += [f'tidy{state}.pcx' for state in range(4)]
        names += [f'tab{tab:02d}{state}.pcx' for tab in range(12) for state in ('n', 's')]
        names += [f'arrow_{key}{state}.pcx' for key in ('prev', 'next') for state in range(2)]
        names += ['pager_n.pcx', 'pager_s.pcx']
        with Image.open(ASSETS / 'preview.png') as preview:
            self.assertEqual(preview.size, (888, 1560))
            for index, name in enumerate(names):
                with self.subTest(asset=name), Image.open(ASSETS / name) as image:
                    # 前 20 格（4 文本按钮 + 关闭 + 整理 = 5 组四态）按 4 列排；
                    # 之后从第 5 行起继续 4 列（与生成器 button_rows 布局一致）。
                    row = index // 4 if index < 20 else 5 + (index - 20) // 4
                    x, y = index % 4 * 222 + 4, row * 120 + 4
                    expected = image.resize((image.width * 3, image.height * 3), Image.Resampling.NEAREST)
                    self.assertEqual(preview.crop((x, y, x + expected.width, y + expected.height)).tobytes(),
                                     expected.tobytes())

    def test_script_encoding_and_manifests(self):
        for name in ('deploy.ps1', 'pack.ps1'):
            with self.subTest(script=name):
                raw = (ROOT / name).read_bytes()
                self.assertTrue(raw.startswith(b'\xef\xbb\xbf'))
                self.assertNotIn(b'\n', raw.replace(b'\r\n', b''))
                text = raw.decode('utf-8-sig')
                self.assertIn("@('open', 'close', 'take', 'put')", text)
                self.assertIn('foreach ($tab in 0..11)', text)
                self.assertIn('foreach ($state in 0..3)', text)
                self.assertIn("@('n', 's')", text)
                for direction in ('prev', 'next'):
                    for state in range(2):
                        self.assertIn(f"'arrow_{direction}{state}.pcx'", text)
                for suffix in ('n', 's'):
                    self.assertIn(f"'pager_{suffix}.pcx'", text)
                # round-13：整理四态与主按钮同一个 foreach 展开（tidy0..tidy3）。
                self.assertIn('foreach ($state in 0..3) { "tidy$state.pcx" }', text)
                self.assertRegex(text, r'\.Count -ne 50')
                self.assertIn('期望50个指定PCX', text)
                self.assertIn('Compare-Object $expectedAssets $assetNames', text)
                gate = text.index('Compare-Object $expectedAssets $assetNames')
                first_write = text.index('Copy-Item') if name == 'deploy.ps1' else text.index('$version = Get-PackVersion')
                self.assertLess(gate, first_write)

    def test_cpp_consumer_manifest(self):
        text = (ROOT / 'tests/warehouse_ui_pcx_test.cpp').read_text(encoding='utf-8')
        self.assertIn('tab < 12', text)
        self.assertIn('names.size() == 50', text)
        self.assertIn('nonEmpty == 50', text)
        for state in range(4):
            self.assertIn(f'"tidy{state}.pcx"', text)
        self.assertIn('name.find("tidy") == 0', text)
        self.assertNotIn('tidy_', text)
        self.assertNotRegex(text, r'\b46\b|\b48\b')
        for dimensions in ('width = 24; height = 24;', 'width = 66; height = 32;',
                           'width = 36; height = 22;', 'width = 18; height = 22;',
                           'width = 28; height = 22;'):
            self.assertIn(dimensions, text)
        # round-12：页码必须是 28 宽的独立分支，不能落回 36 宽的页签分支。
        self.assertIn('name.find("pager_") == 0', text)
        pager_branch = text.index('name.find("pager_") == 0')
        self.assertIn('width = 28; height = 22;', text[pager_branch:pager_branch + 200],
                      '页码必须是 28 宽独立分支，不能沿用 36 宽的页签分支')


if __name__ == '__main__':
    unittest.main(verbosity=2)
