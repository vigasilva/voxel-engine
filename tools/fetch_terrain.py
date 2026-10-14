#!/usr/bin/env python3
"""Fetch measured terrain at its source resolution, with verified vertical CRS.
USGS 3DEP and public PGC polar mosaics supplement the global GLO-30 cache.
Only small georeferenced cells are read from remote COGs; no full survey download.
"""
import argparse
import datetime
import json
import math
from pathlib import Path
import re
import sys
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request
import numpy as np
from fetch_earth_detail import ENV, axis, fetch as fetch_glo
from terrain_pack import read_pack, write_pack, atomic_write

ENV = dict(ENV, PROJ_NETWORK='ON', GDAL_NUM_THREADS='1')
DATASETS = ['Original Product Resolution (OPR) Digital Elevation Model (DEM)',
            'Digital Elevation Model (DEM) 1 meter', 'Seamless 1-m DEM (S1M)']
API = 'https://tnmaccess.nationalmap.gov/api/v1/products'
PROVIDER_DOC = 'https://www.usgs.gov/3d-elevation-program/about-3dep-products-services'
PGC_API = 'https://stac.pgc.umn.edu/api/v1/'
PGC_DOC = 'https://www.pgc.umn.edu/guides/stereo-derived-elevation-models/pgc-dem-products-arcticdem-rema-and-earthdem/'


def polar_acknowledgements(provider):
    if provider not in ('arcticdem','rema'):
        return None
    return dict(institutions=['Polar Geospatial Center, University of Minnesota']+
                (['Byrd Polar and Climate Research Center, The Ohio State University'] if provider=='rema' else []),
                funding='NSF-OPP',
                awards=[1043681,1559691,1542736,1810976,2129685] if provider=='arcticdem' else
                       [1043681,1542736,1543501,1559691,1810976,2129685],
                derived_from='Vantor stereo imagery via SETSM; source imagery is not included',
                policy='https://www.pgc.umn.edu/guides/user-services/acknowledgement-policy/')


def automatic_provider(lat, lon):
    if 24 <= lat <= 50 and -125 <= lon <= -66:
        return 'usgs-3dep'
    if lat >= 60:
        return 'arcticdem'
    if lat <= -60:
        return 'rema'
    return None


def cached_catalogue(cache, fetch, refresh=False):
    if cache and cache.exists() and not refresh and cache.stat().st_size <= 8*1024*1024:
        try:
            saved = json.loads(cache.read_text())
            if 0 <= time.time()-saved['retrieved_unix'] < 7*86400 and isinstance(saved['products'], list):
                return saved['products']
        except (KeyError, TypeError, ValueError, OSError):
            pass
    products = fetch()
    if cache:
        cache.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(cache, (json.dumps(dict(retrieved_unix=time.time(), products=products), allow_nan=False)+'\n').encode())
    return products


def run(*args):
    subprocess.run(args, env=ENV, check=True, timeout=150, stdout=subprocess.DEVNULL)


def command_json(*args):
    return json.loads(subprocess.check_output(args, env=ENV, timeout=60, text=True))


def request_json(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'voxel-engine terrain importer'}), timeout=30) as response:
        data = response.read(8*1024*1024+1)
    if len(data) > 8*1024*1024:
        raise ValueError('Terrain catalogue response exceeds bounded metadata size')
    result = json.loads(data)
    if result.get('errors'):
        raise ValueError(f'Terrain catalogue error: {result["errors"]}')
    return result


def usgs_products(bounds):
    products = {}
    for dataset in DATASETS:
        offset = 0
        while True:
            params = dict(datasets=dataset, bbox=','.join(map(str, bounds)), prodFormats='GeoTIFF',
                          max=100, offset=offset, outputFormat='JSON')
            page = request_json(API+'?'+urllib.parse.urlencode(params))
            items = page.get('items', [])
            for item in items:
                url = item.get('downloadURL', '')
                parsed = urllib.parse.urlparse(url)
                if parsed.scheme != 'https' or parsed.netloc != 'prd-tnm.s3.amazonaws.com' or not parsed.path.lower().endswith('.tif'):
                    continue
                products[url] = dict(item, dataset=dataset)
            offset += len(items)
            if not items or offset >= page.get('total', offset):
                break
    return list(products.values())


