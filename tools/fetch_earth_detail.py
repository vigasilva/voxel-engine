#!/usr/bin/env python3
"""Fetch local Copernicus GLO-30 heights and ESA WorldCover 2021 water via COG ranges.
Requires GDAL command-line tools and numpy; the engine reads the resulting PDEM offline.
"""
import argparse
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import urllib.request
import zlib
import numpy as np

ENV = dict(os.environ, GDAL_DISABLE_READDIR_ON_OPEN='TRUE', CPL_VSIL_CURL_ALLOWED_EXTENSIONS='.tif',
           GDAL_HTTP_TIMEOUT='30', GDAL_HTTP_MAX_RETRY='2', GDAL_HTTP_RETRY_DELAY='1', GDAL_CACHEMAX='32')

def run(*args):
    subprocess.run(args, env=ENV, check=True, timeout=240, stdout=subprocess.DEVNULL)

def axis(n, positive, negative, digits):
    return f'{positive if n >= 0 else negative}{abs(n):0{digits}d}'

def fetch(lat, lon, output):
    if not (-85 <= lat <= 85 and -179.9 <= lon < 179.9):
        raise ValueError('Detail latitude/longitude outside supported source range')
    # 0.1-degree cache cells with a 60 m height halo. Consistent pixel centers
    # keep adjacent files aligned, independent of where the request originated.
    latkey, lonkey = math.floor(lat * 10), math.floor(lon * 10)
    south, west = latkey / 10, lonkey / 10
    north, east = south + .1, west + .1
    output.mkdir(parents=True, exist_ok=True)
    target = output / f'{latkey}_{lonkey}.pdem'
    if target.exists() and target.stat().st_size <= 256 * 1024 * 1024:
        cached = target.read_bytes()
        if len(cached) >= 64:
            h = struct.unpack_from('<4s5I4dII', cached)
            expected = 64 + h[2] * h[3] * 4 + h[4] * h[5]
            if h[0] == b'PDEM' and h[1] == 1 and expected == len(cached) and zlib.crc32(cached[64:]) == h[10]:
                return target
        print(f'Replacing invalid cached terrain: {target}', flush=True)
    halo = 2 / 3600
    bounds = (west - halo, south - halo, east + halo, north + halo)
    with tempfile.TemporaryDirectory(prefix='earth-detail-') as tmp:
        tmp = Path(tmp)
        dem_sources, water_sources = [], []
        for y in range(math.floor(bounds[1]), math.floor(bounds[3]) + 1):
            for x in range(math.floor(bounds[0]), math.floor(bounds[2]) + 1):
                name = f'Copernicus_DSM_COG_10_{axis(y,"N","S",2)}_00_{axis(x,"E","W",3)}_00_DEM'
                dem_sources.append(f'/vsicurl/https://copernicus-dem-30m.s3.amazonaws.com/{name}/{name}.tif')
        for y in range(math.floor(bounds[1] / 3) * 3, math.floor(bounds[3] / 3) * 3 + 1, 3):
            for x in range(math.floor(bounds[0] / 3) * 3, math.floor(bounds[2] / 3) * 3 + 1, 3):
                name = f'ESA_WorldCover_10m_2021_v200_{axis(y,"N","S",2)}{axis(x,"E","W",3)}_Map.tif'
                water_sources.append(f'/vsicurl/https://esa-worldcover.s3.amazonaws.com/v200/2021/map/{name}')
        run('gdalbuildvrt', '-q', '-resolution', 'highest', '-allow_projection_difference', '-vrtnodata', '-9999', str(tmp/'dem.vrt'), *dem_sources)
        run('gdalbuildvrt', '-q', str(tmp/'water.vrt'), *water_sources)
        def crop(vrt, step, dtype, nodata, name):
            path = tmp / name
            nx = round((bounds[2] - bounds[0]) / step)
            ny = round((bounds[3] - bounds[1]) / step)
            run('gdal_translate', '-q', '-of', 'ENVI', '-ot', dtype, '-projwin',
                str(bounds[0]), str(bounds[3]), str(bounds[2]), str(bounds[1]),
                '-outsize', str(nx), str(ny), '-r', 'bilinear' if dtype == 'Float32' else 'near',
                '-a_nodata', str(nodata), str(vrt), str(path))
            info = json.loads(subprocess.check_output(['gdalinfo','-json',str(path)], env=ENV))
            nx, ny = info['size']
            transform = info['geoTransform']
            data = np.fromfile(path, dtype='<f4' if dtype == 'Float32' else 'u1').reshape(ny,nx)[::-1]
            return data.copy(), transform
        heights, transform = crop(tmp/'dem.vrt', 1/3600, 'Float32', -9999, 'height.bin')
        cover, _ = crop(tmp/'water.vrt', 1/10800, 'Byte', 0, 'cover.bin')
        valid = np.isfinite(heights) & (heights != -9999) & (heights >= -12000) & (heights <= 10000)
        if not valid.any():
            raise ValueError('No public Copernicus DEM coverage at requested location')
        quantized = np.where(valid, np.rint(heights * 100), -2147483648).astype('<i4')
        water = (cover == 80).astype('u1')
        payload = quantized.tobytes() + water.tobytes()
        ny, nx = heights.shape
        wy, wx = water.shape
        header = struct.pack('<4s5I4dII', b'PDEM',1,nx,ny,wx,wy,
                             transform[0] + transform[1]/2,
                             transform[3] + transform[5]*(ny-.5), transform[1], -transform[5],
                             zlib.crc32(payload),0)
        temporary = target.with_suffix('.tmp')
        temporary.write_bytes(header + payload)
        temporary.replace(target)
        print(f'{target}: {nx}x{ny} GLO-30 heights, {wx}x{wy} WorldCover water; {int(water.sum())} water pixels',flush=True)
    return target

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--lat', type=float, required=True)
    p.add_argument('--lon', type=float, required=True)
    p.add_argument('--output', type=Path, default=Path('data/earth_detail'))
    args = p.parse_args()
    fetch(args.lat,args.lon,args.output)

if __name__ == '__main__':
    main()
