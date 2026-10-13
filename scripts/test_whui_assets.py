# -*- coding: utf-8 -*-
"""独立素材验收：不导入生成器，不依赖构建或游戏部署。"""
import ast
from pathlib import Path
import re
import struct
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'whui'
TABS = ['常规', '卷轴', '装备', '头部', '肩部', '项链', '武器',
        '盾牌', '胸甲', '戒指', '靴子', '杂物']
EXPECTED = {
    **{f'btn_{button}{state}.pcx': ((24, 24) if button == 'close' else (66, 32))
       for button in ('open', 'close', 'take', 'put') for state in range(4)},
    **{f'tab{tab:02d}{state}.pcx': (44, 22) for tab in range(12) for state in ('n', 's')},
    **{f'arrow_{direction}{state}.pcx': (18, 22)
       for direction in ('prev', 'next') for state in range(2)},
    'pager_n.pcx': (36, 22),
    'pager_s.pcx': (36, 22),
}


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


class WarehouseAssetsTest(unittest.TestCase):
    def test_exact_manifest(self):
        self.assertEqual(len(EXPECTED), 46)
        self.assertEqual({p.name for p in ASSETS.glob('*.pcx')}, set(EXPECTED))
        for obsolete in ('title.pcx', 'tab12n.pcx', 'tab12s.pcx', 'tab13n.pcx', 'tab13s.pcx'):
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
        groups += [['pager_n.pcx', 'pager_s.pcx']]
        for names in groups:
            with self.subTest(group=names):
                self.assertEqual(len({(ASSETS / name).read_bytes() for name in names}), len(names))

    def test_generator_categories(self):
        tree = ast.parse((ROOT / 'scripts/make_whui_assets.py').read_text(encoding='utf-8-sig'))
        tabs = next(ast.literal_eval(node.value) for node in tree.body
                    if isinstance(node, ast.Assign) and any(
                        isinstance(target, ast.Name) and target.id == 'TABS' for target in node.targets))
        self.assertEqual(tabs, TABS)
        pager_size = None
        for node in tree.body:
            if not isinstance(node, ast.Assign):
                continue
            target_names = set()
            for target in node.targets:
                if isinstance(target, ast.Name):
                    target_names.add(target.id)
                elif isinstance(target, ast.Tuple):
                    target_names.update(elt.id for elt in target.elts
                                        if isinstance(elt, ast.Name))
            if {'PAGER_W', 'PAGER_H'} <= target_names:
                pager_size = ast.literal_eval(node.value)
        self.assertEqual(pager_size, (36, 22))
        pager_fn = next(node for node in tree.body
                        if isinstance(node, ast.FunctionDef) and node.name == 'make_pager')
        called = {call.func.id for call in ast.walk(pager_fn)
                  if isinstance(call, ast.Call) and isinstance(call.func, ast.Name)}
        self.assertNotIn('stamp_text', called)

    def test_preview_matches_every_asset(self):
        names = [f'btn_{key}{state}.pcx' for key in ('open', 'take', 'put', 'close')
                 for state in range(4)]
        names += [f'tab{tab:02d}{state}.pcx' for tab in range(12) for state in ('n', 's')]
        names += [f'arrow_{key}{state}.pcx' for key in ('prev', 'next') for state in range(2)]
        names += ['pager_n.pcx', 'pager_s.pcx']
        with Image.open(ASSETS / 'preview.png') as preview:
            self.assertEqual(preview.size, (888, 1440))
            for index, name in enumerate(names):
                with self.subTest(asset=name), Image.open(ASSETS / name) as image:
                    x, y = index % 4 * 222 + 4, index // 4 * 120 + 4
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
                self.assertRegex(text, r'\.Count -ne 46')
                self.assertIn('Compare-Object $expectedAssets $assetNames', text)
                gate = text.index('Compare-Object $expectedAssets $assetNames')
                first_write = text.index('Copy-Item') if name == 'deploy.ps1' else text.index('$version = Get-PackVersion')
                self.assertLess(gate, first_write)

    def test_cpp_consumer_uses_46(self):
        text = (ROOT / 'tests/warehouse_ui_pcx_test.cpp').read_text(encoding='utf-8')
        self.assertIn('tab < 12', text)
        self.assertIn('names.size() == 46', text)
        self.assertIn('nonEmpty == 46', text)
        self.assertNotRegex(text, r'\b48\b')
        for dimensions in ('width = 24; height = 24;', 'width = 66; height = 32;',
                           'width = 44; height = 22;', 'width = 18; height = 22;',
                           'width = 36; height = 22;'):
            self.assertIn(dimensions, text)


if __name__ == '__main__':
    unittest.main(verbosity=2)
