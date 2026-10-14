"""Validated PDEM v3 terrain packs; centimeter heights, source metadata, class masks."""
import json
import math
import os
from pathlib import Path
import struct
import tempfile
import zlib
import numpy as np

HEADER_BYTES = 256
PROVIDERS = {'unknown': 0, 'copernicus': 1, 'usgs-3dep': 2, 'arcticdem': 3, 'rema': 4, 'local': 5, 'nasadem': 6}
SURFACES = {'unknown': 0, 'bare-ground': 1, 'surface-model': 2, 'topobathy': 3}
DATUMS = {'unknown': 0, 'EGM2008': 1, 'NAVD88': 2, 'ellipsoid': 3, 'EGM1996': 4}
CLASSES = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100]
MAX_BYTES = 256 * 1024 * 1024


def atomic_write(target, data):
    target = Path(target)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.'+target.name+'.', suffix='.tmp', delete=False) as f:
            temporary = Path(f.name)
            f.write(data)
        os.replace(temporary, target)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def header_crc(data):
    return zlib.crc32(data[228:256], zlib.crc32(data[60:224], zlib.crc32(data[:56])))


def read_pack(path):
    path = Path(path)
    if not path.exists() or path.stat().st_size > MAX_BYTES:
        return None
    data = path.read_bytes()
    if len(data) < 64:
        return None
    magic, version, nx, ny, wx, wy, lon0, lat0, dx, dy, crc, extra = struct.unpack_from('<4s5I4dII', data)
    offset = extra if version == 3 else 64
    if (magic != b'PDEM' or version not in (1, 2, 3) or
            (version == 3 and offset != HEADER_BYTES) or (version < 3 and extra != 0) or
            min(nx, ny, wx, wy) < 2 or max(nx, ny) > 20000 or
            offset+nx*ny*4+wx*wy != len(data) or
            not all(math.isfinite(v) for v in (lon0, lat0, dx, dy)) or
            not (0 < dx <= .25 and 0 < dy <= .25 and lon0 >= -180 and lat0 >= -90 and
                 lon0+(nx-1)*dx <= 180 and lat0+(ny-1)*dy <= 90) or
            zlib.crc32(data[64:]) != crc):
        return None
    cover = np.frombuffer(data, dtype='u1', offset=offset+nx*ny*4).reshape(wy, wx)
    if not np.isin(cover, [0, 1] if version == 1 else CLASSES).all():
        return None
    metadata = None
    if version == 3:
        provider, surface, source_datum, datum = struct.unpack_from('<4I', data, 64)
        spacing, accuracy = struct.unpack_from('<2d', data, 80)
        publication_day, flags = struct.unpack_from('<2I', data, 96)
        label = data[104:152]
        bounds = struct.unpack_from('<4d', data, 184)
        metadata_crc, body, hcrc, epsg, transform_accuracy = struct.unpack_from('<4Id', data, 216)
        if (hcrc != header_crc(data) or provider not in PROVIDERS.values() or surface not in SURFACES.values() or
                source_datum not in DATUMS.values() or datum != DATUMS['EGM2008'] or body != 399 or
                flags & ~3 or not math.isfinite(spacing) or not 0 < spacing <= 100000 or
                any(not (math.isnan(v) or math.isfinite(v) and v >= 0) for v in (accuracy, transform_accuracy)) or
                b'\0' not in label or any(c < 32 or c > 126 for c in label.split(b'\0', 1)[0]) or
                any(data[248:256]) or not 0 < struct.unpack_from("<d", data, 240)[0] <= spacing or not all(math.isfinite(v) for v in bounds) or
                not (lon0-dx*.5-1e-9 <= bounds[0] < bounds[2] <= lon0+(nx-.5)*dx+1e-9 and
                     lat0-dy*.5-1e-9 <= bounds[1] < bounds[3] <= lat0+(ny-.5)*dy+1e-9)):
            return None
        metadata = dict(provider=provider, surface=surface, source_datum=source_datum, datum=datum,
                        source_spacing_m=spacing, finest_spacing_m=struct.unpack_from("<d", data, 240)[0], vertical_accuracy_m=accuracy, transform_accuracy_m=transform_accuracy,
                        publication_day=publication_day, flags=flags, label=label.split(b'\0')[0].decode('ascii'),
                        bounds=bounds, metadata_crc=metadata_crc, body_id=body, source_epsg=epsg)
    heights = np.frombuffer(data, dtype='<i4', offset=offset, count=nx*ny).reshape(ny, nx)
    return dict(version=version, heights_cm=heights, cover=cover, axes=(lon0, lat0, dx, dy), metadata=metadata)


