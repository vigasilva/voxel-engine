#!/usr/bin/env python3
"""Optional Linux GPU/input integration check (Xvfb + xdotool, real Vulkan GPU).

Usage: python3 tests/window_smoke.py build/voxel_engine [--screenshot docs/preview.png]
The app runs in a temporary directory; existing worlds and desktop input are untouched.
VK_LAYER_PATH/LD_LIBRARY_PATH can point at a locally unpacked validation layer.
"""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile
import time
import struct
import re
from collections import Counter


def wait_for(predicate, description, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.02)
    raise RuntimeError(f"Timed out waiting for {description}")


def decode_world(data):
    """Decode coarse cubes and optional millimeter regions independently."""
    if data[:4] not in (b"VXW2", b"VXW3", b"VXW4", b"VXW5"):
        raise RuntimeError("Expected compressed V2/V3/V4/V5 save")
    nx, ny, nz = struct.unpack_from("<QQQ", data, 8)
    origin = struct.unpack_from("<qqq", data, 40)
    payload_bytes = struct.unpack_from("<Q", data, 64)[0]
    if len(data) != 80 + payload_bytes or (nx, ny, nz) != (128, 64, 128):
        raise RuntimeError("Unexpected compressed save dimensions/length")
    result = bytearray(nx * ny * nz * 8)
    offset = 80

    def cube(x, y, z, size):
        nonlocal offset
        tag = data[offset]
        offset += 1
        if tag == 0:
            value = data[offset:offset+8]
            offset += 8
            row = value * size
            for dy in range(size):
                for dz in range(size):
                    start = (((y+dy)*nz+z+dz)*nx+x)*8
                    result[start:start+size*8] = row
        elif tag == 1 and size > 1:
            half = size // 2
            for i in range(8):
                cube(x+(i&1)*half, y+((i>>1)&1)*half, z+((i>>2)&1)*half, half)
        else:
            raise RuntimeError("Invalid cube tag/depth")

    for cy in range(ny // 16):
        for cz in range(nz // 16):
            for cx in range(nx // 16):
                cube(cx*16, cy*16, cz*16, 16)
    detail_chunk = None
    regions = []
    pending = bytes(32)
    if data[:4] == b"VXW5":
        flags = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        if flags != 1: raise RuntimeError("Unknown procedural flags")
        pending = data[offset:offset+32]
        offset += 64
    def region(lo, hi):
        nonlocal offset
        tag = data[offset]
        offset += 1
        if tag == 0:
            value = data[offset:offset+8]
            offset += 8
            regions.append((tuple(lo), tuple(hi), value))
        elif 1 <= tag <= 3:
            axis = tag-1
            cut = struct.unpack_from("<H", data, offset)[0]
            offset += 2
            if not lo[axis] < cut < hi[axis]:
                raise RuntimeError("Invalid millimeter split")
            low_hi, high_lo = list(hi), list(lo)
            low_hi[axis], high_lo[axis] = cut, cut
            region(lo, low_hi)
            region(high_lo, hi)
        else:
            raise RuntimeError("Invalid millimeter region tag")
    if data[:4] == b"VXW3":
        detail_chunk = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        if detail_chunk == 0xFFFFFFFF:
            detail_chunk = None
        else:
            region([0,0,0], [2000,2000,2000])
    elif data[:4] in (b"VXW4", b"VXW5"):
        count, workshop = struct.unpack_from("<II", data, offset)
        offset += 8
        seen = set()
        for _ in range(count):
            chunk = struct.unpack_from("<I", data, offset)[0]
            offset += 4
            if chunk in seen or chunk >= 256:
                raise RuntimeError("Duplicate/invalid refined chunk")
            seen.add(chunk)
            region([0,0,0], [2000,2000,2000])
        detail_chunk = workshop if workshop != 0xFFFFFFFF else min(seen, default=None)
    if offset != len(data):
        raise RuntimeError("Trailing region records")
    return {"origin": origin, "cells": result, "detail_chunk": detail_chunk, "regions": regions, "pending": pending}


def volume_table(world):
    volumes = Counter()
    for lo, hi, value in world["regions"]:
        material = int.from_bytes(value[:2], "little")
        volumes[material] += (hi[0]-lo[0]) * (hi[1]-lo[1]) * (hi[2]-lo[2])
    return volumes


def main():
    # Serialize GPU checks across invocations; each still has its own X display.
    gpu_lock = open(Path(tempfile.gettempdir()) / "voxel-gpu-test.lock", "a")
    fcntl.flock(gpu_lock, fcntl.LOCK_EX)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--origin-cell", type=int, nargs=3, default=(0, 0, 0))
    parser.add_argument("--millimeter", action="store_true")
    parser.add_argument("--coarse", action="store_true")
    parser.add_argument("--full-detail", action="store_true")
    parser.add_argument("--travel", action="store_true", help="Fly outside the editable map and verify streaming preserves the save")
    parser.add_argument("--view-distance", type=int, default=512)
    args = parser.parse_args()
    if args.travel and args.millimeter: parser.error("--travel uses the terrain camera, without --millimeter")
    executable = args.executable.resolve()
    screenshot = args.screenshot.resolve() if args.screenshot else None
    read_fd, write_fd = os.pipe()
    xvfb = subprocess.Popen(
        ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1600x1000x24", "-nolisten", "tcp"],
        pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    os.close(write_fd)
    app = None
    try:
        with os.fdopen(read_fd) as pipe:
            display = pipe.readline().strip()
        if not display:
            raise RuntimeError("Xvfb failed to allocate a display")
        env = dict(os.environ, DISPLAY=f":{display}", SDL_VIDEODRIVER="x11")

        def xdo(*commands, output=False):
            return subprocess.run(
                ["xdotool", *commands], env=env, check=True,
                stdout=subprocess.PIPE if output else subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, text=True,
            ).stdout

        with tempfile.TemporaryDirectory(prefix="voxel-window-test-") as tmp:
            with open(Path(tmp) / "app.log", "w+") as log:
                app = subprocess.Popen(
                    [str(executable), "--lab", "--frames", "6000", "--validation", "--vsync", "--view-distance", str(args.view_distance), "--origin-cell", *map(str, args.origin_cell),
                     *(["--millimeter"] if args.millimeter else []), *(["--coarse"] if args.coarse else []), *(["--full-detail"] if args.full_detail else [])],
                    cwd=tmp, env=env, stdout=log, stderr=log,
                )

                def find_window():
                    if app.poll() is not None:
                        log.seek(0)
                        raise RuntimeError("App exited before opening a window: " + log.read())
                    try:
                        return xdo("search", "--onlyvisible", "--pid", str(app.pid), output=True).split()[-1]
                    except (subprocess.CalledProcessError, IndexError):
                        return None

                window = wait_for(find_window, "application window")
                time.sleep(2)  # Let the bounded streaming queue fill the initial view.
                xdo("windowfocus", "--sync", window)
                save = Path(tmp) / "world.vxw"

                def saved_after(old=None):
                    return save.read_bytes() if save.exists() and save.read_bytes() != old else None

                xdo("key", "F5")
                baseline = wait_for(saved_after, "initial save")
                xdo("mousemove", "--window", window, "640", "400")
                xdo("key", "Tab")
                time.sleep(0.1)
                xdo("key", "6")
                if baseline[:4] == b"VXW5" and not args.millimeter:
                    # Persist the target chunk via a reversible edit, so independent
                    # volume comparison includes its original generated contents.
                    xdo("click", "3")
                    xdo("click", "1")
                    xdo("key", "F5")
                    baseline = wait_for(lambda: saved_after(baseline), "persisted procedural baseline")
                xdo("click", "3")
                xdo("key", "F5")
                placed = wait_for(lambda: saved_after(baseline), "copper placement")
                # Count actual voxels, not just checksum/header differences.
                initial, edited = decode_world(baseline), decode_world(placed)
                if initial["origin"] != tuple(args.origin_cell) or edited["origin"] != initial["origin"]:
                    raise RuntimeError("64-bit world origin was not preserved in the save")
                if args.millimeter or not args.coarse:
                    if initial["detail_chunk"] is None and baseline[:4] != b"VXW5":
                        raise RuntimeError("Fine map was not enabled")
                    expected = volume_table(initial)
                    expected[6] += 1
                    expected[0] -= 1
                    if volume_table(edited) != expected:
                        raise RuntimeError("Right click did not place exactly 1 mm3 of copper")
                    print("Verified copper placement volume: exactly 1 cubic millimeter")
                else:
                    baseline_cells, placed_cells = initial["cells"], edited["cells"]
                    changed = [i for i in range(0, len(baseline_cells), 8) if baseline_cells[i:i+8] != placed_cells[i:i+8]]
                    if len(changed) != 1 or int.from_bytes(placed_cells[changed[0]:changed[0]+2], "little") != 6:
                        raise RuntimeError("Right click did not place exactly one copper voxel")
                if screenshot:
                    screenshot.parent.mkdir(parents=True, exist_ok=True)
                    subprocess.run(["import", "-window", window, str(screenshot)], env=env, check=True)
                for view in ("overlay", "wireframe", "solid"):
                    xdo("key", "F3")
                    time.sleep(0.08)
                    if screenshot and view != "solid":
                        target = screenshot.with_name(f"{screenshot.stem}-{view}{screenshot.suffix}")
                        subprocess.run(["import", "-window", window, str(target)], env=env, check=True)
                xdo("click", "1")
                xdo("key", "F5")
                removed = wait_for(lambda: saved_after(placed), "voxel removal")
                if removed != baseline:
                    raise RuntimeError("Removing the placed voxel did not restore the original world")
                save.write_bytes(placed)
                xdo("key", "F9")
                time.sleep(0.1)
                # Empty the file so the following save can be observed.
                save.write_bytes(b"")
                xdo("key", "F5")
                loaded = wait_for(lambda: saved_after(b""), "save after load")
                if loaded != placed:
                    raise RuntimeError("F9 did not restore the saved edit")
                if args.travel:
                    xdo("keydown", "Shift_L", "w")
                    time.sleep(3)
                    xdo("keyup", "w", "Shift_L")
                    time.sleep(1)
                    save.write_bytes(b"")
                    xdo("key", "F5")
                    traveled = wait_for(lambda: saved_after(b""), "save after leaving map")
                    if traveled != placed:
                        raise RuntimeError("Streaming camera travel changed persisted voxel data")
                if args.millimeter:
                    xdo("key", "bracketright")
                    xdo("key", "bracketleft")
                    xdo("key", "F4")
                    xdo("key", "F4")
                xdo("key", "Escape")
                xdo("key", "Escape")
                result = app.wait(timeout=5)
                log.seek(0)
                output = log.read()
                print(output, end="")
                if result != 0 or "Vulkan validation:" in output:
                    raise RuntimeError(f"App failed or emitted validation warnings (exit {result})")
                for view in ("TRIANGLE OVERLAY", "WIREFRAME", "SOLID"):
                    if f"Geometry: {view}" not in output:
                        raise RuntimeError(f"F3 did not activate {view}")
                if args.millimeter and not all(s in output for s in ("Brush: 5 mm", "Brush: 1 mm", "Workshop focus: OFF", "Workshop focus: ON")):
                    raise RuntimeError("Workshop focus/brush controls failed")
                if args.travel:
                    camera_match = re.search(r"Camera local meters: ([^ ]+) ([^ ]+) ([^\n]+)", output)
                    if not camera_match or float(camera_match.group(3)) >= 0:
                        raise RuntimeError("Camera did not leave the original map")
                    print("Verified camera travel beyond map bounds; persisted edits unchanged")
                print("PASS: window, mouse capture, geometry views, material selection, placement, removal, save/load, shutdown")
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
        gpu_lock.close()


if __name__ == "__main__":
    main()
