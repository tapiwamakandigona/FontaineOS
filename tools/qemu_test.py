#!/usr/bin/env python3
"""qemu_test.py — headless QEMU test harness for FontaineOS.

Boots the kernel with no display, drives the QEMU monitor over a unix
socket to type keystrokes into the FontaineOS shell (the shell parser runs
in the keyboard IRQ handler, so monitor ``sendkey`` reaches it), takes
``screendump`` snapshots at scripted points, converts them to PNG, and
exits 0/1 based on whether every step of the scenario completed.

Usage (from the repo root, after ``make all``)::

    python3 tools/qemu_test.py                       # built-in smoke scenario
    python3 tools/qemu_test.py --scenario my.json    # custom scenario
    python3 tools/qemu_test.py --outdir /tmp/shots   # where PNGs land

A scenario is a JSON list of steps, executed in order. Each step is an
object with any of these keys (all optional):

    {"keys": "help\n",         # text to type into the guest (\n = Enter)
     "wait": 2.0,              # seconds to sleep after typing
     "screendump": "02-help"}  # take a snapshot named <name>.png

The module is import-friendly for later test milestones::

    from qemu_test import QemuHarness
    with QemuHarness(kernel, disk, outdir) as h:
        h.run([{"wait": 6, "screendump": "boot"}, ...])

Requirements: Python 3.8+, Pillow (only for PPM->PNG conversion; e.g.
``pip install pillow`` or ``uv run --with pillow``), and a
``qemu-system-i386`` on PATH (see tools/sandbox_setup.sh for rootless
environments). Everything else is stdlib.
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
import time

# --------------------------------------------------------------------------
# Keystroke translation: ASCII -> QEMU `sendkey` names.
# --------------------------------------------------------------------------

_KEYMAP = {
    " ": "spc", "\n": "ret", "\t": "tab",
    "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
    "\\": "backslash", ",": "comma", ".": "dot", "/": "slash",
}
# Shifted symbols on a US layout: char -> unshifted key name.
_SHIFTMAP = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
    "&": "7", "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal",
    "{": "bracket_left", "}": "bracket_right", ":": "semicolon",
    '"': "apostrophe", "~": "grave_accent", "|": "backslash",
    "<": "comma", ">": "dot", "?": "slash",
}


def char_to_sendkey(ch: str) -> str:
    """Translate one character to a QEMU ``sendkey`` argument."""
    if ch.isascii() and (ch.islower() or ch.isdigit()):
        return ch
    if ch.isascii() and ch.isupper():
        return f"shift-{ch.lower()}"
    if ch in _KEYMAP:
        return _KEYMAP[ch]
    if ch in _SHIFTMAP:
        return f"shift-{_SHIFTMAP[ch]}"
    raise ValueError(f"no sendkey mapping for character {ch!r}")


# --------------------------------------------------------------------------
# QEMU monitor client (unix socket).
# --------------------------------------------------------------------------

class Monitor:
    """Minimal human-monitor-protocol client over a unix socket."""

    PROMPT = b"(qemu)"

    def __init__(self, sock_path: str, connect_timeout: float = 10.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.monotonic() + connect_timeout
        while True:  # QEMU creates the socket shortly after launch; retry.
            try:
                self.sock.connect(sock_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline:
                    raise TimeoutError(f"monitor socket {sock_path} never came up")
                time.sleep(0.2)
        self.sock.settimeout(5.0)
        self._read_until_prompt()  # swallow the greeting banner

    def _read_until_prompt(self) -> str:
        buf = b""
        while self.PROMPT not in buf:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                break  # tolerate a quiet monitor; command was still sent
            if not chunk:
                break
            buf += chunk
        return buf.decode(errors="replace")

    def cmd(self, line: str) -> str:
        """Send one monitor command and return its textual response."""
        self.sock.sendall(line.encode() + b"\n")
        return self._read_until_prompt()

    def sendkeys(self, text: str, inter_key_delay: float = 0.06) -> None:
        """Type ``text`` into the guest, one keystroke at a time.

        The delay gives the guest's IRQ handler time to consume each
        scancode; hammering sendkey with zero gap can drop keys.
        """
        for ch in text:
            self.cmd(f"sendkey {char_to_sendkey(ch)}")
            time.sleep(inter_key_delay)

    def screendump(self, ppm_path: str, retries: int = 3, settle: float = 0.5) -> None:
        """Write a PPM screendump, retrying until the file looks complete."""
        for attempt in range(retries):
            self.cmd(f"screendump {ppm_path}")
            time.sleep(settle)
            if os.path.exists(ppm_path) and os.path.getsize(ppm_path) > 0:
                return
            time.sleep(1.0 * (attempt + 1))
        raise RuntimeError(f"screendump failed after {retries} attempts: {ppm_path}")

    def close(self) -> None:
        self.sock.close()


# --------------------------------------------------------------------------
# Harness: QEMU lifecycle + scenario runner.
# --------------------------------------------------------------------------

class QemuHarness:
    """Boot the kernel headless and run a scripted keystroke/snapshot scenario."""

    def __init__(self, kernel: str, disk: str, outdir: str,
                 qemu: str = "qemu-system-i386", boot_wait: float = 5.0):
        self.kernel = kernel
        self.disk = disk
        self.outdir = outdir
        self.qemu = qemu
        self.boot_wait = boot_wait
        self.proc = None
        self.monitor = None
        self._tmpdir = None

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        for path, what in [(self.kernel, "kernel"), (self.disk, "disk image")]:
            if not os.path.exists(path):
                raise FileNotFoundError(f"{what} not found: {path} (run `make all` first)")
        os.makedirs(self.outdir, exist_ok=True)
        self._tmpdir = tempfile.mkdtemp(prefix="fontaineos-qemu-")
        sock_path = os.path.join(self._tmpdir, "monitor.sock")
        cmd = [
            self.qemu,
            "-kernel", self.kernel,
            "-drive", f"file={self.disk},format=raw,index=0,media=disk",
            "-display", "none",
            "-vga", "std",
            "-monitor", f"unix:{sock_path},server,nowait",
        ]
        print(f"[harness] launching: {' '.join(cmd)}")
        self.proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.monitor = Monitor(sock_path)
        print(f"[harness] monitor connected; waiting {self.boot_wait}s for boot")
        time.sleep(self.boot_wait)
        if self.proc.poll() is not None:
            err = self.proc.stderr.read().decode(errors="replace")
            raise RuntimeError(f"QEMU exited during boot:\n{err}")

    def stop(self) -> None:
        if self.monitor:
            try:
                self.monitor.cmd("quit")
            except OSError:
                pass
            self.monitor.close()
            self.monitor = None
        if self.proc:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None
        if self._tmpdir:
            shutil.rmtree(self._tmpdir, ignore_errors=True)
            self._tmpdir = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    # -- scenario execution --------------------------------------------------

    def snapshot(self, name: str) -> str:
        """Take a screendump and convert it to ``<outdir>/<name>.png``."""
        from PIL import Image  # deferred so `--help` works without Pillow
        ppm = os.path.join(self._tmpdir, f"{name}.ppm")
        png = os.path.join(self.outdir, f"{name}.png")
        self.monitor.screendump(ppm)
        img = Image.open(ppm)
        img.save(png)
        if len(img.convert("L").getcolors(maxcolors=4) or [0, 0]) < 2:
            # A single-colour frame means the VGA console never drew anything.
            raise RuntimeError(f"snapshot {name} is blank — guest likely not booted")
        print(f"[harness] snapshot -> {png}")
        return png

    def run(self, scenario: list) -> list:
        """Execute a scenario (list of step dicts). Returns snapshot paths."""
        snapshots = []
        for i, step in enumerate(scenario, 1):
            keys = step.get("keys")
            wait = step.get("wait", 0)
            dump = step.get("screendump")
            print(f"[harness] step {i}/{len(scenario)}: "
                  f"keys={keys!r} wait={wait} screendump={dump!r}")
            if keys:
                self.monitor.sendkeys(keys)
            if wait:
                time.sleep(wait)
            if dump:
                snapshots.append(self.snapshot(dump))
        return snapshots


# Default smoke test: prove the shell boots and answers a command.
# `help` is a real FontaineOS shell command (see src/keyboard.cpp; others:
# uptime, meminfo, clear, disktest).
SMOKE_SCENARIO = [
    {"wait": 2.0, "screendump": "01-boot"},
    {"keys": "help\n", "wait": 2.0, "screendump": "02-help"},
]


def main(argv=None) -> int:
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--kernel", default=os.path.join(repo_root, "bin", "fontaineos.bin"),
                   help="multiboot kernel binary (default: bin/fontaineos.bin)")
    p.add_argument("--disk", default=os.path.join(repo_root, "bin", "disk.img"),
                   help="raw disk image (default: bin/disk.img)")
    p.add_argument("--qemu", default="qemu-system-i386",
                   help="qemu binary to use (default: qemu-system-i386 from PATH)")
    p.add_argument("--outdir", default=os.path.join(repo_root, "bin", "screens"),
                   help="directory for PNG snapshots (default: bin/screens)")
    p.add_argument("--scenario", default=None,
                   help="JSON scenario file (default: built-in smoke test)")
    p.add_argument("--boot-wait", type=float, default=5.0,
                   help="seconds to wait after launch before step 1 (default: 5)")
    args = p.parse_args(argv)

    scenario = SMOKE_SCENARIO
    if args.scenario:
        with open(args.scenario) as f:
            scenario = json.load(f)

    try:
        with QemuHarness(args.kernel, args.disk, args.outdir,
                         qemu=args.qemu, boot_wait=args.boot_wait) as h:
            snapshots = h.run(scenario)
    except Exception as e:  # any failed step fails the run
        print(f"[harness] FAIL: {e}", file=sys.stderr)
        return 1

    print(f"[harness] PASS: {len(scenario)} steps, {len(snapshots)} snapshots")
    return 0


if __name__ == "__main__":
    sys.exit(main())