def crs_parts(wkt):
    value = command_json('projinfo', '-q', '-o', 'PROJJSON', wkt)
    parts = value.get('components', [value])
    horizontal = next((p for p in parts if p['type'] in ('ProjectedCRS', 'GeographicCRS')), None)
    vertical = next((p for p in parts if p['type'] == 'VerticalCRS'), None)
    return horizontal, vertical


def unit_factor(unit):
    if isinstance(unit, dict):
        return float(unit.get('conversion_factor', 1))
    if unit in ('metre', 'meter', 'm'):
        return 1
    if unit in ('US survey foot', 'US survey feet'):
        return 1200/3937
    if unit in ('foot', 'feet', 'ft'):
        return .3048
    raise ValueError(f'Unsupported horizontal unit: {unit}')


def raster_source(product, latitude, source_srs=None, default_datum=None):
    url = product['downloadURL']
    path = '/vsicurl/'+url if url.startswith('https://') else url
    info = command_json('gdalinfo', '-json', path)
    if len(info.get('bands', [])) != 1:
        raise ValueError('Terrain must be a single-band elevation raster')
    wkt = source_srs or info.get('coordinateSystem', {}).get('wkt')
    if not wkt:
        raise ValueError('Terrain has no declared horizontal CRS')
    horizontal, vertical = crs_parts(wkt)
    if horizontal is None:
        raise ValueError('Terrain horizontal CRS cannot be identified')
    epsg = horizontal.get('id', {}).get('code', 0) if horizontal.get('id', {}).get('authority') == 'EPSG' else 0
    datum_codes = {3855: 'EGM2008', 5703: 'NAVD88', 5773: 'EGM1996'}
    if vertical:
        datum = datum_codes.get(vertical.get('id', {}).get('code'))
        if not datum:
            datum = 'unknown' # The full declared CRS is retained and strictly transformed.
    elif len(horizontal['coordinate_system']['axis']) == 3:
        datum = 'ellipsoid'
    elif default_datum == 'ellipsoid':
        # PGC explicitly documents WGS84 ellipsoidal heights, although its
        # TIFFs tag only a 2D polar CRS. Promote that verified CRS to 3D.
        if epsg not in (3413, 3031):
            raise ValueError('PGC ellipsoid reference requires the documented WGS84 polar CRS')
        datum = 'ellipsoid'
        wkt = subprocess.check_output(['projinfo', '-q', '-o', 'WKT2:2019', '--3d', wkt],
                                      env=ENV, timeout=60, text=True).strip()
    elif default_datum:
        datum = default_datum
        vcode = {'EGM2008': 3855, 'NAVD88': 5703, 'EGM1996': 5773}[datum]
        if not epsg:
            raise ValueError('Missing compound CRS; specify it explicitly for an unregistered horizontal CRS')
        wkt = f'EPSG:{epsg}+{vcode}'
    else:
        raise ValueError('Unknown vertical datum; an explicit source CRS is required')
    gt = info['geoTransform']
    if horizontal['type'] == 'ProjectedCRS':
        factor = unit_factor(horizontal['coordinate_system']['axis'][0]['unit'])
        spacing = max(math.hypot(gt[1], gt[4]), math.hypot(gt[2], gt[5]))*factor
    else:
        spacing = max(abs(gt[1])*max(.001, abs(math.cos(math.radians(latitude)))), abs(gt[5]))*111319.49079327358
    if not math.isfinite(spacing) or not 0 < spacing <= 100000:
        raise ValueError('Invalid measured raster spacing')
    return dict(product=product, path=path, source_srs=wkt, datum=datum, epsg=epsg,
                spacing_m=spacing, info=info)


