#!/usr/bin/env python3
"""Offline terrain serialization and actual GDAL crop/reprojection checks."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
import numpy as np

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
import terrain_pack as packs
import fetch_terrain as fetch

with tempfile.TemporaryDirectory(prefix='voxel-import-') as directory:
    tmp=Path(directory)
    target=tmp/'pack.pdem'
    meta=dict(provider='local',surface='bare-ground',source_datum='EGM2008',datum='EGM2008',
              source_spacing_m=1,vertical_accuracy_m=None,label='test survey',bounds=(10.000005,20.000005,10.000025,20.000025))
    h=np.array([[123.456,125,128],[np.nan,130,133],[135,136,137]],dtype='f4')
    cover=np.full((3,3),40,dtype='u1')
    packs.write_pack(target,h,cover,(10,20,.00001,.00001),meta)
    data=packs.read_pack(target)
    assert data and data['version']==3 and data['heights_cm'][0,0]==12346 and data['heights_cm'][1,0]==-2147483648
    assert data['metadata']['provider']==5 and data['metadata']['datum']==1
    assert data['metadata']['metadata_crc']==zlib.crc32(Path(str(target)+'.json').read_bytes())
    original=target.read_bytes()
    corrupt=bytearray(original);struct.pack_into('<d',corrupt,24,11)
    target.write_bytes(corrupt);assert packs.read_pack(target) is None # base axes protected by header CRC
    corrupt=bytearray(original);corrupt[-1]=7
    target.write_bytes(corrupt);assert packs.read_pack(target) is None
    target.write_bytes(original)
    try:
        packs.write_pack(target,np.full((2,2),np.nan),np.zeros((2,2)),(10,20,.001,.001),meta)
        raise AssertionError('Missing height grid accepted')
    except ValueError:
        pass
    assert target.read_bytes()==original # failed imports preserve the validated cache
    assert fetch.tile_bounds(40,-105,.25)[0]>fetch.tile_bounds(40,-105,1)[0]
    assert fetch.publication_day(None)==0 and fetch.publication_day('2022-02-10')==20220210
    # Deliberately projected, georeferenced elevation-only raster; this tests
    # coordinate reprojection rather than copying the importer's own output.
    if all(shutil.which(command) for command in ('gdal_translate','gdalwarp','gdalinfo','projinfo')):
        raw=tmp/'input.bin'
        np.full((33,33),125.5,dtype='<f4').tofile(raw)
        raw.with_suffix('.hdr').write_text('ENVI\nsamples = 33\nlines = 33\nbands = 1\nheader offset = 0\nfile type = ENVI Standard\ndata type = 4\ninterleave = bsq\nbyte order = 0\n')
        raster=tmp/'projected.tif'
        subprocess.run(['gdal_translate','-q','-of','GTiff','-a_srs','EPSG:3857+3855','-a_ullr','1113000','2273400','1113500','2272700',str(raw),str(raster)],check=True)
        product=dict(downloadURL=str(raster),title='projected synthetic survey',publicationDate='2026-01-01')
        source=fetch.raster_source(product,20)
        assert source['epsg']==3857 and source['datum']=='EGM2008' and source['spacing_m']>9
        values=fetch.warp_height(source,(9.999,19.999,10.001,20.001),17,17,tmp/'warped.bin')
        assert np.isfinite(values).all() and np.max(np.abs(values-125.5))<.001
        scaled=tmp/'scaled.tif'
        subprocess.run(['gdal_translate','-q','-of','GTiff','-a_scale','.1','-a_offset','10',str(raster),str(scaled)],check=True)
        source=fetch.raster_source(dict(product,downloadURL=str(scaled)),20)
        values=fetch.warp_height(source,(9.999,19.999,10.001,20.001),17,17,tmp/'scaled-warp.bin')
        assert np.isfinite(values).all() and np.max(np.abs(values-22.55))<.001
        without_vertical=tmp/'unknown-datum.tif'
        subprocess.run(['gdal_translate','-q','-of','GTiff','-a_srs','EPSG:3857',str(raster),str(without_vertical)],check=True)
        try:
            fetch.raster_source(dict(product,downloadURL=str(without_vertical)),20)
            raise AssertionError('Unknown vertical datum silently accepted')
        except ValueError:
            pass
print('PASS: protected terrain metadata, measured-value/nodata serialization, atomic failure preservation and projected GDAL elevation import')
