"""Synthetic data only for subprocess/load/cleanup tests; never used by the app."""
import argparse
from pathlib import Path
import struct
import time
import zlib
p=argparse.ArgumentParser()
p.add_argument('--lat',type=float)
p.add_argument('--lon',type=float)
p.add_argument('--output',type=Path)
a=p.parse_args()
if a.lat>=32:
    time.sleep(600)
if a.lat>=31:
    raise SystemExit(1)
a.output.mkdir(parents=True,exist_ok=True)
data=struct.pack('<i',10000)*(364*364)+bytes([1])*(1092*1092)
header=struct.pack('<4s5I4dII',b'PDEM',1,364,364,1092,1092,29.999583333333333,29.999583333333333,1/3600,1/3600,zlib.crc32(data),0)
f=a.output/'300_300.pdem.tmp';f.write_bytes(header+data);f.replace(f.with_suffix(''))