def tile_bounds(lat, lon, spacing):
    # Decrease cell area for sub-meter surveys; source resolution is retained.
    scale = 100
    while 111319.49079327358/scale/spacing > 1400:
        scale *= 2
    latkey, lonkey = math.floor(lat*scale), math.floor(lon*scale)
    core = (lonkey/scale, latkey/scale, (lonkey+1)/scale, (latkey+1)/scale)
    halo_y = max(20, spacing*2)/111319.49079327358
    halo_x = halo_y/max(.001, abs(math.cos(math.radians(lat))))
    bounds = (max(-180,core[0]-halo_x),max(-90,core[1]-halo_y),
              min(180,core[2]+halo_x),min(90,core[3]+halo_y))
    return scale, latkey, lonkey, core, bounds


def warp_height(source, bounds, nx, ny, path):
    # Explicit vertical CRS plus ONLY_BEST/ALLOW_BALLPARK prevents silently
    # treating NAVD88/ellipsoidal elevations as EGM2008 when grids are missing.
    source_path=source['path']
    band=source['info']['bands'][0]
    if band.get('scale',1) != 1 or band.get('offset',0) != 0:
        unscaled=Path(str(path)+'.source.vrt')
        run('gdal_translate','-q','-of','VRT','-ot','Float32','-unscale',source_path,str(unscaled))
        source_path=str(unscaled)
    run('gdalwarp','-q','-overwrite','-s_srs',source['source_srs'],'-t_srs','EPSG:4326+3855',
        '-to','ONLY_BEST=YES','-to','ALLOW_BALLPARK=NO','-vshift','-te',*map(str,bounds),
        '-ts',str(nx),str(ny),'-et','0','-wm','32','-ot','Float32','-of','ENVI',
        '-dstnodata','-999999','-r','bilinear',source_path,str(path))
    values = np.fromfile(path, dtype='<f4').reshape(ny,nx)[::-1].copy()
    values[(~np.isfinite(values)) | (values < -12000) | (values > 10000)] = np.nan
    return values


def crop_cover(bounds, tmp):
    sources = []
    for y in range(math.floor(bounds[1]/3)*3, math.floor(bounds[3]/3)*3+1, 3):
        for x in range(math.floor(bounds[0]/3)*3, math.floor(bounds[2]/3)*3+1, 3):
            name = f'ESA_WorldCover_10m_2021_v200_{axis(y,"N","S",2)}{axis(x,"E","W",3)}_Map.tif'
            sources.append('/vsicurl/https://esa-worldcover.s3.amazonaws.com/v200/2021/map/'+name)
    vrt = tmp/'cover.vrt'
    run('gdalbuildvrt','-q',str(vrt),*sources)
    wx, wy = max(2,round((bounds[2]-bounds[0])*10800)),max(2,round((bounds[3]-bounds[1])*10800))
    path = tmp/'cover.bin'
    run('gdalwarp','-q','-t_srs','EPSG:4326','-of','ENVI','-ot','Byte','-r','near','-dstnodata','0',
        '-te',*map(str,bounds),'-ts',str(wx),str(wy),'-wm','32',str(vrt),str(path))
    return np.fromfile(path,dtype='u1').reshape(wy,wx)[::-1].copy()


def publication_day(text):
    try:
        return int(datetime.date.fromisoformat(text[:10]).strftime('%Y%m%d'))
    except (TypeError, ValueError):
        return 0


