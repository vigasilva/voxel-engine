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
from unittest.mock import patch
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
    _,_,_,core,bounds=fetch.tile_bounds(89.895,15,2)
    assert abs((core[0]-bounds[0])*111319.49079327358*np.cos(np.deg2rad(89.895))-20)<.001
    assert fetch.publication_day(None)==0 and fetch.publication_day('2022-02-10')==20220210
    assert fetch.automatic_provider(78,15)=='arcticdem' and fetch.automatic_provider(-77,166)=='rema'
    assert fetch.automatic_provider(40,-105)=='usgs-3dep' and fetch.automatic_provider(20,15) is None
    collections=[dict(id='arcticdem-mosaics-v4.1-2m',license='CC-BY-4.0'),
                 dict(id='arcticdem-mosaics-v3.0-2m',license='CC-BY-4.0'),
                 dict(id='arcticdem-mosaics-v9.0-2m',license='proprietary'),
                 dict(id='arcticdem-mosaics-v4.1-10m',license='CC-BY-4.0')]
    url='https://pgc-opendata-dems.s3.us-west-2.amazonaws.com/arcticdem/mosaics/v4.1/2m/01_01/tile_dem.tif'
    feature=dict(id='test mosaic',collection=collections[0]['id'],geometry=None,
                 properties=dict(gsd=2,license='CC-BY-4.0',published='2023-06-01',start_datetime='2010-01-01',end_datetime='2020-01-01'),
                 assets=dict(dem=dict(href=url)))
    thumbnail=dict(feature,assets=dict(dem=dict(href=url.replace('_dem.tif','_browse.tif'))))
    restricted=dict(feature,properties=dict(feature['properties'],license='proprietary'))
    page=dict(features=[feature,thumbnail,restricted],links=[dict(rel='next',href=fetch.PGC_API+'search?page=2')])
    with patch.object(fetch,'request_json',side_effect=[page,dict(features=[feature],links=[])]) as requests:
        products=fetch.pgc_products('arcticdem',(15,78,15.01,78.01),collections)
        assert len(products)==1 and products[0]['source_epsg']==3413 and products[0]['publicationDate']=='2023-06-01'
        assert requests.call_count==2 and 'arcticdem-mosaics-v4.1-2m' in requests.call_args_list[0].args[0]
    with patch.object(fetch,'request_json',return_value=dict(features=[],links=[dict(rel='next',href='https://other.example/next')])):
        try:
            fetch.pgc_products('arcticdem',(15,78,15.01,78.01),collections)
            raise AssertionError('Unverified STAC pagination followed')
        except ValueError:
            pass
    catalogue=tmp/'.catalog.json'
    with patch.object(fetch,'request_json',return_value={}) as request:
        assert fetch.cached_catalogue(catalogue,lambda:[],False)==[]
        assert fetch.cached_catalogue(catalogue,lambda:request('unexpected'),False)==[]
        request.assert_not_called() # Empty coverage is cached too.
        assert fetch.cached_catalogue(catalogue,lambda:products,True)==products
    # Runtime fine requests beyond GLO-30's range must still import the polar
    # source, without requiring an unsupported global crop first.
    with patch.object(sys,'argv',['fetch_terrain','--lat','86.055','--lon','15.045','--output',str(tmp)]), \
         patch.object(fetch,'discover_polar_sources',return_value=[{}]), \
         patch.object(fetch,'import_sources') as importer, patch.object(fetch,'fetch_glo') as glo:
        fetch.main()
        assert importer.call_args.args[4:6]==('arcticdem','surface-model')
        glo.assert_not_called()
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
        # Verify the documented PGC reference becomes a genuine 3D CRS and
        # retains its nominal horizontal identity; no online geoid required.
        for epsg,lat,lon in ((3413,78,15),(3031,-77,166)):
            point=subprocess.check_output(['gdaltransform','-s_srs','EPSG:4326','-t_srs',f'EPSG:{epsg}'],
                                          input=f'{lon} {lat}\n',text=True).split()
            x,y=map(float,point[:2])
            polar=tmp/f'polar-{epsg}.tif'
            subprocess.run(['gdal_translate','-q','-of','GTiff','-a_srs',f'EPSG:{epsg}','-a_ullr',
                            str(x-33),str(y+33),str(x+33),str(y-33),str(raw),str(polar)],check=True)
            source=fetch.raster_source(dict(product,downloadURL=str(polar)),lat,default_datum='ellipsoid')
            assert source['epsg']==epsg and source['datum']=='ellipsoid' and source['spacing_m']==2
            horizontal,vertical=fetch.crs_parts(source['source_srs'])
            assert vertical is None and len(horizontal['coordinate_system']['axis'])==3
            transformed=subprocess.check_output(['gdaltransform','-s_srs',source['source_srs'],'-t_srs','EPSG:4979'],
                                                input=f'{x} {y} 125.5\n',text=True).split()
            assert abs(float(transformed[0])-lon)<1e-7 and abs(float(transformed[1])-lat)<1e-7 and float(transformed[2])==125.5
print('PASS: protected terrain metadata, measured-value/nodata serialization, atomic failure preservation and projected GDAL elevation import')
