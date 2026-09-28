#!/usr/bin/env python3
"""Stage 4 (docs/stage4-enroll.md): compare two packed ChicagoHS galleries field by field.

    ./cmp_tpl.py <dll.tpl> <port.tpl> [-v]

Both files are templatePack blobs (algo_eval4/oracle_feat TPL_OUT=..., port_eval TPL_OUT=...).
The matcher only ever sees a fresh templateUnPack of this blob (algo_eval4 protocol), so equal
blobs mean equal gallery input to identifyImage.  The TLV layout (AlgoChicago 0x34e60, type 24):
tag byte + u32; containers/blobs carry a length, other tags a scalar value.
Prints the first differing field, then per-field counts; exit 1 if anything differs.
The top-level CRC (tag 0x87) is reported separately: it only follows the body.
"""
import struct
import sys
from collections import Counter

CONTAINERS = {0x86, 0x95, 0x96, 0x93, 0x94, 0xb2, 0xcf, 0xcd}
BLOBS = {0xce, 0xb4, 0xc5, 0xa1, 0xa3, 0xa4}
NAMES = {0x91: 'n_sub', 0x92: 'n_rel', 0x97: 'capacity', 0xb2: 'primary', 0xcf: 'secondary',
         0xcd: 'validity', 0xce: 'coarse', 0xb3: 'n_rec', 0xb4: 'recs', 0xb5: 'group_state',
         0xb6: 'relation_base', 0xb7: 'active', 0xb8: 'quality', 0xb9: 'coverage',
         0xba: 'study_state', 0xbb: 'study_flags', 0xbc: 'lineage', 0xbd: 'replacements',
         0xbe: 'study_a', 0xc0: 'study_b', 0xc7: 'packed_res', 0xe3: 'index', 0xe1: 'inliers',
         0xa1: 'match_order', 0xa2: 'matcher_active', 0xa5: 'mval_a', 0xa6: 'mval_b',
         0xa7: 'mval_c', 0xa8: 'mval_d'}
for k in range(6):
    NAMES[0xe4 + k] = f't{k}'


def name(tag):
    return NAMES.get(tag, f'{tag:02x}')


def parse(buf, off, end, path, out, counters):
    while off < end:
        tag = buf[off]
        (val,) = struct.unpack_from('<I', buf, off + 1)
        off += 5
        if tag in CONTAINERS:
            key = {0x95: 'sub', 0x96: 'rel'}.get(tag)
            if key:
                p = f'{path}{key}[{counters[key]}]'
                counters[key] += 1
            else:
                p = f'{path}{name(tag)}'
            parse(buf, off, off + val, p + '.', out, Counter())
            off += val
        elif tag in BLOBS:
            data = buf[off:off + val]
            if tag == 0xb4:   # feature records, 32 bytes: u32 x<<16|y<<4|orient, 28 descriptor
                for r in range(val // 32):
                    rec = data[r * 32:r * 32 + 32]
                    out.append((f'{path}rec[{r}].pos', rec[:4]))
                    out.append((f'{path}rec[{r}].desc', rec[4:]))
            elif tag == 0xa1:
                for r in range(val // 4):
                    out.append((f'{path}match_order[{r}]', data[r * 4:r * 4 + 4]))
            else:
                out.append((f'{path}{name(tag)}', data))
            off += val
        elif tag == 0x87:
            out.append(('crc', struct.pack('<I', val)))
        else:
            out.append((f'{path}{name(tag)}', struct.pack('<i', struct.unpack('<i', struct.pack('<I', val))[0])))
    return off


def load(fn):
    buf = open(fn, 'rb').read()
    out = []
    parse(buf, 0, len(buf), '', out, Counter())
    return out


def show(v):
    if len(v) == 4:
        return str(struct.unpack('<i', v)[0])
    return v.hex()[:48] + ('…' if len(v) > 24 else '')


def field(path):
    """generic field name: sub[3].rec[5].pos -> rec.pos"""
    import re
    return re.sub(r'\[\d+\]', '', path).replace('sub.', '').replace('rel.', 'rel:')


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    verbose = '-v' in sys.argv
    da, db = dict(a), dict(b)
    crc_diff = da.get('crc') != db.get('crc')
    keys = [k for k, _ in a] + [k for k, _ in b if k not in da]
    diffs = [k for k in keys if k != 'crc' and da.get(k) != db.get(k)]
    per = Counter(field(k) for k in diffs)
    print(f'fields: dll {len(a)}, port {len(b)}; differing (without crc): {len(diffs)}; crc differs: {crc_diff}')
    for k in diffs[:40 if verbose else 8]:
        print(f'  {k:40s} dll={show(da[k]) if k in da else "-":>14s} port={show(db[k]) if k in db else "-":>14s}')
    if per:
        print('per field: ' + ', '.join(f'{f}={n}' for f, n in sorted(per.items())))
    return 1 if diffs or crc_diff else 0


if __name__ == '__main__':
    sys.exit(main())
