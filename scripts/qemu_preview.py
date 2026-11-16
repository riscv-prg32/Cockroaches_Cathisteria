#!/usr/bin/env python3
"""Run Cockroaches Cathisteria in PRG32 QEMU with a scripted player.

Saves framebuffer screenshots (PNG), the firmware audio stream (WAV, stereo by
default) and, with --video, an MP4 preview of actual QEMU gameplay and audio:

  scripts/qemu_preview.py --script play --out build/preview
  scripts/qemu_preview.py --script preview --video --seconds 75 --fps 12 \
      --out build/qemu-preview
  scripts/qemu_preview.py --recut --cut 0:21,24:42,62:83 --fps 12 --out build/qemu-preview

--cut keeps the listed spans of the recording and joins them, with a short
audio fade at each join, into preview.mp4; the whole recording stays in
raw.mp4. --recut cuts raw.mp4 again without running QEMU. Every frame and every
sample of the result comes from QEMU.

Requirements: the PRG32 QEMU firmware built in $PRG32_REPO/build-qemu
(`python3 -m prg32 qemu build`), Espressif's qemu-riscv32, Pillow, and ffmpeg
for --video. QEMU uses the SDL display with SDL's offscreen driver (the
firmware waits for display refreshes) so no window opens. Frames are read from
the firmware framebuffer (through the `g_fb` pointer, 320x240 RGB565) with QMP
`pmemsave`.

Stereo: the QEMU firmware is built mono; the boot script first goes to SETUP >
AUDIO SETUP and switches I2S MODE to STEREO (saved in the NVS of the temporary
flash copy), so the panned effects are heard.

Adapted from scripts/qemu_preview.py of FairWind-napoli97 and Azzurra's Pro
Tennis Challenge (PRG32 project family, MIT). The flash image is copied to a temporary directory, so
the PRG32 checkout is never modified.
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
CONSOLE_PORT, AUDIO_PORT, QMP_PORT = 4532, 4531, 4533
RATE = 22050
FRAME_BYTES = [4]                    # bytes per audio frame: 4 stereo, 2 mono (set in main)

# PRG32 QEMU UART keyboard: w/s/a/d directions, h = A, j = B, Enter = START.
# (Before PRG32 82b8925, which added C/D/L/R, it was j = A and k = B; with the
# old map, a held "gas" is now a held brake.) The firmware holds every key for
# 120 ms; a longer press is a stream of keys.
KEYS = {"left": "a", "right": "d", "up": "w", "down": "s", "a": "h", "b": "j", "start": "\r", "select": " "}

# The firmware boots into its setup menu: choose "run cartridge", then cart0.
BOOT = [(7.0, "key", "a"), (1.5, "key", "a"), (2.0, "wait", None)]
# The same, after switching the audio to stereo: SETUP (third entry) > AUDIO SETUP
# (third) > I2S MODE (second row) toggled, back twice, up to RUN CARTRIDGE.
BOOT_STEREO = [
    (9.0, "key", "down"), (0.6, "key", "down"), (0.6, "key", "a"),        # SETUP
    (0.8, "key", "down"), (0.5, "key", "down"), (0.5, "key", "down"), (0.5, "key", "a"),  # AUDIO SETUP
    (0.8, "key", "right"), (0.4, "key", "right"), (0.4, "key", "right"),  # VOLUME 100%
    (0.5, "key", "down"), (0.5, "key", "a"),                              # I2S MODE: STEREO
    (0.6, "shot", "_audio_menu"),
    (0.3, "key", "b"), (0.8, "key", "b"),                                  # back to the main menu
    (0.8, "key", "up"), (0.5, "key", "up"), (0.5, "key", "a"),            # RUN CARTRIDGE
    (1.5, "key", "a"), (2.0, "wait", None),                                # cart0
]

# (delay seconds, action, argument). Actions: key, hold (key, seconds), chord
# (keys, seconds: several held together), shot, rec, wait (seconds, or until the
# video is complete), loop ((n, times): the previous n steps again, `times` more
# times).
SCRIPTS = {
    "probe": [(0.5, "shot", "_probe")],
    # A smoke test of the menus and the controls (no video).
    "play": [
        (2.0, "shot", "title"),
        (0.3, "key", "b"),
        (1.0, "shot", "help"),
        (0.3, "key", "start"),                 # START works on the title
        (1.5, "shot", "intro"),
        (0.4, "key", "a"), (0.6, "key", "a"), (0.6, "key", "a"),
        (1.5, "shot", "level_start"),
        (0.4, "key", "start"),                 # ignored on the level card
        (1.0, "shot", "level_start_still"),
        (0.3, "key", "a"),
        (1.5, "shot", "front_yard"),
        (0.2, "hold", ("up", 0.8)),
        (0.0, "shot", "walk"),
        (0.2, "hold", ("a", 0.4)),
        (0.0, "shot", "cat_leap"),
        (0.2, "hold", ("left", 0.4)),
        (0.2, "hold", ("a", 1.0)),
        (0.0, "shot", "attack"),
        (0.2, "chord", (("b", "left"), 1.0)),  # B + LEFT: strafe behind the shield
        (0.0, "shot", "shield_strafe"),
        (0.2, "hold", ("start", 0.5)),         # no pause in play
        (0.3, "shot", "no_pause"),
        (0.2, "hold", ("right", 0.8)),
        (0.2, "hold", ("a", 1.5)),
        (0.5, "shot", "later"),
    ],
    # The Store preview: title with its music, the story, then the front yard
    # fight: cats sent at the roaches, the shield, a strafe, the boss.
    "preview": [
        (0.3, "rec", None),
        (5.5, "key", "a"),                     # title -> story
        (2.6, "key", "a"),
        (2.6, "key", "a"),
        (2.6, "key", "a"),                     # -> level card
        (2.8, "key", "a"),                     # -> play
        (0.8, "hold", ("up", 0.7)),
        (0.1, "hold", ("a", 1.2)),
        (0.2, "hold", ("left", 0.35)),
        (0.1, "hold", ("a", 1.2)),
        (0.2, "chord", (("b", "right"), 1.2)),
        (0.2, "hold", ("right", 0.5)),
        (0.1, "hold", ("a", 1.2)),
        (0.2, "hold", ("up", 0.6)),
        (0.2, "hold", ("left", 0.5)),
        (0.1, "hold", ("a", 1.0)),
        (0.2, "chord", (("b", "left"), 1.0)),
        (0.2, "loop", (11, 8)),
        (0.0, "wait", 600.0),
    ],
    # The second preview segment, recorded from a build that starts on the
    # portal level (CC_START_LEVEL 11, CC_TEST_GOD 1; see docs/design.md).
    "portal": [
        (5.0, "key", "a"),                     # title -> story
        (1.5, "key", "b"),                     # B skips the story
        (1.5, "rec", None),
        (2.5, "key", "a"),                     # level card -> play
        (0.6, "hold", ("up", 0.8)),
        (0.1, "hold", ("a", 1.0)),
        (0.2, "hold", ("right", 0.4)),
        (0.1, "hold", ("a", 1.0)),
        (0.2, "chord", (("b", "left"), 1.0)),
        (0.2, "hold", ("up", 0.6)),
        (0.2, "hold", ("left", 0.6)),
        (0.1, "hold", ("a", 1.2)),
        (0.2, "loop", (8, 8)),
        (0.0, "wait", 600.0),
    ],
}


def find_qemu() -> str:
    env = os.environ.get("QEMU_BIN")
    if env:
        return env
    found = sorted(Path.home().glob(".espressif/tools/qemu-riscv32/*/qemu/bin/qemu-system-riscv32"))
    return str(found[-1]) if found else "qemu-system-riscv32"


def connect(port: int, proc: subprocess.Popen, timeout: float = 25) -> socket.socket:
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


def is_result_screen(img: Image.Image) -> bool:
    """Not used: the Cockroaches preview is cut by time."""
    return False


def framebuffer_address() -> int:
    elf = PRG32 / "build-qemu/PRG32.elf"
    nm = shutil.which("riscv32-esp-elf-nm")
    if not nm:
        found = sorted(Path.home().glob(".espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-nm"))
        nm = str(found[-1]) if found else "riscv32-esp-elf-nm"
    out = subprocess.run([nm, str(elf)], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "g_fb":
            return int(parts[0], 16)
    raise SystemExit("g_fb not found in the QEMU firmware ELF")


def rgb565_image(raw: bytes) -> Image.Image:
    img = Image.new("RGB", (320, 240))
    px = img.load()
    for i in range(320 * 240):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px[i % 320, i // 320] = ((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6)
    return img


def audio_reader(sock: socket.socket, stop: threading.Event, pcm: bytearray):
    """Grant 20 ms audio credits ('K'), at most one every 20 ms and never more
    than a few ahead of the PCM received, and collect the PCM.

    Credits granted on the wall clock alone pile up in the firmware's UART buffer
    whenever QEMU runs slower than real time (driving, frame grabs); after
    40 to 50 s of that the audio stream stopped. PRG32's own capture tool grants
    one credit per chunk received, as this does, with a small lead."""
    chunk = int(RATE * 0.02) * FRAME_BYTES[0]   # one 20 ms credit of 16-bit PCM
    ahead = 4
    sent = 0
    sock.settimeout(0.005)
    next_credit = time.monotonic()
    while not stop.is_set():
        now = time.monotonic()
        if now >= next_credit and sent - len(pcm) // chunk < ahead:
            try:
                sock.sendall(b"K")
            except OSError:
                return
            sent += 1
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


def encoder_args(channels: int) -> list[str]:
    return ["-c:v", "libx264", "-crf", "24", "-preset", "slow", "-pix_fmt", "yuv420p",
            "-movflags", "+faststart", "-c:a", "aac", "-b:a", "128k", "-ac", str(channels)]


def cut_video(ffmpeg: str, src: Path, dst: Path, spec: str, length: float, fps: int, encode: list[str],
              result: float | None = None) -> None:
    """Keep the spans of `spec` of src and join them, with a short audio fade at
    each join. Spans are START:END in seconds, comma-separated; a time may be END
    (the end of the recording) or RESULT (when the result screen first appeared),
    either with a +/- offset: "0:14,28:56,RESULT-10:RESULT+8"."""
    def t(v: str) -> float:
        for name, base in (("END", length), ("RESULT", result)):
            if v.startswith(name):
                if base is None:
                    raise SystemExit("--cut uses RESULT but no result screen was seen")
                return base + (float(v[len(name):]) if len(v) > len(name) else 0.0)
        return float(v)
    spans = []
    for part in spec.split(","):
        a, b = part.split(":")
        spans.append((t(a), t(b)))
    graph, labels = [], ""
    for k, (a, b) in enumerate(spans):
        if not 0 <= a < b <= length + 0.01:
            raise SystemExit(f"--cut span {a:.1f}:{b:.1f} is outside the {length:.1f} s recording")
        d = b - a
        graph.append(f"[0:v]trim={a:.3f}:{b:.3f},setpts=PTS-STARTPTS[v{k}]")
        graph.append(f"[0:a]atrim={a:.3f}:{b:.3f},asetpts=PTS-STARTPTS,"
                     f"afade=t=in:d=0.15,afade=t=out:st={d - 0.25:.3f}:d=0.25[a{k}]")
        labels += f"[v{k}][a{k}]"
    graph.append(f"{labels}concat=n={len(spans)}:v=1:a=1[v][a]")
    subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(src), "-filter_complex", ";".join(graph),
                    "-map", "[v]", "-map", "[a]", "-r", str(fps), *encode, str(dst)], check=True)
    total = sum(b - a for a, b in spans)
    print(f"video {dst} ({total:.1f} s cut from {', '.join(f'{a:.1f}-{b:.1f}' for a, b in spans)})")


def recut(out: Path, spec: str, fps: int) -> int:
    """--recut: cut raw.mp4 (or preview.mp4) of an earlier recording again, without QEMU."""
    ffmpeg = shutil.which("ffmpeg") or "ffmpeg"
    src = out / "raw.mp4"
    if not src.exists():
        shutil.move(out / "preview.mp4", src)
    probe = subprocess.run([shutil.which("ffprobe") or "ffprobe", "-v", "error", "-show_entries",
                            "format=duration:stream=channels", "-of", "json", str(src)],
                           capture_output=True, text=True, check=True)
    info = json.loads(probe.stdout)
    channels = max(int(st.get("channels", 0)) for st in info["streams"])
    capture = out / "capture.json"
    result = json.loads(capture.read_text())["result_seconds"] if capture.exists() else None
    cut_video(ffmpeg, src, out / "preview.mp4", spec, float(info["format"]["duration"]), fps,
              encoder_args(channels), result)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cart", type=Path, default=ROOT / "dist/cockroaches-qemu.prg32")
    ap.add_argument("--script", choices=sorted(SCRIPTS), default="play")
    ap.add_argument("--out", type=Path, default=ROOT / "build/preview")
    ap.add_argument("--video", action="store_true", help="record frames for an MP4")
    ap.add_argument("--seconds", type=float, default=60.0, help="length of the MP4")
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--mono", action="store_true", help="keep the firmware's mono audio")
    ap.add_argument("--cut", help="spans to keep, START:END in seconds, e.g. 0:14,28:56,RESULT-10:RESULT+8")
    ap.add_argument("--recut", action="store_true", help="only cut OUT/raw.mp4 again with --cut (no QEMU)")
    args = ap.parse_args()
    if args.recut:
        if not args.cut:
            raise SystemExit("--recut needs --cut")
        return recut(args.out, args.cut, args.fps)
    FRAME_BYTES[0] = 2 if args.mono else 4
    args.out.mkdir(parents=True, exist_ok=True)
    flash_src, efuse_src = PRG32 / "build-qemu/qemu_flash.bin", PRG32 / "build-qemu/qemu_efuse.bin"
    for f in (flash_src, efuse_src, args.cart):
        if not f.exists():
            raise SystemExit(f"missing {f}")

    with tempfile.TemporaryDirectory(prefix="cc-qemu-") as tmp_s:
        tmp = Path(tmp_s)
        flash, efuse = tmp / "flash.bin", tmp / "efuse.bin"
        shutil.copy2(flash_src, flash)
        shutil.copy2(efuse_src, efuse)
        subprocess.run([sys.executable, "-m", "prg32", "qemu", "upload",
                        str(args.cart.resolve()), "--flash", str(flash)], cwd=PRG32, check=True,
                       stdout=subprocess.DEVNULL)
        cmd = [find_qemu(), "-M", "esp32c3", "-m", "4M",
               "-drive", f"file={flash},if=mtd,format=raw",
               "-drive", f"file={efuse},if=none,format=raw,id=efuse",
               "-global", "driver=nvram.esp32c3.efuse,property=drive,value=efuse",
               "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
               "-nic", "user,model=open_eth", "-display", "sdl",
               "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server=on,wait=off",
               "-serial", f"tcp::{CONSOLE_PORT},server=on,wait=off,nodelay=on",
               "-serial", f"tcp::{AUDIO_PORT},server=on,wait=on,nodelay=on"]
        log = open(tmp / "qemu.log", "wb")
        env = dict(os.environ, SDL_VIDEODRIVER=os.environ.get("SDL_VIDEODRIVER", "offscreen"))
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
        stop = threading.Event()
        pcm, transcript = bytearray(), bytearray()
        frames: list[Path] = []
        try:
            audio = connect(AUDIO_PORT, proc)
            threading.Thread(target=audio_reader, args=(audio, stop, pcm), daemon=True).start()
            console = connect(CONSOLE_PORT, proc)
            threading.Thread(target=console_reader, args=(console, stop, transcript), daemon=True).start()
            qmp = Qmp(connect(QMP_PORT, proc))
            fb_addr = framebuffer_address()
            qmp_lock = threading.Lock()

            def grab(path: Path) -> Image.Image:
                dump = tmp / "fb.raw"
                with qmp_lock:
                    qmp.execute("stop")
                    # In current firmware g_fb is a pointer to the panel's own
                    # framebuffer, outside the emulated SRAM: follow it.
                    qmp.execute("pmemsave", val=fb_addr, size=4, filename=str(dump))
                    pointer = int.from_bytes(dump.read_bytes(), "little")
                    qmp.execute("pmemsave", val=pointer, size=320 * 240 * 2, filename=str(dump))
                    qmp.execute("cont")
                img = rgb565_image(dump.read_bytes())
                img.save(path)
                return img

            video_stop = threading.Event()
            audio_mark = [0]

            video_done = threading.Event()
            result_seen = threading.Event()
            result_at: list[float | None] = [None]

            # Frames are taken on the audio clock, not the wall clock. PRG32 QEMU
            # clocks its audio by game frames, so when emulation slows (driving, on
            # a busy host) a wall-clock minute holds less than a minute of sound;
            # frame n is grabbed once n/fps seconds of audio have arrived, so the
            # picture stays on the sound however fast QEMU runs.
            def recorder():
                n, want = 0, int(round(args.seconds * args.fps))
                step = FRAME_BYTES[0] * RATE / args.fps   # bytes of audio per video frame
                seen, seen_at = len(pcm), time.monotonic()
                try:
                    while not video_stop.is_set() and n < want:
                        if len(pcm) != seen:
                            seen, seen_at = len(pcm), time.monotonic()
                        elif time.monotonic() - seen_at > 40:
                            print(f"warning: no audio for 40 s; video stops at {n} frames", file=sys.stderr)
                            break
                        if len(pcm) - audio_mark[0] < n * step:
                            time.sleep(0.005)
                            continue
                        p = tmp / f"frame{n:05d}.png"
                        if is_result_screen(grab(p)) and not result_seen.is_set():
                            result_seen.set()
                            result_at[0] = n / args.fps
                            shutil.copy2(p, args.out / "result.png")
                            print(f"result screen at {n / args.fps:.1f} s")
                        frames.append(p)
                        n += 1
                except Exception as e:  # never leave a "repeat" waiting for frames that won't come
                    print(f"warning: recorder stopped at frame {n}: {e!r}", file=sys.stderr)
                finally:
                    video_done.set()

            def tap(name: str, seconds: float = 0.12):
                end = time.monotonic() + seconds
                while True:
                    console.sendall(KEYS[name].encode())
                    time.sleep(0.03)
                    if time.monotonic() >= end:
                        break

            rec = None
            loops: dict[int, int] = {}
            steps = list(BOOT if args.mono else BOOT_STEREO) + list(SCRIPTS[args.script])
            i = 0
            while i < len(steps):
                delay, action, arg = steps[i]
                i += 1
                if action == "loop":
                    n_steps, times = arg
                    left = loops.setdefault(i, times)
                    if left > 0 and not video_done.is_set() and not result_seen.is_set():
                        loops[i] = left - 1
                        i -= n_steps + 1
                    continue
                if video_done.is_set() and action in ("hold", "chord", "wait"):
                    continue
                if result_seen.is_set() and action in ("key", "hold", "chord"):
                    continue                  # hands off: let the result screen play
                time.sleep(delay)
                if args.video and rec is None and action == "rec":
                    audio_mark[0] = len(pcm)
                    rec = threading.Thread(target=recorder, daemon=True)
                    rec.start()
                if action == "key":
                    tap(arg)
                elif action == "hold":
                    tap(arg[0], arg[1])
                elif action == "chord":
                    end = time.monotonic() + arg[1]
                    while time.monotonic() < end:
                        for name in arg[0]:
                            tap(name, 0.0)
                elif action == "wait" and arg:
                    end = time.monotonic() + arg
                    while time.monotonic() < end and not (args.video and video_done.is_set()):
                        time.sleep(0.1)
                elif action == "shot" and arg:
                    path = args.out / f"{arg}.png"
                    grab(path)
                    print(f"shot {path}")
            if rec is not None:
                video_stop.set()
                rec.join(timeout=5)
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
            start = audio_mark[0] if args.video else 0
            fb = FRAME_BYTES[0]
            start -= start % fb           # stay on a frame boundary
            data = bytes(pcm[start:])
            data = data[:len(data) - len(data) % fb]
            with wave.open(str(args.out / "audio.wav"), "wb") as w:
                w.setnchannels(fb // 2)
                w.setsampwidth(2)
                w.setframerate(RATE)
                w.writeframes(data)
            print(f"audio {args.out / 'audio.wav'} ({len(data) / fb / RATE:.1f} s, {fb // 2} channels)")
        if args.video and frames:
            ffmpeg = shutil.which("ffmpeg") or "ffmpeg"
            for k, f in enumerate(frames):        # crop the firmware status bands
                Image.open(f).crop((0, 20, 320, 220)).resize((960, 600), Image.NEAREST).save(tmp / f"v{k:05d}.png")
            # Frames sit on the audio clock, so the requested rate is the true one.
            fps = args.fps
            seconds = len(frames) / fps
            length = min(args.seconds, seconds)
            mp4 = args.out / ("raw.mp4" if args.cut else "preview.mp4")
            encode = encoder_args(FRAME_BYTES[0] // 2)
            subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-framerate", f"{fps:.4f}",
                            "-i", str(tmp / "v%05d.png"), "-i", str(args.out / "audio.wav"),
                            "-t", f"{length:.2f}", *encode, "-shortest", str(mp4)], check=True)
            print(f"video {mp4} ({length:.1f} s, {len(frames)} frames at {fps:.2f} fps)")
            (args.out / "capture.json").write_text(json.dumps(
                {"seconds": length, "fps": fps, "result_seconds": result_at[0]}, indent=2) + "\n")
            if args.cut:
                cut_video(ffmpeg, mp4, args.out / "preview.mp4", args.cut, length, fps, encode, result_at[0])
        if b"loaded cartridge" not in transcript.lower() and b"cartridge" not in transcript.lower():
            print("warning: no cartridge load message in console.log", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
