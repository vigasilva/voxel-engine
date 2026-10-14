#!/usr/bin/env python3
"""Vulkan terrain-colour uploads and exact material inspection in an isolated world."""
import argparse
import fcntl
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
from window_smoke import wait_for

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('--screenshots', type=Path)
args = parser.parse_args()
executable = args.executable.resolve()
with open(Path(tempfile.gettempdir())/'voxel-gpu-test.lock', 'a') as lock, tempfile.TemporaryDirectory(prefix='voxel-field-test-') as tmp:
    fcntl.flock(lock, fcntl.LOCK_EX)
    rd, wr = os.pipe()
    xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(wr), '-screen', '0', '800x600x24', '-nolisten', 'tcp'], pass_fds=(wr,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(wr)
    with os.fdopen(rd) as pipe:
        display = pipe.readline().strip()
    env = dict(os.environ, DISPLAY=f':{display}', SDL_VIDEODRIVER='x11')
    app = None
    logpath = Path(tmp)/'renderer.log'
    log = open(logpath, 'w+')
    def xdo(*commands, output=False):
        return subprocess.run(['xdotool', *commands], env=env, check=True, text=True, stderr=subprocess.DEVNULL,
                              stdout=subprocess.PIPE if output else subprocess.DEVNULL).stdout
    def window_for_app():
        if app.poll() is not None:
            raise AssertionError(logpath.read_text())
        try:
            return xdo('search', '--onlyvisible', '--pid', str(app.pid), output=True).split()[-1]
        except (IndexError, subprocess.CalledProcessError):
            return None
    def stats():
        matches = re.findall(r'Render stats: .*?triangles=(\d+).*?fields=(\d+) field_uploads=(\d+)', logpath.read_text())
        return tuple(map(int, matches[-1])) if matches else None
    try:
        app = subprocess.Popen(['stdbuf', '-oL', str(executable), '--validation', '--vsync', '--terrain-offline',
                                '--render-stats', '--lat', '40.025', '--lon', '-104.975', '--altitude', '3000',
                                '--freecam', '--frames', '6000'], cwd=tmp, env=env, stdout=log, stderr=log)
        window = wait_for(window_for_app, 'terrain colour window', timeout=40)
        xdo('windowsize', '--sync', window, '640', '480')
        xdo('windowfocus', '--sync', window)
        xdo('key', 'F7', 'F12')
        uploaded = wait_for(lambda: (s if (s := stats()) and s[0] > 0 and s[1] >= 3 else None),
                            'multiple cached terrain colour fields', timeout=50)
        xdo('key', 'F4')
        time.sleep(1.5)
        # With lighting disabled, level-zero material IDs must retain distinct
        # land-cover colours even though those boundaries share greedy triangles.
        data = subprocess.check_output(['import', '-window', window, '-crop', '180x120+230+160', '-depth', '8', 'ppm:-'], env=env, timeout=10)
        pixels = data.partition(b'\n255\n')[2]
        assert len(pixels) == 180*120*3
        from collections import Counter
        counts = Counter(pixels[i:i+3] for i in range(0, len(pixels), 3))
        dominant = [(rgb, n) for rgb, n in counts.items() if n > 30 and max(rgb)-min(rgb) > 15]
        assert len(dominant) >= 2, f'Material boundaries disappeared: {dominant}'
        if args.screenshots:
            args.screenshots.mkdir(parents=True, exist_ok=True)
            subprocess.run(['import', '-window', window, str(args.screenshots/'terrain-field-materials.png')], env=env, check=True)
        xdo('key', 'F4')
        time.sleep(.5)
        if args.screenshots:
            subprocess.run(['import', '-window', window, str(args.screenshots/'terrain-field-lit.png')], env=env, check=True)
        xdo('key', 'Escape')
        assert app.wait(timeout=15) == 0
        output = logpath.read_text()
        assert 'Vulkan validation:' not in output, output
        print(f'PASS: {uploaded[1]} cached colour fields / {uploaded[0]} triangles; {len(dominant)} distinct ground colours; Vulkan and synchronization validation clean')
    except Exception:
        print(logpath.read_text())
        raise
    finally:
        if app and app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait(timeout=5)
        xvfb.terminate()
        xvfb.wait(timeout=5)
        log.close()
