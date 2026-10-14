# Survey selection and terrain provenance

The engine can now use overlapping terrain rasters with different resolutions and coverage. The existing global NOAA relief, regional samples, GLO-30 packs and WorldCover classes remain useful fallbacks. Fine data is fetched into small cells near the player, not allocated as a dense Earth raster.

## Public USGS surveys

`tools/fetch_terrain.py` queries the official [National Map catalogue API](https://tnmaccess.nationalmap.gov/api/v1/docs) for original-resolution DEMs (OPR), project-based 1 m DEMs and seamless 1 m products (S1M). It inspects the actual raster spacing, coordinate system and vertical reference before using it. The [USGS product descriptions](https://www.usgs.gov/3d-elevation-program/about-3dep-products-services) distinguish bare-ground DEMs from surface models and document the expanding S1M coverage. Availability and sampling resolution vary; this is not uniform 1 m coverage of the world.

The included `data/earth_detail/usgs-3dep_100_4000_-10500.pdem` covers approximately 40.00–40.01° N, 105.00–104.99° W, plus a 20 m halo. Its 893 × 1153 height grid uses approximately 1 m sampling and takes about 4 MiB on disk. The matching JSON records source URLs, full source coordinate systems, publication dates, retrieval time, coverage and attribution. The included source products come from the Colorado DRCOG 2020 bare-ground survey. The importer can use older/coarser valid samples to fill missing coverage when needed. Public USGS 3DEP data has no use restrictions; the accompanying WorldCover classifications require their CC BY 4.0 attribution.

```sh
# Discover and inspect eligible fine surveys without importing their height cells.
python3 tools/fetch_terrain.py --lat 40.0095 --lon -104.9985 --query-only
# Maintain the broad fallback and add fine data where available.
python3 tools/fetch_terrain.py --lat 40.0095 --lon -104.9985
# Query/import fine surveys explicitly; fail if no verified source is available.
python3 tools/fetch_terrain.py --lat 40.0095 --lon -104.9985 --quality fine
# Recheck a catalogue/cache after a source update.
python3 tools/fetch_terrain.py --lat 40.0095 --lon -104.9985 --refresh
./build/terrain_probe data/earth_10min.edem 40 -105
```

Runtime F10 first uses the broad GLO-30 path away from the ground. Near the ground in the contiguous US, it queries fine USGS data even when a coarse cache already exists. Completed/no-coverage checks are remembered for the current cell; catalogue results are also cached for seven days on disk. Downloads use COG range reads, GDAL's bounded warp/cache memory and one conversion thread. Sub-meter source resolution reduces cell area rather than enlarging a raster without bound. Failed discovery, unavailable transformation grids or missing classification retain the existing fallback.

## Datum conversion and honest quality

NAVD88, EGM2008, ellipsoidal and other explicitly declared height references cannot be interchanged by relabeling their values. Fine imports use an explicit compound/3D source CRS and target **EPSG:4326 + EGM2008 height (3855)**. GDAL/PROJ perform the height conversion with `ONLY_BEST=YES`, `ALLOW_BALLPARK=NO`, and vertical shifting enabled. Required public PROJ grids may be accessed through PROJ networking. Sources lacking a vertical CRS require an explicit override; standardized CONUS USGS 1 m products use their documented NAVD88 reference when the TIFF only tags the horizontal CRS. Unknown OPR references are skipped. [GDAL's vertical transformation documentation](https://gdal.org/en/stable/programs/gdalwarp.html#vertical-transformation) describes the coordinate and band-unit conversion.

The engine still approximates EGM2008 heights as height above its reference ellipsoid. A full global geoid displacement of the rendered/collision surface is unfinished. Converting incoming surveys to a common reference removes one source of inconsistency; it does not make the globe survey-grade.

Sampling spacing and vertical accuracy are separate fields. Unknown survey/transformation accuracy stays unknown. A pack containing mixed source resolutions reports the conservative spacing and the finest contributing spacing, with its detailed provenance retained in JSON. Upsampling a coarse survey therefore cannot make it outrank genuine finer measurements. Publication date is not an acquisition date.

The loader chooses the finest valid height coverage, prefers bare-ground data at comparable resolution, then declared non-estimated accuracy, publication date and a stable filename tie-break. All four interpolation samples must be valid. Missing heights fall back to another raster; missing land-cover classifications independently fall back to another classified source. A coverage-bin index supports both small cells and wider overlapping packs. Core heights remain measured; halos blend into surrounding validated terrain to avoid abrupt source edges.

F1 continues to show resource allocations. The material inspector adds source/provider/reference/resolution information. `terrain_probe` prints the full selected pack identity and explicit unknown accuracy values. Neither Earth texture path imports photographic colour imagery.

## Other sources and remaining integration

The generic importer accepts a georeferenced elevation raster with a declared vertical reference:

```sh
python3 tools/fetch_terrain.py --source /path/to/elevation.tif --source-srs 'EPSG:26913+5703' \
  --surface-kind bare-ground --lat 40.01 --lon -105 --output data/earth_detail
```

[ArcticDEM](https://www.pgc.umn.edu/data/arcticdem/) and [REMA/PGC product documentation](https://www.pgc.umn.edu/guides/stereo-derived-elevation-models/pgc-dem-products-arcticdem-rema-and-earthdem/) describe public 2 m surface models, their polar projections, ellipsoidal heights and source limitations. Those products are researched, but automatic PGC discovery/import is still unfinished. Restricted EarthDEM and restricted Alaska acquisitions must not be treated as openly available. Other national bare-ground surveys, stronger hydrology networks/bathymetry, planetary DEMs/shape models and source-boundary water reconciliation remain active work.

## PDEM v3

V1/v2 readers remain supported. V3 uses a 256-byte little-endian header. The initial axes/dimensions retain their v1/v2 meaning; centimeter `int32_t` heights and the WorldCover byte grid follow at byte 256. `INT32_MIN` represents missing height. Core coverage excludes the halo. The binary header records provider, bare-ground/surface kind, input/output vertical references, nominal and finest source spacing, optional accuracy, publication day, flags, a short source label, provenance JSON CRC, physical body identity and source horizontal EPSG. A header CRC protects the georeferencing and metadata; the payload CRC also covers the header extension. See `tools/terrain_pack.py` and `src/terrain_source.h` for the exact layout.

Only compatible Earth/EGM2008 packs are accepted by the current Earth loader. A corrupt or incompatible replacement keeps the previous validated mapping. Source revisions invalidate background mesh jobs; edited voxel values and their integer addresses remain unchanged. Future planetary import must use each body's own reference and coordinate frame.
