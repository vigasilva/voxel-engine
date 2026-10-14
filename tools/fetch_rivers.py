#!/usr/bin/env python3
"""Convert Natural Earth 1:10 million river centerlines to a compact global layer.
Widths are visual/model estimates from cartographic rank, not surveyed river banks.
Requires ogr2ogr only for conversion; the engine reads RIVR offline.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import urllib.request
import zipfile
import zlib
URL='https://naturalearth.s3.amazonaws.com/10m_physical/ne_10m_rivers_lake_centerlines.zip'
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-json',type=Path)
    p.add_argument('--output',type=Path,default=Path('data/earth_rivers.rivr'))
    args=p.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        tmp=Path(tmp)
        if args.source_json: source=args.source_json
        else:
            with urllib.request.urlopen(URL,timeout=90) as r: data=r.read(8*1024*1024+1)
            if len(data)>8*1024*1024: raise ValueError('Unexpected Natural Earth download size')
            archive=tmp/'rivers.zip';archive.write_bytes(data)
            with zipfile.ZipFile(archive) as z:
                for ext in ('shp','shx','dbf','prj'):
                    name=f'ne_10m_rivers_lake_centerlines.{ext}'
                    (tmp/name).write_bytes(z.read(name))
            source=tmp/'rivers.json'
            subprocess.run(['ogr2ogr','-f','GeoJSON',str(source),str(tmp/'ne_10m_rivers_lake_centerlines.shp')],check=True)
        segments=[]
        for feature in json.loads(source.read_text())['features']:
            if feature['properties'].get('featurecla')!='River': continue
            rank=feature['properties'].get('scalerank') or 1
            width=max(15,800/(1.7**rank))
            lines=feature['geometry']['coordinates']
            if feature['geometry']['type']=='LineString': lines=[lines]
            for line in lines:
                for a,b in zip(line,line[1:]):
                    if abs(a[0]-b[0])>180: continue
                    steps=max(1,math.ceil(max(abs(a[0]-b[0]),abs(a[1]-b[1]))/.05))
                    for i in range(steps):
                        t,u=i/steps,(i+1)/steps
                        segments.append(struct.pack('<5f',a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,
                                                   a[0]+(b[0]-a[0])*u,a[1]+(b[1]-a[1])*u,width))
        data=b''.join(segments)
        args.output.write_bytes(struct.pack('<4sIII',b'RIVR',1,len(segments),zlib.crc32(data))+data)
        print(f'{len(segments)} global river segments, {len(data)} payload bytes')
if __name__=='__main__':main()
