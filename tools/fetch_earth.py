#!/usr/bin/env python3
"""Download a compact NOAA ETOPO 2022 global sample; no runtime network dependency."""
import argparse
import array
from pathlib import Path
import math
import re
import struct
import sys
import urllib.request
URL = 'https://oceanwatch.pifsc.noaa.gov/erddap/griddap/ETOPO_2022_v1_15s.dods?z[0:40:43199][0:40:86399]'
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-file', type=Path)
    parser.add_argument('--output', type=Path, default=Path('data/earth_10min.edem'))
    parser.add_argument("--stride", type=int, choices=(20,40), default=40)
    args = parser.parse_args()
    url = URL.replace("0:40:", f"0:{args.stride}:")
    byte_limit = 48 * 1024 * 1024
    if args.source_file:
        data = args.source_file.read_bytes()
    else:
        with urllib.request.urlopen(url, timeout=180) as response:
            data = response.read(byte_limit + 1)
    if len(data) > byte_limit:
        raise ValueError('Unexpectedly large NOAA subset')
    header, raw = data.split(b'\nData:\n', 1)
    dimensions = re.search(rb'Float32 z\[latitude = (\d+)\]\[longitude = (\d+)\]', header)
    rows, columns = map(int, dimensions.groups())
    if (rows, columns) != (43200 // args.stride, 86400 // args.stride):
        raise ValueError('Unexpected NOAA grid')
    count = rows * columns
    if struct.unpack_from('>II', raw) != (count, count):
        raise ValueError('Invalid XDR elevation length')
    samples = array.array('f'); samples.frombytes(raw[8:8 + count * 4])
    if sys.byteorder == 'little': samples.byteswap()
    at = 8 + count * 4
    if struct.unpack_from('>II', raw, at) != (rows, rows): raise ValueError('Invalid latitude count')
    lat0, lat1 = struct.unpack_from('>dd', raw, at + 8)
    at += 8 + rows * 8
    if struct.unpack_from('>II', raw, at) != (columns, columns): raise ValueError('Invalid longitude count')
    lon0, lon1 = struct.unpack_from('>dd', raw, at + 8)
    if len(raw) != at + 8 + columns * 8: raise ValueError('Unexpected trailing data')
    quantized = array.array('h')
    for value in samples:
        if not math.isfinite(value) or value < -12000 or value > 10000: raise ValueError('Invalid elevation')
        quantized.append(round(value))
    if sys.byteorder == 'big': quantized.byteswap()
    payload = quantized.tobytes()
    output_header = bytearray(struct.pack('<4sIII4dII', b'EDEM', 1, rows, columns, lat0, lon0, lat1-lat0, lon1-lon0, 0, 0))
    checksum = 2166136261
    for byte in output_header[:48] + output_header[52:] + payload:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    struct.pack_into('<I', output_header, 48, checksum)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output_header + payload)
    print(f'NOAA ETOPO 2022: {rows} x {columns}, {len(payload):,} elevation bytes, {args.stride / 4:g} arc-minute sample, min/max {min(samples):.0f}/{max(samples):.0f} m')
if __name__ == '__main__': main()