def write_pack(target, heights, cover, axes, metadata):
    """Inputs are south-to-north; NaN elevations remain missing, never fake terrain."""
    heights = np.asarray(heights, dtype=np.float64)
    cover = np.asarray(cover)
    if heights.ndim != 2 or cover.ndim != 2 or min(*heights.shape, *cover.shape) < 2 or not np.isin(cover, CLASSES).all():
        raise ValueError('Invalid terrain or land-cover raster')
    valid = np.isfinite(heights) & (heights >= -12000) & (heights <= 10000)
    if not valid.any():
        raise ValueError('No valid measured elevation coverage')
    quantized = np.full(heights.shape, -2147483648, dtype='<i4')
    quantized[valid] = np.rint(heights[valid]*100).astype('<i4')
    payload = quantized.tobytes() + cover.astype('u1').tobytes()
    if len(payload)+HEADER_BYTES > MAX_BYTES:
        raise ValueError('Terrain cell exceeds bounded cache-pack size')
    ny, nx = heights.shape
    wy, wx = cover.shape
    header = bytearray(HEADER_BYTES)
    struct.pack_into('<4s5I4dII', header, 0, b'PDEM', 3, nx, ny, wx, wy, *axes, 0, HEADER_BYTES)
    struct.pack_into('<4I', header, 64, PROVIDERS[metadata['provider']], SURFACES[metadata['surface']],
                     DATUMS[metadata['source_datum']], DATUMS[metadata['datum']])
    struct.pack_into('<2d2I', header, 80, metadata['source_spacing_m'],
                     metadata.get('vertical_accuracy_m') if metadata.get('vertical_accuracy_m') is not None else float('nan'),
                     metadata.get('publication_day', 0), metadata.get('flags', 0))
    label = metadata['label'].encode('ascii', 'replace')[:47]
    header[104:104+len(label)] = label
    datum = metadata['datum'].encode('ascii')
    header[152:152+len(datum)] = datum
    struct.pack_into('<4d', header, 184, *metadata['bounds'])
    provenance = json.dumps(metadata, indent=2, sort_keys=True, allow_nan=False).encode('utf-8')+b'\n'
    struct.pack_into('<4Id', header, 216, zlib.crc32(provenance), metadata.get('body_id', 399), 0,
                     metadata.get('source_epsg', 0), metadata.get('transform_accuracy_m') if metadata.get('transform_accuracy_m') is not None else float('nan'))
    struct.pack_into('<d', header, 240, metadata.get('finest_spacing_m', metadata['source_spacing_m']))
    struct.pack_into('<I', header, 224, header_crc(header))
    struct.pack_into('<I', header, 56, zlib.crc32(payload, zlib.crc32(header[64:])))
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    # Validate the exact serialized bytes before publishing either artifact.
    with tempfile.TemporaryDirectory(prefix='pdem-validation-') as directory:
        check = Path(directory)/'check.pdem'
        check.write_bytes(header+payload)
        if read_pack(check) is None:
            raise ValueError('Invalid georeferencing/source metadata')
    atomic_write(Path(str(target)+'.json'), provenance)
    atomic_write(target, header+payload)
    return target
