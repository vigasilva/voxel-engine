#!/usr/bin/env python3
"""Isolated Earth GPU/input/persistence check; requires Xvfb and xdotool."""
import argparse
import fcntl
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import tempfile
import time
from window_smoke import wait_for


def copper_volume(file, selected=6):
    """Independently decode values trees; untouched values are always AIR."""
    total = 0
    with sqlite3.connect(file) as db:
        records = db.execute("SELECT data FROM chunks").fetchall()
    for (data,) in records:
        assert data[:4] == b"PVED" and struct.unpack_from("<I", data, 4)[0] == 1
        mask_size = struct.unpack_from("<I", data, 8)[0]
        at = 20 + mask_size

        def visit(lo, hi):
            nonlocal at
            tag = data[at]
            at += 1
            if tag == 0:
                material, flags, temperature = struct.unpack_from("<HHf", data, at)
                at += 8
                return ((hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2])) if material == selected else 0
            assert 1 <= tag <= 3
            axis = tag - 1
            cut = struct.unpack_from("<H", data, at)[0]
            at += 2
            assert lo[axis] < cut < hi[axis]
            low_hi, high_lo = list(hi), list(lo)
            low_hi[axis] = high_lo[axis] = cut
            return visit(lo, low_hi) + visit(high_lo, hi)

        total += visit([0, 0, 0], [2000, 2000, 2000])
        assert at == len(data)
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--screenshots", type=Path)
    parser.add_argument("--state-only", action="store_true", help="focused material-state HUD and saved-matter check; requires tesseract")
    args = parser.parse_args()
    executable = args.executable.resolve()
    screenshots = args.screenshots.resolve() if args.screenshots else None
    with open(Path(tempfile.gettempdir()) / "voxel-gpu-test.lock", "a") as gpu_lock:
        fcntl.flock(gpu_lock, fcntl.LOCK_EX)
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(
            ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1600x1000x24", "-nolisten", "tcp"],
            pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(write_fd)
        app = None
        try:
            with os.fdopen(read_fd) as pipe:
                display = pipe.readline().strip()
            assert display, "Xvfb did not allocate a display"
            env = dict(os.environ, DISPLAY=f":{display}", SDL_VIDEODRIVER="x11")

            def xdo(*commands, output=False):
                return subprocess.run(["xdotool", *commands], env=env, check=True, text=True,
                                      stdout=subprocess.PIPE if output else subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL).stdout

            def snapshot(name):
                if screenshots:
                    screenshots.mkdir(parents=True, exist_ok=True)
                    subprocess.run(["import", "-window", window, str(screenshots / name)], env=env, check=True, timeout=10)

            def planet_pixels():
                # Center crop excludes the HUD. The few crosshair pixels cannot
                # satisfy this threshold; actual rendered Earth must be present.
                data = subprocess.run(["import", "-window", window, "-crop", "120x120+580+340",
                                       "-depth", "8", "ppm:-"], env=env, check=True,
                                      stdout=subprocess.PIPE, timeout=10).stdout
                header, marker, pixels = data.partition(b"\n255\n")
                assert marker and len(pixels) == 120 * 120 * 3, header
                return sum(max(pixels[i:i + 3]) > 50 for i in range(0, len(pixels), 3))

            def sun_pixels():
                data = subprocess.run(["import", "-window", window, "-crop", "40x40+620+380",
                                       "-depth", "8", "ppm:-"], env=env, check=True,
                                      stdout=subprocess.PIPE, timeout=10).stdout
                pixels = data.partition(b"\n255\n")[2]
                assert len(pixels) == 40 * 40 * 3
                # Crosshair is white; the Sun is warm yellow.
                return sum(pixels[i] > 220 and pixels[i + 1] > 180 and pixels[i + 2] < 230
                           for i in range(0, len(pixels), 3))

            with tempfile.TemporaryDirectory(prefix="planet-window-") as tmp:
                save = Path(tmp) / "earth.vxp"
                with open(Path(tmp) / "app.log", "w+") as log:
                    def launch(arguments, **options):
                        offset = os.fstat(log.fileno()).st_size
                        proc = subprocess.Popen(["stdbuf", "-oL", *arguments], **options)
                        def initialized():
                            output = os.pread(log.fileno(), 131072, offset).decode(errors="replace")
                            if proc.poll() is not None and proc.returncode != 0:
                                raise RuntimeError("Planet initialization failed: " + output)
                            return "Planet allocation limits:" in output
                        wait_for(initialized, "Vulkan renderer initialization", timeout=30)
                        return proc
                    app = launch([str(executable), "--frames", "6000", "--validation", "--vsync", "--terrain-offline"],
                                           cwd=tmp, env=env, stdout=log, stderr=log)

                    def find_window():
                        if app.poll() is not None:
                            log.seek(0)
                            raise RuntimeError("Planet exited early: " + log.read())
                        try:
                            return xdo("search", "--onlyvisible", "--pid", str(app.pid), output=True).split()[-1]
                        except (subprocess.CalledProcessError, IndexError):
                            return None

                    window = wait_for(find_window, "planet window")
                    time.sleep(3)
                    xdo("windowfocus", "--sync", window)
                    xdo("mousemove", "--window", window, "640", "400")
                    xdo("key", "Tab")
                    time.sleep(.2)
                    # Freeze the camera to make a 1 mm click repeatable.
                    xdo("key", "F6", "6")
                    time.sleep(.1)
                    assert copper_volume(save) == 0
                    xdo("click", "3")
                    wait_for(lambda: copper_volume(save) == 1, "exact 1 mm3 copper placement")
                    snapshot("earth-ground.png")
                    if args.state_only:
                        def inspector_text():
                            image = subprocess.check_output(["import", "-window", window, "-crop", "314x280+948+18",
                                                             "-resize", "300%", "png:-"], env=env, timeout=10)
                            return subprocess.check_output(["tesseract", "stdin", "stdout", "--psm", "6"],
                                                           input=image, stderr=subprocess.DEVNULL, timeout=10).decode().upper().replace("LIGUID","LIQUID")
                        def has_state(phase,model):
                            text=inspector_text()
                            return text if phase in text and model in text and "ASSUMED" in text else None
                        copper_text=wait_for(lambda:has_state("ESTIMATE SOLID","ELEMENT REFERENCE"),"rendered copper state estimate",timeout=30)
                        xdo("click","1")
                        wait_for(lambda:copper_volume(save)==0,"state-test copper removal")
                        xdo("key","9")
                        xdo("click","3")
                        wait_for(lambda:copper_volume(save,9)==1,"state-test water placement")
                        water_text=wait_for(lambda:has_state("STATE LIQUID","IAPWS WATER"),"rendered IAPWS water state",timeout=30)
                        snapshot("material-state-water.png")
                        xdo("key","F5","Escape","Escape")
                        assert app.wait(timeout=15)==0
                        log.flush()
                        output=Path(tmp,"app.log").read_text()
                        assert "Vulkan validation:" not in output,output
                        assert copper_volume(save,9)==1 and copper_volume(save)==0
                        print(copper_text,water_text)
                        print("PASS: rendered copper estimate/water equilibrium/assumed pressure; 1 mm saved matter intact; Vulkan validation clean")
                        return
                    xdo("key", "F4", "minus", "minus", "minus")
                    time.sleep(.7)
                    # The swapchain converts linear material colors to sRGB.
                    def copper_pixel():
                        pixel = subprocess.run(["import", "-window", window, "-crop", "1x1+1104+410", "-depth", "8", "ppm:-"], env=env, check=True, stdout=subprocess.PIPE, timeout=10).stdout.partition(b"\n255\n")[2]
                        return len(pixel)==3 and pixel[0]>215 and 150<pixel[1]<190 and 100<pixel[2]<145
                    wait_for(copper_pixel,"rendered copper material slice",timeout=30)
                    snapshot("earth-resources.png")
                    xdo("key", "Next", "Next", "Prior", "equal", "F1", "F1")
                    time.sleep(.6)
                    xdo("key", "F4")
                    for name in ("overlay", "wireframe", "solid"):
                        xdo("key", "F3")
                        time.sleep(.1)
                    xdo("click", "1")
                    wait_for(lambda: copper_volume(save) == 0, "exact copper removal")
                    # Search the expanded catalogue, restore capture, place a new
                    # geological material, then verify its independent saved ID.
                    xdo("key", "m")
                    time.sleep(.2)
                    xdo("type", "--clearmodifiers", "--delay", "60", "Basalt")
                    time.sleep(.2)
                    snapshot("material-catalogue.png")
                    xdo("key", "Return")
                    time.sleep(.2)
                    xdo("click", "3")
                    wait_for(lambda: copper_volume(save, 12) == 1, "catalogue-selected 1 mm basalt placement")
                    xdo("click", "1")
                    wait_for(lambda: copper_volume(save, 12) == 0, "basalt removal")
                    xdo("key", "m")
                    time.sleep(.2)
                    xdo("type", "--clearmodifiers", "--delay", "60", "element Og")
                    xdo("key", "Escape")
                    time.sleep(.2)
                    # Cancelling returns to the prior material and camera capture.
                    xdo("click", "3")
                    wait_for(lambda: copper_volume(save, 12) == 1, "catalogue cancel preserves material")
                    xdo("click", "1")
                    wait_for(lambda: copper_volume(save, 12) == 0, "cancel-case cleanup")
                    xdo("key", "9")
                    xdo("click", "3")
                    wait_for(lambda: copper_volume(save, 9) == 1, "non-solid 1 mm water placement")
                    xdo("click", "1")
                    wait_for(lambda: copper_volume(save, 9) == 0, "water removal through liquid picking")
                    xdo("key", "6")
                    xdo("click", "3")
                    wait_for(lambda: copper_volume(save) == 1, "persistent copper replacement")
                    # Default sphere: a 5 mm stroke contains 81 voxel centers,
                    # not the 125 cells of its old axis-aligned bounding cube.
                    xdo("key", "bracketright")
                    xdo("click", "3")
                    wait_for(lambda: copper_volume(save) >= 81, "spherical 5 mm placement")
                    spherical_volume = copper_volume(save)
                    assert spherical_volume < 126, spherical_volume
                    xdo("key", "bracketleft", "b", "b")
                    xdo("key", "F5", "bracketright", "bracketleft", "F8")
                    time.sleep(2)
                    snapshot("earth-orbit.png")
                    xdo("key", "F8")
                    time.sleep(2)
                    assert copper_volume(save) == spherical_volume, "Planetary LOD lost saved edits"
                    xdo("key", "Escape")
                    xdo("key", "Escape")
                    result = app.wait(timeout=10)
                    log.seek(0)
                    output = log.read()
                    print(output, end="")
                    assert "Material visualizer: ON" in output and "Material visualizer: OFF" in output
                    assert "Computer resources: OFF" in output and "Computer resources: ON" in output
                    assert "Material slice: depth 0.005 m" in output
                    assert result == 0 and "Vulkan validation:" not in output
                    for message in ("Geometry: TRIANGLE OVERLAY", "Geometry: WIREFRAME", "Geometry: SOLID",
                                    "Planet view: SPACE", "Planet view: GROUND", "Planet brush: 5 mm",
                                    "PLANET EDITS FLUSHED"):
                        assert message in output, message
                    # Reopen the actual application to validate saved data, not just SQLite visibility.
                    app = launch([str(executable), "--frames", "30", "--validation", "--vsync", "--terrain-offline"],
                                           cwd=tmp, env=env, stdout=log, stderr=log)
                    assert app.wait(timeout=20) == 0
                    assert copper_volume(save) == spherical_volume
                    log.seek(0)
                    assert "Vulkan validation:" not in log.read()
                    app = launch([str(executable), "--orbit", "--frames", "30", "--validation", "--vsync", "--terrain-offline"],
                                           cwd=tmp, env=env, stdout=log, stderr=log)
                    assert app.wait(timeout=20) == 0
                    log.seek(0)
                    assert "Vulkan validation:" not in log.read()
                    # Real satellite-mapped inland water at 1,565 m elevation.
                    app = launch([str(executable), "--lat", "40.010509259259294", "--lon", "-104.9581944444449",
                                            "--altitude", "250", "--freecam", "--frames", "6000",
                                            "--validation", "--vsync", "--terrain-offline"],
                                           cwd=tmp, env=env, stdout=log, stderr=log)
                    window = wait_for(find_window, "inland water window")
                    xdo("windowfocus", "--sync", window)
                    xdo("key", "F7", "v", "v", "v")
                    time.sleep(3)
                    pixels = subprocess.run(["import", "-window", window, "-crop", "120x120+580+340", "-depth", "8", "ppm:-"],
                                            env=env, check=True, stdout=subprocess.PIPE, timeout=10).stdout.partition(b"\n255\n")[2]
                    blue=sum(pixels[i+2]>80 and pixels[i+2]>pixels[i+1]*1.2 and pixels[i+1]>pixels[i]*1.2
                             for i in range(0,len(pixels),3))
                    assert blue>200, ("Inland water was not rendered", blue)
                    snapshot("earth-inland-water.png")
                    xdo("key", "Escape")
                    assert app.wait(timeout=15)==0
                    app = launch([str(executable), "--altitude", "43000000", "--freecam",
                                            "--view-distance", "100", "--frames", "6000", "--validation", "--vsync", "--terrain-offline"],
                                           cwd=tmp, env=env, stdout=log, stderr=log)
                    window = wait_for(find_window, "43 million m altitude window")
                    time.sleep(3)
                    assert planet_pixels() > 1000, "Earth invisible at 43 million m"
                    snapshot("earth-distant.png")
                    xdo("windowfocus", "--sync", window)
                    xdo("mousemove", "--window", window, "640", "400")
                    xdo("key", "Tab")
                    time.sleep(.1)
                    xdo("mousemove_relative", "--", "0", "-700")
                    time.sleep(.5)
                    assert planet_pixels() < 1000, "Look-away input did not turn away from Earth"
                    xdo("key", "F7")
                    time.sleep(2)
                    assert planet_pixels() > 1000, "F7 did not restore Earth at the current position"
                    xdo("key", "F2")
                    time.sleep(.5)
                    assert sun_pixels() > 8, "Visible Sun missing or misaligned with lighting"
                    snapshot("earth-sun.png")
                    # A distant Sun is a fixed direction in world space. It must
                    # stay centered after a multi-thousand-kilometer sideways flight.
                    xdo("keydown", "Shift_L", "d")
                    time.sleep(1.2)
                    xdo("keyup", "d", "Shift_L")
                    time.sleep(.2)
                    assert sun_pixels() > 8, "Flight camera rotated with planetary up during translation"
                    xdo("key", "F6", "F6")
                    time.sleep(.3)
                    assert sun_pixels() > 8, "Flight/ground transition changed the camera aim"
                    xdo("key", "Escape")
                    xdo("key", "Escape")
                    assert app.wait(timeout=10) == 0
                    log.seek(0)
                    output = log.read()
                    assert "Vulkan validation:" not in output and "Planet camera: AIM AT EARTH" in output
                    assert "Rendering: DISTANT ENGINE TEXTURE / ZERO TERRAIN MESHES" in output
                    print("PASS: material catalogue selection/cancel and exact basalt edits, height terrain, exact 1 mm copper/water edits, spherical brushes, greedy geometry, autosave/reopen, orbit, 43 million m visibility, F7 aim, visible Sun/F2 aim, F1 resources/F4 material slice, flight orientation, inland water")
        finally:
            if app and app.poll() is None:
                app.terminate()
                try:
                    app.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    app.kill()
                    app.wait(timeout=5)
            xvfb.terminate()
            try:
                xvfb.wait(timeout=5)
            except subprocess.TimeoutExpired:
                xvfb.kill()
                xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
