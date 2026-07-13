#!/usr/bin/env python3
"""fontfs_inject.py — host-side FontFS file injector for disk.img.

Writes files (typically user programs built by ``make userprogs``) directly
into a FontaineOS disk image, following the exact on-disk layout implemented
by src/fontfs.cpp / include/fontfs.h:

    LBA 0            superblock   (magic, version, geometry)
    LBA 1            file table   (16 entries x 32 bytes = one 512B sector)
    LBA 2..257       data area    (16 fixed regions of 16 sectors = 8192B)

The shell's ``write`` command can only take keyboard text, so binaries cannot
be typed in; this tool is how ``.bin`` programs get onto the disk before boot.
The kernel's own ``ls``/``cat``/``run`` then see them exactly as if they had
been created in-OS, and they persist across boots like any FontFS file.

Behaviour mirrors the kernel precisely:
  * If the image has no valid superblock (magic mismatch) it is formatted
    first, exactly like fontfs_format(): fresh superblock + empty table with
    self-describing per-slot start_lba values.
  * Injecting a name that already exists overwrites it in place (same slot),
    like fontfs_write().
  * Limits enforced: 16 files, 8192 bytes/file, names <= 19 chars + NUL.

Usage (from the repo root)::

    make all userprogs
    python3 tools/fontfs_inject.py bin/disk.img user/hello.bin user/count.bin
    python3 tools/fontfs_inject.py --list bin/disk.img       # show directory

Stdlib only; no dependencies.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

# ---- Constants copied from include/fontfs.h (keep in sync!) ---------------
SECTOR = 512
FONTFS_MAGIC = 0xF047F5AA
FONTFS_VERSION = 1
SUPERBLOCK_LBA = 0
FILETABLE_LBA = 1
DATA_START_LBA = 2
MAX_FILES = 16
BLOCKS_PER_FILE = 16
NAME_MAX = 20                      # includes the terminating NUL
MAX_FILE_SIZE = BLOCKS_PER_FILE * SECTOR  # 8192

# struct fontfs_superblock: 6 packed uint32_t (rest of sector is zero pad)
SB_FMT = "<6I"
# struct fontfs_entry: char name[20]; u32 size; u32 start_lba; u8 in_use; u8 pad[3]
ENTRY_FMT = "<20sIIB3x"
assert struct.calcsize(ENTRY_FMT) == 32


def slot_lba(index: int) -> int:
    """Data region base LBA for directory slot 'index' (ff_slot_lba)."""
    return DATA_START_LBA + index * BLOCKS_PER_FILE


def read_sector(img, lba: int) -> bytes:
    img.seek(lba * SECTOR)
    data = img.read(SECTOR)
    return data.ljust(SECTOR, b"\0")


def write_sector(img, lba: int, data: bytes) -> None:
    assert len(data) <= SECTOR
    img.seek(lba * SECTOR)
    img.write(data.ljust(SECTOR, b"\0"))


def is_formatted(img) -> bool:
    magic = struct.unpack_from("<I", read_sector(img, SUPERBLOCK_LBA))[0]
    return magic == FONTFS_MAGIC


def format_fs(img) -> None:
    """Byte-for-byte equivalent of fontfs_format()."""
    sb = struct.pack(SB_FMT, FONTFS_MAGIC, FONTFS_VERSION, MAX_FILES,
                     FILETABLE_LBA, DATA_START_LBA, BLOCKS_PER_FILE)
    write_sector(img, SUPERBLOCK_LBA, sb)
    table = b"".join(
        struct.pack(ENTRY_FMT, b"", 0, slot_lba(i), 0) for i in range(MAX_FILES)
    )
    write_sector(img, FILETABLE_LBA, table)


def load_table(img):
    raw = read_sector(img, FILETABLE_LBA)
    return [list(struct.unpack_from(ENTRY_FMT, raw, i * 32)) for i in range(MAX_FILES)]


def store_table(img, table) -> None:
    raw = b"".join(struct.pack(ENTRY_FMT, *e) for e in table)
    write_sector(img, FILETABLE_LBA, raw)


def entry_name(entry) -> str:
    return entry[0].split(b"\0", 1)[0].decode("ascii", "replace")


def inject(img, path: str) -> None:
    name = os.path.basename(path)
    if not (0 < len(name) < NAME_MAX):
        sys.exit(f"error: name '{name}' must be 1..{NAME_MAX - 1} characters")
    with open(path, "rb") as f:
        data = f.read()
    if len(data) > MAX_FILE_SIZE:
        sys.exit(f"error: {path} is {len(data)} bytes (max {MAX_FILE_SIZE})")

    table = load_table(img)
    # Overwrite-in-place if the name exists (fontfs_write semantics)...
    slot = next((i for i, e in enumerate(table)
                 if e[3] and entry_name(e) == name), None)
    if slot is None:  # ...otherwise take the first free slot.
        slot = next((i for i, e in enumerate(table) if not e[3]), None)
    if slot is None:
        sys.exit("error: directory full (16 files max)")

    # Write the data region: full sectors, zero-padding the tail sector,
    # exactly like fontfs_write's per-sector loop.
    for s in range((len(data) + SECTOR - 1) // SECTOR or 1):
        write_sector(img, slot_lba(slot) + s, data[s * SECTOR:(s + 1) * SECTOR])

    table[slot] = [name.encode("ascii"), len(data), slot_lba(slot), 1]
    store_table(img, table)
    print(f"injected '{name}' ({len(data)} bytes) into slot {slot} "
          f"(LBA {slot_lba(slot)})")


def list_fs(img) -> None:
    if not is_formatted(img):
        print("(no filesystem: superblock magic missing)")
        return
    live = [e for e in load_table(img) if e[3]]
    if not live:
        print("(no files)")
    for e in live:
        print(f"  {entry_name(e):<20} {e[1]:>6} bytes  @LBA {e[2]}")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("image", help="raw disk image (e.g. bin/disk.img)")
    p.add_argument("files", nargs="*", help="host files to inject (basename becomes the FontFS name)")
    p.add_argument("--list", action="store_true", help="print the FontFS directory and exit")
    p.add_argument("--format", action="store_true", help="force a fresh format before injecting")
    args = p.parse_args()

    if not os.path.exists(args.image):
        sys.exit(f"error: {args.image} not found (run 'make all' first)")

    with open(args.image, "r+b") as img:
        if args.list:
            list_fs(img)
            return
        if args.format or not is_formatted(img):
            print("formatting: writing fresh superblock + empty file table")
            format_fs(img)
        for path in args.files:
            inject(img, path)
        list_fs(img)


if __name__ == "__main__":
    main()
