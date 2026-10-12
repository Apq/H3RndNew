"""Independent warehouse wire-format golden generator (Python standard library).
Run from any directory: python tests/warehouse_golden.py --check
Without --check, creates the fixture exclusively; never overwrites an existing one.
"""
from pathlib import Path
import hashlib
import struct
import sys
import zlib

prefix = b'native-fixture-not-a-real-GM1'
owner = b'H3RndNew.WH' + b'\0' * 5
stocks = [[] for _ in range(156)]
stocks[0] = [(1, 7), (1, 8), (42, -1), (42, -1)]
stocks[155] = [(143, -1)]
payload = b'H3WHDAT1' + b''.join(
    struct.pack('<IQ', hero, len(records))
    + b''.join(struct.pack('<ii', *record) for record in records)
    for hero, records in enumerate(stocks))


def common(magic, size):
    return struct.pack('<8s16sIIQQQ32s', magic, owner, 1, size,
                       len(prefix), len(payload), len(prefix),
                       hashlib.sha256(prefix).digest())


header = common(b'H3WHBGN1', 96) + struct.pack('<I', 0)
header_crc = zlib.crc32(header)
header += struct.pack('<I', header_crc)
footer = common(b'H3WHFIN1', 104) + struct.pack('<III', zlib.crc32(payload), header_crc, 0)
footer += struct.pack('<I', zlib.crc32(footer))
assert len(header) == 96 and len(footer) == 104
blob = prefix + header + payload + footer
out = Path(__file__).resolve().parent / 'fixtures' / 'warehouse_v1.hex'
if '--check' in sys.argv:
    if bytes.fromhex(out.read_text(encoding='ascii')) != blob:
        raise SystemExit('FAIL: warehouse golden differs from independent encoder')
else:
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('xb') as stream:
        stream.write(blob.hex().encode('ascii') + b'\r\n')
print('PASS warehouse v1 golden:', len(blob), 'bytes; SHA256', hashlib.sha256(blob).hexdigest())