def import_sources(lat, lon, output, sources, provider='usgs-3dep', surface='bare-ground',
                   vertical_accuracy=None, refresh=False):
    best = min(s['spacing_m'] for s in sources)
    scale, latkey, lonkey, core, bounds = tile_bounds(lat,lon,best)
    target = Path(output)/f'{provider}_{scale}_{latkey}_{lonkey}.pdem'
    cached = read_pack(target)
    if cached and cached['metadata'] and not refresh:
        return target
    dx = best/(111319.49079327358*max(.001,abs(math.cos(math.radians((core[1]+core[3])*.5)))))
    dy = best/111319.49079327358
    nx,ny=max(2,round((bounds[2]-bounds[0])/dx)),max(2,round((bounds[3]-bounds[1])/dy))
    if nx*ny > 8*1024*1024:
        raise ValueError('Survey cell exceeds bounded conversion memory')
    with tempfile.TemporaryDirectory(prefix='terrain-survey-') as directory:
        tmp = Path(directory)
        heights = np.full((ny,nx),np.nan,dtype='f4')
        used = []
        # Coarser/older valid samples fill holes; finest/newest sources overlay.
        ordered = sorted(sources,key=lambda s:(-s['spacing_m'],publication_day(s['product'].get('publicationDate')),
                                               1 if s['product'].get('dataset') == DATASETS[0] else 2 if s['product'].get('dataset') == DATASETS[2] else 0))
        for i, source in enumerate(ordered):
            values = warp_height(source,bounds,nx,ny,tmp/f'height-{i}.bin')
            valid = np.isfinite(values)
            if valid.any():
                heights[valid] = values[valid]
                used.append(source)
        if not np.isfinite(heights).any():
            raise ValueError('No measured source samples cover this cell')
        try:
            cover = crop_cover(bounds,tmp)
        except (subprocess.SubprocessError, OSError, ValueError) as error:
            print(f'WorldCover unavailable: {error}; retaining unknown classification and engine fallback',flush=True)
            wx,wy=max(2,round((bounds[2]-bounds[0])*10800)),max(2,round((bounds[3]-bounds[1])*10800))
            cover = np.zeros((wy,wx),dtype='u1')
        meta = dict(provider=provider,surface=surface,source_datum=used[-1]['datum'],datum='EGM2008',
                    source_spacing_m=max(s['spacing_m'] for s in used),finest_spacing_m=min(s['spacing_m'] for s in used),vertical_accuracy_m=vertical_accuracy,transform_accuracy_m=None,
                    publication_day=max(publication_day(s['product'].get('publicationDate')) for s in used),
                    flags=1 if any(s['datum']!='EGM2008' for s in used) else 0,
                    source_epsg=used[-1]['epsg'],body_id=399,bounds=core,label=used[-1]['product']['title'][:47],
                    retrieved_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    valid_fraction=float(np.isfinite(heights).mean()),
                    target_crs='EPSG:4326+3855',
                    provider_documentation=PROVIDER_DOC if provider=='usgs-3dep' else PGC_DOC if provider in ('arcticdem','rema') else None,
                    attribution=('USGS 3DEP / public domain' if provider=='usgs-3dep' else
                                 'Polar Geospatial Center, University of Minnesota; NSF, NGA; '+provider+' / CC BY 4.0' if provider in ('arcticdem','rema') else
                                 'User-supplied terrain; see source terms')+'; ESA WorldCover 2021 v200 / CC BY 4.0',
                    acknowledgements=polar_acknowledgements(provider),
                    coverage_kind='raster validity; bounding boxes are discovery hints',
                    sources=[dict(product=s['product'],source_crs=s['source_srs'],source_spacing_m=s['spacing_m']) for s in used])
        axes = (bounds[0]+(bounds[2]-bounds[0])/nx*.5,bounds[1]+(bounds[3]-bounds[1])/ny*.5,
                (bounds[2]-bounds[0])/nx,(bounds[3]-bounds[1])/ny)
        write_pack(target,heights,cover,axes,meta)
    print(f'{target}: {nx}x{ny} measured heights, {best:g} m source sampling; {meta["valid_fraction"]:.1%} coverage; EGM2008',flush=True)
    return target


def discover_sources(lat,lon,cache_root=None,refresh=False):
    _,_,_,_,bounds=tile_bounds(lat,lon,1)
    cache=Path(cache_root)/f'.usgs-catalog-{math.floor(lat*100)}_{math.floor(lon*100)}.json' if cache_root else None
    products=cached_catalogue(cache,lambda:usgs_products(bounds),refresh)
    sources=[]
    for product in products:
        try:
            # Standard CONUS 1 m DEMs use NAVD88 even when the TIFF only tags
            # its horizontal CRS. OPRs require their explicit vertical CRS.
            default='NAVD88' if product['dataset']==DATASETS[1] and 24<=lat<=50 and -125<=lon<=-66 else None
            sources.append(raster_source(product,lat,default_datum=default))
        except (subprocess.SubprocessError,OSError,ValueError) as error:
            print(f'Skipping unverified survey {product["title"]}: {error}',flush=True,file=sys.stderr)
    return sources


