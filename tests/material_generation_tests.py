#!/usr/bin/env python3
"""Verify generated files and adding a geological recipe without moving saved IDs."""
import importlib.util
import json
from pathlib import Path
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('import_materials', ROOT/'tools/import_materials.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
original = module.generate()
for relative, text in original.items():
    assert (ROOT/relative).read_text() == text, f'Stale generated source: {relative}'

def ids(header):
    return {name:int(number) for name,number in re.findall(r'MAT_([A-Z0-9_]+)\s*=\s*(\d+)', header) if name!='COUNT'}

with tempfile.TemporaryDirectory(prefix='voxel-material-generation-') as directory:
    module.ROOT = Path(directory)
    for source in ('data/elements/pubchem.json', 'data/materials/recipes.json'):
        target = module.ROOT/source
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT/source).read_bytes())
    path = module.ROOT/'data/materials/recipes.json'
    recipe = json.loads(path.read_text())
    addition = dict(recipe['materials'][11], id=178, key='TEST_ROCK', name='Test rock',
                    recipe={'mix':{'QUARTZ':.5,'CALCITE':.5}})
    recipe['materials'].append(addition)
    path.write_text(json.dumps(recipe))
    extended = module.generate()
    before, after = ids(original['src/material_ids.h']), ids(extended['src/material_ids.h'])
    assert all(after[key] == number for key,number in before.items())
    assert after['TEST_ROCK'] == 178 and after['ELEMENT_OG'] == 177
    assert '#define MATERIAL_COUNT 179' in extended['shaders/material_ids.glsl']
    assert '[MAT_TEST_ROCK]' in extended['src/materials.inc']
    recipe['materials'][-1]['id'] = 60
    path.write_text(json.dumps(recipe))
    try:
        module.generate()
    except ValueError:
        pass
    else:
        raise AssertionError('Accepted a recipe occupying a saved elemental ID')
print('PASS: reproducible material source, append-only IDs and synchronized shader palette')
