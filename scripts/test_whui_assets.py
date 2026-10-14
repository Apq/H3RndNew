# -*- coding: utf-8 -*-
"""独立素材验收：不依赖构建或游戏部署。

整理按钮字形差分一项例外地导入生成器模块（仅重建“无字底板”，main() 有
__main__ 门不会出图），其余测试仍不导入生成器。"""
import ast
from pathlib import Path
import re
import struct
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'whui'
SRC = ROOT / 'whui_src'
# 出图按 TabKind 枚举规范序（固定），显示顺序由运行期 [WarehouseUI] TabOrder 决定。
TABS = ['常规', '卷轴', '装备', '头部', '肩部', '项链', '武器',
        '盾牌', '胸甲', '戒指', '靴子', '杂物']
# round-12：页码按两位数定宽 28x22；两字页签与整理按钮仍是 36x22。
PAGER_WIDTH, PAGER_HEIGHT = 28, 22
TIDY_WIDTH, TIDY_HEIGHT = 36, 22
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


def mirror_tile(patch, width, height):
    """镜像平铺：与生成器同规则，独立实现以便自建无字底板。"""
    out = Image.new('RGB', (width, height))
    tile_w, tile_h = patch.size
    for oy in range(0, height, tile_h):
        for ox in range(0, width, tile_w):
            tile = patch
            if (ox // tile_w) % 2:
                tile = tile.transpose(Image.FLIP_LEFT_RIGHT)
            if (oy // tile_h) % 2:
                tile = tile.transpose(Image.FLIP_TOP_BOTTOM)
            out.paste(tile, (ox, oy))
    return out


def scale_brightness(image, factors):
    """两态亮度：按生成器的 int(v*f + 0.5) 后钳制，不用 Python round（半值取偶会偏）。"""
    src = image.load()
    out = Image.new('RGB', image.size)
    dst = out.load()
    for y in range(image.height):
        for x in range(image.width):
            dst[x, y] = tuple(max(0, min(255, int(src[x, y][i] * factors[i] + 0.5)))
                              for i in range(3))
    return out


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


def no_text_plate(width, height, selected):
    """不导入生成器，从母版 whui_src/bv_ok0.pcx 的 3,3,10,10 7x7 块自建同纹理无字底板。

    页码在 round-12 收窄到 28x22，整理仍是 36x22，两者不再共用同一块底板，
    所以整理字形差分不能再直接拿 whui/pager_*.pcx 当参照——这里按生成器相同的
    纹理取样、镜像平铺、亮度取整与金框规则，独立重建 36x22 同纹理无字板。"""
    with Image.open(SRC / 'bv_ok0.pcx') as master:
        patch = master.convert('RGB').crop((3, 3, 10, 10))
    factors = (1.35, 1.32, 1.28) if selected else (0.92, 0.92, 0.92)
    fill = scale_brightness(mirror_tile(patch, width - 2, height - 2), factors)
    canvas = Image.new('RGB', (width, height))
    canvas.paste(fill, (1, 1))
    gold_border(canvas)
    return canvas


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
        # round-13：整理与主按钮同一构建管线（清勾母版 + 九宫格 + 参考框 + 文字）。
        self.assertEqual(called_names('make_tidy'),
                         {'clean_ok_base', 'nine_slice', 'apply_reference_frame',
                          'stamp_text'})
        self.assertIn('"tidy%d.pcx" % index', source)
        self.assertIn('make_tidy(state)', source)

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
        本文件独立重建的纹理/亮度/金框规则与生产生成器等价。"""
        for suffix, selected in (('n', False), ('s', True)):
            with self.subTest(state=suffix):
                with Image.open(ASSETS / f'pager_{suffix}.pcx') as pager:
                    produced = pager.convert('RGB')
                self.assertEqual(produced.size, (PAGER_WIDTH, PAGER_HEIGHT))
                expected = no_text_plate(PAGER_WIDTH, PAGER_HEIGHT, selected)
                self.assertEqual(produced.tobytes(), expected.tobytes(),
                                 '独立重建的底板必须与生产页码出图完全一致')

    def test_tidy_states_follow_button_pipeline(self):
        """整理四态必须与主按钮同一管线（round-13 用户裁定）。

        底板 = 对应态 bv_ok 清勾九宫格 + 参考金框。此项例外地导入生成器模块
        重建“无字底板”（main() 有 __main__ 门，不会出图），验证的组合事实是：
        make_tidy 只在底板上叠“整理”两个字——差分区域必须小、居中、左右两半
        各有足量笔画、比底板亮；四态同字形，按压态文字整体下移 1px。"""

        import make_whui_assets as gen
        states = list(gen.STATE_ORDER)
        plates, images, regions = {}, {}, {}
        for index, state in enumerate(states):
            plates[state] = gen.apply_reference_frame(
                gen.nine_slice(gen.clean_ok_base(index), TIDY_WIDTH, TIDY_HEIGHT))
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
