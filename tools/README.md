# FontaineOS tools

Developer tooling that lives outside the kernel build proper.

## sandbox_setup.sh — rootless toolchain install

Installs `nasm` and `qemu-system-i386` on Debian/Ubuntu machines where you
cannot use `sudo` or `apt-get install` (CI sandboxes, shared dev boxes).
It downloads the .debs with `apt-get download`, extracts them with
`dpkg -x` into a private prefix, and writes wrapper scripts that set the
library and firmware paths.

```sh
sh tools/sandbox_setup.sh              # installs into /work/tools/x86dev
sh tools/sandbox_setup.sh ~/x86dev     # or pick your own prefix
export PATH="/work/tools/x86dev/bin:$PATH"
make all
```

Re-running is safe; downloaded packages are cached. On a machine with root,
skip this and just `apt-get install nasm qemu-system-x86`.

## fontfs_inject.py — host-side FontFS file injector

Writes files (typically the user programs built by `make userprogs`) directly
into `bin/disk.img`, following the exact FontFS on-disk layout implemented by
`src/fontfs.cpp` / `include/fontfs.h`. This is how `.bin` programs get onto
the disk: the shell's `write` command can only take keyboard text.

```sh
make all                                                  # builds user/*.bin too
python3 tools/fontfs_inject.py bin/disk.img user/hello.bin user/count.bin
python3 tools/fontfs_inject.py --list bin/disk.img        # show the FS directory
make inject                                               # shorthand for the injection above
```

Behaviour mirrors the kernel precisely: an image with no valid superblock is
formatted first (like `fontfs_format()`), re-injecting an existing name
overwrites it in place (like `fontfs_write()`), and the kernel limits are
enforced (16 files, 8192 bytes/file, names ≤ 19 chars). Stdlib only.

## qemu_test.py — headless boot & shell test harness

Boots `bin/fontaineos.bin` under QEMU with no display, types commands into
the FontaineOS shell via the QEMU monitor (`sendkey`), and captures
`screendump` snapshots as PNGs. Exit code 0 means every scripted step ran
and produced a non-blank frame; 1 means something failed.

```sh
make all
python3 -m pip install pillow          # or: uv run --with pillow tools/qemu_test.py
python3 tools/qemu_test.py             # built-in smoke test: boot + `help`
python3 tools/qemu_test.py --outdir /tmp/shots --scenario my-scenario.json
```

A scenario is a JSON list of steps; each step may type keys, wait, and/or
snapshot:

```json
[
  {"wait": 2.0, "screendump": "01-boot"},
  {"keys": "meminfo\n", "wait": 2.0, "screendump": "02-meminfo"}
]
```

The harness is also importable (`from qemu_test import QemuHarness`) so
future automated tests can drive it programmatically.