def pgc_products(provider, bounds, collections):
    # Only public 2 m mosaics; never substitute hillshades, thumbnails or
    # restricted strips. Choose the newest public mosaic release dynamically.
    versions = []
    for collection in collections:
        match = re.fullmatch(provider+r'-mosaics-v(\d+)\.(\d+)-2m', collection['id'])
        if match and collection.get('license') == 'CC-BY-4.0':
            versions.append((tuple(map(int,match.groups())), collection['id']))
    if not versions:
        raise ValueError('No public 2 m PGC mosaic collection available')
    collection = max(versions)[1]
    url = PGC_API+'search?'+urllib.parse.urlencode(dict(collections=collection,bbox=','.join(map(str,bounds)),limit=100))
    products = {}
    visited = set()
    while url:
        if url in visited or len(visited) >= 8:
            raise ValueError('PGC local catalogue pagination exceeds bounds')
        visited.add(url)
        page = request_json(url)
        for feature in page.get('features', []):
            props = feature.get('properties', {})
            asset = feature.get('assets', {}).get('dem', {})
            href = asset.get('href', '')
            parsed = urllib.parse.urlparse(href)
            prefix = '/'+provider+'/mosaics/'+collection.split('-')[2]+'/2m/'
            if (feature.get('collection') != collection or props.get('license') != 'CC-BY-4.0' or
                    props.get('gsd') != 2 or parsed.scheme != 'https' or
                    parsed.netloc != 'pgc-opendata-dems.s3.us-west-2.amazonaws.com' or
                    not parsed.path.startswith(prefix) or not parsed.path.endswith('_dem.tif')):
                continue
            products[href] = dict(title=feature['id'],downloadURL=href,dataset=collection,
                                  publicationDate=props.get('published') or props.get('created'),
                                  license=props['license'],footprint=feature.get('geometry'),
                                  source_start=props.get('start_datetime'),source_end=props.get('end_datetime'),
                                  source_epsg=3413 if provider=='arcticdem' else 3031,
                                  tile_valid_fraction=props.get('pgc:data_perc'),
                                  metadata_url=feature.get('assets',{}).get('metadata',{}).get('href'),
                                  citation='https://doi.org/10.7910/DVN/3VDC4W' if collection=='arcticdem-mosaics-v4.1-2m' else
                                           'https://doi.org/10.7910/DVN/EBW8UC' if collection=='rema-mosaics-v2.0-2m' else None)
        next_link = next((link for link in page.get('links',[]) if link.get('rel')=='next'), None)
        url = urllib.parse.urljoin(url,next_link['href']) if next_link else None
        if url and (not url.startswith(PGC_API) or next_link.get('method','GET') != 'GET'):
            raise ValueError('Unsupported PGC pagination link')
    return list(products.values())


