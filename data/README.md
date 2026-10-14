# Earth elevation data

`earth_10min.edem` is a compact sample of **NOAA ETOPO 2022 v1, 15 arc-second global relief**. Retrieved 2026-10-14 through the NOAA Pacific Islands Fisheries Science Center ERDDAP server. It contains genuine measured/modelled global elevation and bathymetry data, decimated for this engine.

- [NOAA ETOPO product documentation](https://www.ncei.noaa.gov/products/etopo-global-relief-model)
- [Source dataset, metadata, and license](https://oceanwatch.pifsc.noaa.gov/erddap/griddap/ETOPO_2022_v1_15s.html)
- [Exact downloaded subset](https://oceanwatch.pifsc.noaa.gov/erddap/griddap/ETOPO_2022_v1_15s.dods?z%5B0:40:43199%5D%5B0:40:86399%5D)

The source dataset license permits free use and redistribution. This repository includes the derived compact sample and a reproducible download/conversion script, `tools/fetch_earth.py`.

## Sampling and accuracy

The request takes every 40th source row/column: **1080 × 2160 samples**, or **10 arc minutes** between samples. Equatorial spacing is about 18.5 km, decreasing east/west toward the poles. This is point decimation, not area averaging; small islands, peaks, narrow valleys, and coastline detail can be missed. The original ETOPO grid is much finer than this subset.

Samples are rounded to signed integer meters. The payload occupies 4,665,600 bytes (4.45 MiB), with a 56-byte header. The loaded sample ranges from −10,629 m to +6,664 m. These are sample extrema, not the deepest and highest points of Earth.

NOAA heights reference the EGM2008 geoid. The current engine uses them approximately as altitude over its reference ellipsoid: **no geoid-to-ellipsoid correction is applied**. It bilinearly interpolates elevation, wraps longitude, and averages the final latitude row smoothly within one degree of each pole. Consequently this is broad Earth relief at physical scale, not survey-grade terrain. Biome colors and subsurface material layers are engine approximations rather than NOAA classifications.

## Native regional detail and synthetic local relief

Three files under `earth_regions/` retain native 15-arcsecond sampling from the same NOAA dataset. Each contains 483 × 483 samples, covering approximately two degrees in each direction with a one-sample border. Equatorial north/south spacing is about 463 m; longitude spacing is smaller at higher latitudes. These files add 1,399,734 payload bytes (1.34 MiB), bringing all height payloads to 6,065,334 bytes (5.79 MiB).

| Region | Approximate coverage | Included height range |
| --- | --- | --- |
| Colorado | 39–41° N, 106–104° W | 1,325 to 4,247 m |
| Sydney | 35–33° S, 150–152° E | −4,947 to 1,316 m |
| Himalayas | 27–29° N, 86–88° E | 189 to 8,586 m |

Regional heights blend into the global sample over 24 regional grid intervals at each boundary. The engine also adds continuous deterministic 3D noise at wavelengths 1,024, 256, 64, 16, 4, 1, 0.25 and 0.0625 m, with amplitudes 12, 4, 1, 0.18, 0.035, 0.012, 0.004 and 0.001 m. This variation fades near sea level and scales with a custom planet radius. **This small-scale terrain is synthetic, not measured terrain or millimeter-accurate Earth features.** Rendering, collision, editor picking, and material generation query the same height function.

The version-2 height recipe was the first regional/synthetic terrain upgrade. Its saves retain all absolute edits when upgrading to the version-3 height/hydrology recipe described below.

## Worldwide rivers and streamed fine detail

`earth_5min.edem` improves the global fallback to **2160 × 4320 samples at 5 arc minutes**, about 9.3 km north/south at the equator. Its 18,662,400-byte payload is four times the original global sample. The original 10-minute file is retained for save identity and fallback. The 15-arcsecond regional maps still provide intermediate coverage.

The fine-detail importer combines [Copernicus GLO-30 Public](https://registry.opendata.aws/copernicus-dem/) elevation with [ESA WorldCover 2021 v200](https://github.com/ESA-WorldCover/esa-worldcover-datasets) permanent-water classification. GLO-30 supplies approximately 30 m source sampling; WorldCover supplies approximately 10 m water pixels (class 80). Source TIFFs are read using HTTP ranges, cropped into 0.1° cells with a roughly 60 m halo, and cached as `.pdem` files. Heights are stored as signed integer centimeters and interpolated; this storage precision is **not centimeter survey accuracy**. These are surface elevations: Copernicus includes buildings, vegetation, and infrastructure, rather than a pure bare-ground model. Geoid-to-ellipsoid correction is still not applied.

Starting cache cells cover the default Colorado location and its western neighbors, part of Sydney, and part of Belém/Guamá. Each file is about 1.64 MiB. Additional cells download in the background while traveling near the surface; only one worker runs. F10 toggles online loading; `--terrain-offline` uses bundled/cached data. Downloads require Python 3, NumPy, and the GDAL command-line tools (`gdalbuildvrt`, `gdal_translate`, `gdalinfo`). The main C/Vulkan engine has no GDAL runtime library dependency. Failed or unavailable downloads retain the global/regional fallback. Public GLO-30 coverage has gaps; WorldCover permanent-water classification can miss small, seasonal, or obscured streams. Neither dataset is a complete map of every waterway.

On Unix, the engine memory-maps fine files read-only. A spatial hash finds the active cells, while the operating system pages their contents on demand. Growing the disk cache does not allocate a dense Earth raster in RAM. New data invalidates cached terrain meshes; voxel overrides remain at their original coordinates. When finer terrain loads under a walking player, the controller lifts an embedded player above the new ground. Free flight preserves its position and aim.

`earth_rivers.rivr` contains **275,126 short segments** derived from [Natural Earth river centerlines at 1:10 million scale](https://www.naturalearthdata.com/downloads/10m-physical-vectors/10m-rivers-lake-centerlines/). Here “10m” in the source filename means **ten million map scale, not ten-meter detail**. This public-domain layer supplies major mapped rivers worldwide before satellite cells are downloaded. Widths are estimates from cartographic rank; they are not measured river banks. Satellite water masks take precedence inside fine-data coverage. Global river ribbons are clipped to planet patches so narrow paths survive coarse terrain LOD.

Water surface height follows sampled source elevations (or a centerline elevation for the coarse river model), with an estimated 2 m channel depth where there is no real bathymetry. Inland water exists above sea level, is non-solid for player collision, can be picked and edited, and has no flowing-water or buoyancy simulation. Measured land receives only the small synthetic surface octaves; larger synthetic relief supplements areas where finer heights are absent. Existing ocean bathymetry remains in the NOAA fallback.

Base height packs and the river model now bind `terrain_config` **version 4**. Version-2 and version-3 saves upgrade automatically, retaining every absolute edit. Finer optional cache cells can be added later without invalidating the save. Their arrival can change surrounding ground elevation; edit positions and material values do not move.

Attribution for the included/derived fine data: **© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium.** WorldCover 2021 v200: Zanaga et al. (2022), [doi:10.5281/zenodo.7254221](https://doi.org/10.5281/zenodo.7254221), CC BY 4.0. Copernicus Digital Elevation Model was accessed on 2026-10-14 from the [AWS Open Data Registry](https://registry.opendata.aws/copernicus-dem/), under its linked Copernicus license. NOAA and Natural Earth source links above provide their data provenance.

## Regeneration

```sh
python3 tools/fetch_earth.py
python3 tools/fetch_earth.py --stride 20 --output data/earth_5min.edem
python3 tools/fetch_rivers.py
python3 tools/fetch_earth_detail.py --lat 40 --lon -105
python3 tools/fetch_earth_regions.py
# Or convert an already downloaded DODS response without network access:
python3 tools/fetch_earth.py --source-file /path/to/earth.dods
```

Global NOAA requests are bounded to 48 MiB and have a 180-second timeout. NOAA conversion uses only the Python standard library. NOAA is not queried at runtime; online fine-detail loading uses the public Copernicus/WorldCover COGs through a separate worker. The application cancels a worker after 180 seconds and waits 120 seconds before retrying a failed request. Cache writes replace files atomically.

SHA-256 of the included file:

```text
b61fbcfd41e9053c8fcd2ff6938fe4e0d13da41c27bb3025152d5679335a047e
```

## EDEM version 1 format

All numeric fields are little-endian. Bytes 0–3 are `EDEM`; bytes 4–7 version `1`; bytes 8–15 two `uint32_t` row/column counts. Bytes 16–47 contain four IEEE-754 doubles: first latitude, first longitude, latitude step, longitude step. Bytes 48–51 contain FNV-1a checksum of header bytes 0–47, 52–55, and payload. Bytes 52–55 are reserved zero. Row-major signed 16-bit elevation samples follow. The loader validates dimensions, global coverage for the main map (bounded regional axes for regional maps), finite axes, exact file length, reserved bytes, and checksum before use.

## Fine cache and river formats

PDEM version 1 starts with a 64-byte little-endian header: magic/version; height width/height and water width/height (`uint32_t`); first height longitude/latitude and longitude/latitude steps (`double`); payload CRC32; reserved zero. South-to-north rows of signed `int32_t` centimeter heights follow, with `INT32_MIN` for missing heights, then a byte water mask at its own resolution. The two grids cover the same pixel-edge extent. The loader checks axes, dimensions, exact lengths, and CRC32 before mapping data.

RIVR version 1 has a 16-byte header (magic/version/count/payload CRC32), followed by five little-endian floats per segment: start longitude/latitude, end longitude/latitude, estimated half-width in meters. A one-degree spatial index limits point queries and patch construction to nearby river segments.

## Engine-generated distant terrain atlas

`earth_surface.ptex` is generated in the build directory by `tools/bake_surface_texture.c`, using the same measured base height function and procedural material classification as the engine. It stores signed height, engine material IDs and inland-water coverage. Subpixel synthetic relief is omitted from this distant representation. It is a derived engine cache, not a photographic or satellite-colour map. The shader filters its height values before deciding sea versus land, preserving coastlines independently of the coarse mesh. Its geographic accuracy remains bounded by the source elevation/water datasets. Fine source coverage can change between launches; rebuilding refreshes this atlas, and local meshes independently stream finer data.

Measured terrain now also receives small synthetic relief at 4 m, 1 m, 25 cm and 6.25 cm wavelengths (maximum summed amplitude 5.2 cm). Larger synthetic octaves are limited to fallback areas, and variation fades near sea level. Rendering normals and shader detail add appearance rather than new measurements; the 1 mm editing address does not imply 1 mm surveyed Earth data.
