#!/usr/bin/env python3
"""Exercise exported landscapes and the measured-Earth recipe, without a GPU."""
import csv
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

executable = Path(sys.argv[1]).resolve()
data = Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix='voxel-landscape-') as directory:
    root = Path(directory)

    def generate(name, *options):
        prefix = root / name
        subprocess.run([str(executable), '--output', str(prefix), '--size', '17', *options],
                       check=True, stdout=subprocess.DEVNULL)
        rows = list(csv.DictReader(prefix.with_suffix('.csv').open()))
        meta = json.loads(prefix.with_suffix('.json').read_text())
        assert len(rows) == 289 and meta['size'] == 17
        for row in rows:
            assert all(math.isfinite(float(row[key])) for key in
                       ('x_m', 'y_m', 'z_m', 'ground_height_m', 'temperature_k', 'estimated_rain_mm_year'))
        ply = prefix.with_suffix('.ply').read_text()
        assert 'element vertex 289\n' in ply and 'element face 512\n' in ply
        header, payload = ply.split('end_header\n')
        assert len(payload.splitlines()) == 289 + 512
        ppm = prefix.with_suffix('.ppm').read_bytes()
        assert ppm.startswith(b'P6\n17 17\n255\n') and len(ppm.partition(b'\n255\n')[2]) == 289 * 3
        return prefix, rows, meta

    a, _, _ = generate('a', '--biome', 'alpine', '--seed', '4', '--spacing-mm', '50000')
    b, _, _ = generate('b', '--biome', 'alpine', '--seed', '4', '--spacing-mm', '50000')
    c, _, _ = generate('c', '--biome', 'alpine', '--seed', '5', '--spacing-mm', '50000')
    for extension in ('.csv', '.ply', '.ppm', '.json'):
        assert a.with_suffix(extension).read_bytes() == b.with_suffix(extension).read_bytes()
    assert a.with_suffix('.csv').read_bytes() != c.with_suffix('.csv').read_bytes()
    presets = subprocess.check_output([str(executable), '--help'], text=True).splitlines()[-1].split(', ')
    for preset in presets:
        _, rows, meta = generate(preset, '--biome', preset)
        assert meta['mode'] == 'synthetic' and meta['preset'] == preset
        assert all(row['biome'] == preset for row in rows)
    _, rows, meta = generate('earth', '--earth-data', str(data), '--lat', '40.025', '--lon', '-104.975')
    assert meta['mode'] == 'engine-earth' and meta['preset'] == 'auto'
    assert all(20 < float(row['source_spacing_m']) < 40 for row in rows)
    assert all(row['measured_cover'] == '1' for row in rows)
    assert all(float(row['base_height_m']) > 1000 for row in rows)
    for options in (['--size', '1'], ['--spacing-mm', '0'], ['--biome', 'invalid'],
                    ['--seed', '-1'], ['--roughness', 'nan'], ['--lat', '91'],
                    ['--earth-data', str(data), '--roughness', '2']):
        assert subprocess.run([str(executable), *options], cwd=root, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode != 0
print('PASS: reproducible landscape exports, all biome presets, measured Earth sampling and invalid options')
