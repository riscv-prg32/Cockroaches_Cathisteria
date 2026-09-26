#!/usr/bin/env python3
"""Run the cartridge in PRG32 QEMU, drive it with scripted input, and save
screenshots (PNG) plus the real firmware audio stream (WAV).

Used to produce assets/screenshot.png and to smoke-test builds:

  scripts/qemu_preview.py --script play --out build/preview

The firmware must already be built with `python3 -m prg32 qemu build` in
PRG32_REPO. QEMU uses the SDL display (the firmware waits for its refresh)
with SDL's offscreen video driver, so no window opens. Espressif's QEMU
has no `screendump`, so frames are read by dumping the firmware framebuffer
(`g_fb`, 320x240 RGB565) from guest memory through QMP `pmemsave`.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PRG32 = Path(os.environ.get("PRG32_REPO", ROOT.parent / "PRG32")).resolve()
CONSOLE_PORT = 4422
AUDIO_PORT = 4421
QMP_PORT = 4423
RATE = 22050


def find_qemu() -> str:
    """Prefer Espressif's QEMU (the upstream build has no esp32c3 machine)."""
    env = os.environ.get("QEMU_BIN")
    if env:
        return env
    found = sorted(Path.home().glob(".espressif/tools/qemu-riscv32/*/qemu/bin/qemu-system-riscv32"))
    return str(found[-1]) if found else "qemu-system-riscv32"

# Keys understood by the PRG32 QEMU UART keyboard mapper.
KEYS = {"left": "a", "right": "d", "up": "w", "down": "s", "a": "j", "b": "k",
        "start": "\r"}

# (seconds, action, argument).  action: key/hold/shot/wait
SCRIPTS = {
    "title": [
        (8.0, "shot", "title"),
        (1.0, "key", "b"),
        (1.5, "shot", "help"),
    ],
    "play": [
        (8.0, "shot", "title"),
        (0.5, "key", "a"),
        (1.5, "shot", "intro"),
        (0.4, "key", "a"),
        (0.4, "key", "a"),
        (0.4, "key", "a"),
        (1.5, "shot", "level_start"),
        (0.4, "key", "a"),
        (1.0, "shot", "front_yard"),
        (0.2, "hold", ("up", 0.8)),
        (0.0, "shot", "walk"),
        (0.2, "hold", ("a", 0.4)),
        (0.0, "shot", "cat_leap"),
        (0.3, "shot", "cat_flight"),
        (0.2, "hold", ("left", 0.4)),
        (0.2, "hold", ("a", 1.0)),
        (0.0, "shot", "attack"),
        (0.2, "hold", ("b", 1.2)),
        (0.0, "shot", "shield"),
        (0.2, "hold", ("right", 0.8)),
        (0.2, "hold", ("a", 1.5)),
        (0.5, "shot", "later"),
    ],
}

def connect(port: int, proc: subprocess.Popen, timeout: float = 20) -> socket.socket:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited early ({proc.returncode})")
        s = socket.socket()
        try:
            s.connect(("127.0.0.1", port))
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return s
        except ConnectionRefusedError:
            s.close()
            time.sleep(0.1)
    raise TimeoutError(f"port {port}")


class Qmp:
    def __init__(self, sock: socket.socket):
        self.f = sock.makefile("rw")
        self.f.readline()
        self.execute("qmp_capabilities")

    def execute(self, name: str, **arguments):
        msg = {"execute": name}
        if arguments:
            msg["arguments"] = arguments
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise RuntimeError(reply["error"])


