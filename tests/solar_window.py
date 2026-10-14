#!/usr/bin/env python3
"""Isolated Vulkan checks for orbit navigation, engine terrain textures and real occlusion."""
import argparse
import fcntl
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
from window_smoke import wait_for


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--screenshots', type=Path)
    parser.add_argument('--cache-only', action='store_true')
    args = parser.parse_args()
    executable = args.executable.resolve()
    root = Path(__file__).resolve().parents[1]
    with open(Path(tempfile.gettempdir()) / 'voxel-gpu-test.lock', 'a') as lock, tempfile.TemporaryDirectory(prefix='voxel-solar-test-') as tmp:
        fcntl.flock(lock, fcntl.LOCK_EX)
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1600x1000x24', '-nolisten', 'tcp'], pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(write_fd)
        with os.fdopen(read_fd) as pipe:
            display = pipe.readline().strip()
        env = dict(os.environ, DISPLAY=f':{display}', SDL_VIDEODRIVER='x11')
        app = None
        logpath = Path(tmp)/'renderer.log'
        log = open(logpath, 'w+')

        def xdo(*commands, output=False):
            return subprocess.run(['xdotool', *commands], env=env, check=True, text=True, stdout=subprocess.PIPE if output else subprocess.DEVNULL, stderr=subprocess.DEVNULL).stdout

        def window_for_app():
            if app.poll() is not None:
                log.flush()
                raise AssertionError(logpath.read_text())
            try:
                return xdo('search', '--onlyvisible', '--pid', str(app.pid), output=True).split()[-1]
            except (subprocess.CalledProcessError, IndexError):
                return None

        def pixels(window):
            data = subprocess.run(['import', '-window', window, '-crop', '100x100+590+350', '-depth', '8', 'ppm:-'], env=env, check=True, stdout=subprocess.PIPE, timeout=10).stdout
            return data.partition(b'\n255\n')[2]

        def snapshot(window, name):
            if args.screenshots:
                args.screenshots.mkdir(parents=True, exist_ok=True)
                subprocess.run(['import', '-window', window, str(args.screenshots/name)], env=env, check=True, timeout=10)

        def start(*options):
            nonlocal app
            start_offset=logpath.stat().st_size
            app = subprocess.Popen(['stdbuf', '-oL', str(executable), '--validation', '--vsync', '--terrain-offline', '--frames', '6000', *options], cwd=tmp, env=env, stdout=log, stderr=log)
            wait_for(lambda: 'Planet allocation limits:' in logpath.read_text()[start_offset:] or (app.poll() is not None and window_for_app()), 'renderer initialization')
            window = wait_for(window_for_app, 'rendering window')
            xdo('windowfocus', '--sync', window)
            return window

        def stop():
            xdo('key', 'Escape')
            assert app.wait(timeout=15) == 0
            log.flush()
            output = logpath.read_text()
            assert 'Vulkan validation:' not in output, output
            return output

        def cache_check():
            window = start('--altitude', '1000000', '--freecam', '--render-stats')
            xdo('key', 'F7', 'F12')
            def stats():
                matches = re.findall(r'Render stats: frame=(\d+) mode=(\w+) visible=(\d+) pending=(\d+) triangles=(\d+) cache=(\d+) cache_mib=([0-9.]+)', logpath.read_text())
                return [dict(frame=int(m[0]), mode=m[1], visible=int(m[2]), pending=int(m[3]), triangles=int(m[4]), cache=int(m[5]), cache_mib=float(m[6])) for m in matches]
            def loaded():
                values = stats()
                return values[-1] if values and values[-1]['visible'] > 0 and values[-1]['pending'] == 0 else None
            initial = wait_for(loaded, 'complete overview terrain', timeout=25)
            xdo('key', 'F2')
            def away():
                values = stats()
                return values[-1] if values and values[-1]['frame'] >= initial['frame'] + 300 and values[-1]['cache'] > 20 else None
            retained = wait_for(away, '300 away-facing frames with retained GPU meshes', timeout=25)
            offset = logpath.stat().st_size
            xdo('key', 'F7')
            def returned():
                matches = re.findall(r'Render stats: frame=(\d+) mode=(\w+) visible=(\d+) pending=(\d+)', logpath.read_text()[offset:])
                return matches[0] if matches else None
            first = wait_for(returned, 'first returning-view statistics')
            assert int(first[2]) == initial['visible'] and int(first[3]) == 0, first
            assert pixels(window) and max(pixels(window)) > 50, 'Cached Earth did not render'
            stop()
            print(f"PASS: {retained['cache']} GPU meshes retained ({retained['cache_mib']:.2f} MiB); return to {initial['visible']} patches required zero mesh jobs")

        try:
            if args.cache_only:
                cache_check()
                return
            altitude = subprocess.check_output([str(executable.parent/'occlusion_seed'), str(root/'data/earth_10min.edem'), str(Path(tmp)/'earth.vxp')], text=True, timeout=20).strip()
            window = start('--lat', '0', '--lon', '0', '--altitude', altitude, '--freecam')
            time.sleep(.8)
            xdo('key', 'F7', 'F12')
            time.sleep(4)
            enabled = pixels(window)
            print('Occlusion foreground bytes/range', len(enabled), (min(enabled), max(enabled)) if enabled else None)
            snapshot(window, 'earth-occlusion.png')
            xdo('key', 'o')
            time.sleep(1)
            disabled = pixels(window)
            assert enabled == disabled, 'Occlusion altered the visible foreground surface'
            assert len(enabled) == 30000 and max(enabled) > 45, 'No foreground occluder rendered'
            xdo('key', 'o')
            time.sleep(1)
            snapshot(window, 'earth-occlusion.png')
            output = stop()
            maximum = int(re.search(r'Occlusion: \d+ hidden, maximum (\d+) patches', output).group(1))
            assert maximum >= 4, output
            print(f'PASS: {maximum} hidden patches skipped; foreground pixels identical with culling disabled')

            window = start('--solar-view', '--solar-body', '4', '--time-scale', '86400')
            time.sleep(1)
            mars = pixels(window)
            assert sum(mars[0::3]) > sum(mars[2::3])*1.3, 'Mars did not render as a lit sphere'
            snapshot(window, 'solar-mars.png')
            xdo('key', 'Right')
            time.sleep(.7)
            jupiter = pixels(window)
            assert mars != jupiter, 'Planet selection did not change the rendered body'
            snapshot(window, 'solar-jupiter.png')
            xdo('key', 'Right')
            time.sleep(.7)
            snapshot(window, 'solar-saturn.png')
            xdo('key', 'F9', 'F12', 'F11')
            time.sleep(.7)
            xdo('key', 'F11')
            time.sleep(.5)
            output = stop()
            assert 'Solar focus: JUPITER' in output and 'Solar focus: SATURN' in output
            assert 'Solar view: OFF' in output and 'Solar view: ON' in output
            days = float(re.findall(r'Solar: J2000 \+ ([0-9.]+) days', output)[-1])
            assert days > .5, days
            assert 'Engine terrain atlas:' in output, output
            print(f'PASS: physical planetary spheres, time warp ({days:.2f} simulated days), pause and Earth return')
        except Exception:
            log.flush()
            print(logpath.read_text())
            raise
        finally:
            if app and app.poll() is None:
                app.terminate()
                try:
                    app.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    app.kill()
                    app.wait()
            log.close()
            xvfb.terminate()
            xvfb.wait(timeout=8)


if __name__ == '__main__':
    main()
