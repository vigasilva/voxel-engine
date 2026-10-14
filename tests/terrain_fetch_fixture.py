"""Synthetic data only for subprocess/load/cleanup tests; never used by the app."""
import argparse
from pathlib import Path
import struct
import time
import math
import zlib
p=argparse.ArgumentParser()
p.add_argument('--lat',type=float)
p.add_argument('--lon',type=float)
p.add_argument('--output',type=Path)
p.add_argument('--quality',default='glo30')
a=p.parse_args()
if a.quality == 'auto':
    if a.lat > 40.06:
        raise SystemExit(0) # Verified absence of a fine source; no repeated requests.
    a.output.mkdir(parents=True,exist_ok=True)
    n=64;dx=1/(111319.49079327358*math.cos(math.radians(a.lat)));dy=1/111319.49079327358
    lon0=a.lon-(n-1)*dx/2;lat0=a.lat-(n-1)*dy/2
    header=bytearray(256)
    struct.pack_into('<4s5I4dII',header,0,b'PDEM',3,n,n,n,n,lon0,lat0,dx,dy,0,256)
    struct.pack_into('<4I',header,64,2,1,1,1)
    struct.pack_into('<2d2I',header,80,1,float('nan'),20200101,0)
    header[104:116]=b'TEST SURVEY\0'
    struct.pack_into('<4d',header,184,lon0,lat0,lon0+(n-1)*dx,lat0+(n-1)*dy)
    struct.pack_into('<4Id',header,216,0,399,0,4326,float('nan'))
    struct.pack_into('<d',header,240,1)
    struct.pack_into('<I',header,224,zlib.crc32(header[228:256],zlib.crc32(header[60:224],zlib.crc32(header[:56]))))
    payload=struct.pack('<i',140000)*(n*n)+bytes([30])*(n*n)
    struct.pack_into('<I',header,56,zlib.crc32(payload,zlib.crc32(header[64:])))
    f=a.output/'fine.pdem.tmp';f.write_bytes(header+payload);f.replace(f.with_suffix(''))
    raise SystemExit(0)
if a.lat>=32:
    time.sleep(600)
if a.lat>=31:
    raise SystemExit(1)
a.output.mkdir(parents=True,exist_ok=True)
data=struct.pack('<i',10000)*(364*364)+bytes([1])*(1092*1092)
header=struct.pack('<4s5I4dII',b'PDEM',1,364,364,1092,1092,29.999583333333333,29.999583333333333,1/3600,1/3600,zlib.crc32(data),0)
f=a.output/'300_300.pdem.tmp';f.write_bytes(header+data);f.replace(f.with_suffix(''))