def framebuffer_address() -> int:
    """Address of the QEMU RGB backend framebuffer in the firmware ELF."""
    elf = PRG32 / "build-qemu/PRG32.elf"
    nm = shutil.which("riscv32-esp-elf-nm")
    if not nm:
        found = sorted(Path.home().glob(".espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-nm"))
        nm = str(found[-1]) if found else "riscv32-esp-elf-nm"
    for line in subprocess.run([nm, str(elf)], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "g_fb":
            return int(parts[0], 16)
    raise SystemExit("g_fb not found in the QEMU firmware ELF")


def rgb565_to_png(raw: bytes, path: Path) -> None:
    img = Image.new("RGB", (320, 240))
    px = img.load()
    for i in range(320 * 240):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px[i % 320, i // 320] = ((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6)
    img.save(path)


def audio_reader(sock: socket.socket, stop: threading.Event, pcm: bytearray):
    """Grant one 20 ms audio credit ('K') every 20 ms of wall time and collect
    whatever PCM arrives; the firmware waits for one credit per chunk."""
    sock.settimeout(0.005)
    next_credit = time.monotonic()
    while not stop.is_set():
        now = time.monotonic()
        if now >= next_credit:
            try:
                sock.sendall(b"K")
            except OSError:
                return
            next_credit += 0.02
            if next_credit < now - 0.2:
                next_credit = now
        try:
            data = sock.recv(8192)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            return
        if not data:
            return
        pcm.extend(data)


def console_reader(sock: socket.socket, stop: threading.Event, log: bytearray):
    sock.settimeout(0.3)
    while not stop.is_set():
        try:
            data = sock.recv(4096)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            return
        if not data:
            return
        log.extend(data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cart", type=Path, default=ROOT / "dist/cockroaches-qemu.prg32")
    ap.add_argument("--script", choices=sorted(SCRIPTS), default="title")
    ap.add_argument("--out", type=Path, default=ROOT / "build/preview")
    ap.add_argument("--tail", type=float, default=0.0, help="extra seconds to record")
    ap.add_argument("--burst", type=int, default=3, help="framebuffer dumps per shot")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    flash_src = PRG32 / "build-qemu/qemu_flash.bin"
    efuse_src = PRG32 / "build-qemu/qemu_efuse.bin"
    for f in (flash_src, efuse_src, args.cart):
        if not f.exists():
            raise SystemExit(f"missing {f}")

    with tempfile.TemporaryDirectory(prefix="cc-qemu-") as tmp_s:
        tmp = Path(tmp_s)
        flash, efuse = tmp / "flash.bin", tmp / "efuse.bin"
        shutil.copy2(flash_src, flash)
        shutil.copy2(efuse_src, efuse)
        subprocess.run([sys.executable, str(ROOT / "tools/prg32_cli_64.py"), "qemu", "upload",
                        str(args.cart.resolve()), "--flash", str(flash)], cwd=PRG32,
                       check=True, stdout=subprocess.DEVNULL,
                       env=dict(os.environ, PRG32_REPO=str(PRG32)))
        cmd = [
            find_qemu(), "-M", "esp32c3", "-m", "4M",
            "-drive", f"file={flash},if=mtd,format=raw",
            "-drive", f"file={efuse},if=none,format=raw,id=efuse",
            "-global", "driver=nvram.esp32c3.efuse,property=drive,value=efuse",
            "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth", "-display", "sdl",
            "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server=on,wait=off",
            "-serial", f"tcp::{CONSOLE_PORT},server=on,wait=off,nodelay=on",
            "-serial", f"tcp::{AUDIO_PORT},server=on,wait=on,nodelay=on",
        ]
        log = open(tmp / "qemu.log", "wb")
        # SDL drives the display refresh the firmware waits for; the offscreen
        # video driver keeps it headless (no window appears).
        env = dict(os.environ, SDL_VIDEODRIVER=os.environ.get("SDL_VIDEODRIVER", "offscreen"))
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
        stop = threading.Event()
        pcm, transcript = bytearray(), bytearray()
        try:
            audio = connect(AUDIO_PORT, proc)
            threading.Thread(target=audio_reader, args=(audio, stop, pcm), daemon=True).start()
            console = connect(CONSOLE_PORT, proc)
            threading.Thread(target=console_reader, args=(console, stop, transcript),
                             daemon=True).start()
            qmp = Qmp(connect(QMP_PORT, proc))
            fb_addr = framebuffer_address()

            def tap(name: str, seconds: float = 0.12):
                end = time.monotonic() + seconds
                while True:
                    console.sendall(KEYS[name].encode())
                    time.sleep(0.03)
                    if time.monotonic() >= end:
                        break

            for delay, action, arg in SCRIPTS[args.script]:
                time.sleep(delay)
                if action == "key":
                    tap(arg)
                elif action == "hold":
                    tap(arg[0], arg[1])
                elif action == "shot":
                    # The back buffer may be mid-draw: pause the VM and take a
                    # small burst so at least one frame is complete.
                    for k in range(args.burst):
                        dump = tmp / f"{arg}{k}.fb"
                        qmp.execute("stop")
                        qmp.execute("pmemsave", val=fb_addr, size=320 * 240 * 2,
                                    filename=str(dump))
                        qmp.execute("cont")
                        suffix = "" if k == 0 else f"_{k}"
                        rgb565_to_png(dump.read_bytes(), args.out / f"{arg}{suffix}.png")
                        time.sleep(0.013)
                    print(f"shot {args.out / (arg + '.png')}")
            time.sleep(args.tail)
        finally:
            stop.set()
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            log.close()
            (args.out / "console.log").write_bytes(transcript)
            shutil.copy2(tmp / "qemu.log", args.out / "qemu.log")
        if pcm:
            n = len(pcm) - (len(pcm) % 2)
            with wave.open(str(args.out / "audio.wav"), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(RATE)
                w.writeframes(bytes(pcm[:n]))
            print(f"audio {args.out / 'audio.wav'} ({n / 2 / RATE:.1f} s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
