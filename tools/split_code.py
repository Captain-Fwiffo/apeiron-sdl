#!/usr/bin/env python3
"""Writes the CODE resources of the user's own Apeiron file to code0.bin .. code9.bin.

The verify\musashi tests load these files (their first argument is the folder that holds them).
    python tools/split_code.py <the Apeiron file: ._Apeiron, Apeiron.rsrc or a raw resource fork> <output folder>
"""
import os, struct, sys

def fork_of(path):
    d = open(path, 'rb').read()
    if d[:4] == b'\x00\x05\x16\x07':              # AppleDouble: entry 2 is the resource fork
        n = struct.unpack('>H', d[24:26])[0]
        for i in range(n):
            eid, off, ln = struct.unpack('>III', d[26 + i * 12:38 + i * 12])
            if eid == 2:
                return d[off:off + ln]
        raise SystemExit('no resource fork in ' + path)
    return d                                       # a raw fork

def code_resources(res):
    data_off, map_off, _, map_len = struct.unpack('>IIII', res[:16])
    m = res[map_off:map_off + map_len]
    type_list = struct.unpack('>H', m[24:26])[0]
    for i in range(struct.unpack('>H', m[type_list:type_list + 2])[0] + 1):
        t, cnt, ref_off = struct.unpack('>4sHH', m[type_list + 2 + i * 8:type_list + 10 + i * 8])
        if t != b'CODE':
            continue
        for j in range(cnt + 1):
            e = m[type_list + ref_off + j * 12:type_list + ref_off + j * 12 + 12]
            rid = struct.unpack('>h', e[:2])[0]
            off = (e[5] << 16) | (e[6] << 8) | e[7]
            ln = struct.unpack('>I', res[data_off + off:data_off + off + 4])[0]
            yield rid, res[data_off + off + 4:data_off + off + 4 + ln]

if len(sys.argv) != 3:
    raise SystemExit(__doc__)
os.makedirs(sys.argv[2], exist_ok=True)
for rid, body in code_resources(fork_of(sys.argv[1])):
    with open(os.path.join(sys.argv[2], 'code%d.bin' % rid), 'wb') as f:
        f.write(body)
    print('code%d.bin  %d bytes' % (rid, len(body)))