def discover_polar_sources(lat,lon,provider,cache_root=None,refresh=False):
    if provider not in ('arcticdem','rema'):
        raise ValueError('Invalid polar provider')
    root = Path(cache_root) if cache_root else None
    collection_cache = root/'.pgc-collections.json' if root else None
    collections = cached_catalogue(collection_cache,lambda:request_json(PGC_API+'collections')['collections'],refresh)
    _,_,_,_,bounds = tile_bounds(lat,lon,2)
    cache = root/f'.{provider}-catalog-{math.floor(lat*100)}_{math.floor(lon*100)}.json' if root else None
    products = cached_catalogue(cache,lambda:pgc_products(provider,bounds,collections),refresh)
    sources = []
    for product in products:
        try:
            source = raster_source(product,lat,default_datum='ellipsoid')
            if source['epsg'] != product['source_epsg'] or source['datum'] != 'ellipsoid' or abs(source['spacing_m']-2) > .01:
                raise ValueError('PGC asset disagrees with its declared projection/reference/spacing')
            sources.append(source)
        except (subprocess.SubprocessError,OSError,ValueError) as error:
            print(f'Skipping unverified polar mosaic {product["title"]}: {error}',flush=True,file=sys.stderr)
    return sources


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lat',type=float,required=True)
    parser.add_argument('--lon',type=float,required=True)
    parser.add_argument('--output',type=Path,default=Path('data/earth_detail'))
    parser.add_argument('--quality',choices=['auto','glo30','fine'],default='auto')
    parser.add_argument('--upgrade-cover',action='store_true')
    parser.add_argument('--refresh',action='store_true')
    parser.add_argument('--query-only',action='store_true')
    parser.add_argument('--source',action='append',help='local or HTTPS single-band elevation GeoTIFF')
    parser.add_argument('--source-srs',help='explicit compound/3D CRS, required when vertical datum is missing')
    parser.add_argument('--surface-kind',choices=['unknown','bare-ground','surface-model','topobathy'],default='unknown')
    parser.add_argument('--vertical-accuracy',type=float,help='documented vertical accuracy in meters; omitted means unknown')
    args=parser.parse_args()
    if not math.isfinite(args.lat) or not math.isfinite(args.lon) or not -89.9<=args.lat<=89.9 or not -180<=args.lon<180:
        parser.error('Invalid geographic position')
    if args.vertical_accuracy is not None and (not math.isfinite(args.vertical_accuracy) or args.vertical_accuracy<0):
        parser.error('Vertical accuracy must be a documented nonnegative value')
    sources=[]
    provider=automatic_provider(args.lat,args.lon)
    if args.source:
        for name in args.source:
            product=dict(title=Path(urllib.parse.urlparse(name).path).name,downloadURL=name,publicationDate=None)
            sources.append(raster_source(product,args.lat,source_srs=args.source_srs))
    elif args.quality!='glo30' and (provider or args.quality=='fine' or args.query_only):
        try:
            sources=(discover_polar_sources(args.lat,args.lon,provider,args.output,args.refresh) if provider in ('arcticdem','rema') else
                     discover_sources(args.lat,args.lon,args.output,args.refresh))
        except (subprocess.SubprocessError,OSError,ValueError) as error:
            if args.quality=='fine' or args.query_only: raise
            print(f'Fine terrain catalogue unavailable: {error}; using GLO-30 fallback',flush=True)
    if args.query_only:
        print(json.dumps([dict(title=s['product']['title'],url=s['product']['downloadURL'],source_spacing_m=s['spacing_m'],
                               datum=s['datum'],source_epsg=s['epsg']) for s in sources],indent=2))
        return
    if sources:
        try:
            import_sources(args.lat,args.lon,args.output,sources,'local' if args.source else provider or 'usgs-3dep',
                           args.surface_kind if args.source else 'surface-model' if provider in ('arcticdem','rema') else 'bare-ground',
                           args.vertical_accuracy,args.refresh)
            return
        except (subprocess.SubprocessError,OSError,ValueError) as error:
            if args.quality=='fine' or args.source:raise
            print(f'Fine survey conversion unavailable: {error}; using cached/global fallback',flush=True)
    elif args.quality=='fine' or args.source:
        raise ValueError('No verified public fine terrain coverage at this location')
    # Prioritize the near-player fine crop. An unavailable broad polar crop
    # must not delay or prevent valid detailed terrain from being installed.
    coarse=None
    if not args.source and args.quality!='fine' and -85<=args.lat<=85 and -179.9<=args.lon<179.9:
        coarse=fetch_glo(args.lat,args.lon,args.output,args.upgrade_cover)
    print(f'Using measured global/regional fallback: {coarse or "existing engine relief; no fine coverage"}',flush=True)


if __name__=='__main__':
    main()
