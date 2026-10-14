#!/usr/bin/env python3
"""Fetch bounded native ETOPO 2022 regional height maps, including one-sample borders."""
import argparse
import array
import math
from pathlib import Path
import re
import struct
import sys
import urllib.request
REGIONS = {'colorado': (39, 254), 'sydney': (-35, 150), 'himalaya': (27, 86)}
BASE = 'https://oceanwatch.pifsc.noaa.gov/erddap/griddap/ETOPO_2022_v1_15s.dods'

def fetch(name, output):
    lat, lon = REGIONS[name]
    row, column = (lat + 90) * 240 - 1, lon * 240 - 1
    url = f'{BASE}?z[{row}:1:{row + 482}][{column}:1:{column + 482}]'
    with urllib.request.urlopen(url, timeout=120) as response:
        data = response.read(2 * 1024 * 1024 + 1)
    if len(data) > 2 * 1024 * 1024:
        raise ValueError('Subset exceeded byte budget')
    header, raw = data.split(b'\nData:\n', 1)
    rows, cols = map(int, re.search(rb'Float32 z\[latitude = (\d+)\]\[longitude = (\d+)\]', header).groups())
    if (rows, cols) != (483, 483):
        raise ValueError('Unexpected subset dimensions')
    count = rows * cols
    if struct.unpack_from('>II', raw) != (count, count):
        raise ValueError('Bad XDR sample count')
    values = array.array('f'); values.frombytes(raw[8:8 + count * 4])
    if sys.byteorder == 'little': values.byteswap()
    at = 8 + count * 4
    if struct.unpack_from('>II', raw, at) != (rows, rows): raise ValueError('Bad latitude count')
    lat0, lat1 = struct.unpack_from('>dd', raw, at + 8)
    at += 8 + rows * 8
    if struct.unpack_from('>II', raw, at) != (cols, cols): raise ValueError('Bad longitude count')
    lon0, lon1 = struct.unpack_from('>dd', raw, at + 8)
    if len(raw) != at + 8 + cols * 8: raise ValueError('Trailing/missing data')
    quantized = array.array('h')
    for v in values:
        if not math.isfinite(v) or not -12000 <= v <= 10000: raise ValueError('Invalid elevation')
        quantized.append(round(v))
    if sys.byteorder == 'big': quantized.byteswap()
    payload = quantized.tobytes()
    h = bytearray(struct.pack('<4sIII4dII', b'EDEM', 1, rows, cols, lat0, lon0, lat1-lat0, lon1-lon0, 0, 0))
    checksum = 2166136261
    for byte in h[:48] + h[52:] + payload:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    struct.pack_into('<I', h, 48, checksum)
    output.mkdir(parents=True, exist_ok=True)
    (output / f'{name}.edem').write_bytes(h + payload)
    print(f'{name}: {rows}x{cols}, {len(payload)} bytes, {lat0:.6f}/{lon0:.6f}, heights {min(values):.0f}..{max(values):.0f} m', flush=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--region', choices=list(REGIONS), action='append')
    parser.add_argument('--output', type=Path, default=Path('data/earth_regions'))
    args = parser.parse_args()
    for region in args.region or REGIONS:
        fetch(region, args.output)
if __name__ == '__main__': main()
