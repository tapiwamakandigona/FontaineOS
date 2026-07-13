#!/bin/sh
# tools/sandbox_setup.sh — set up nasm + qemu-system-i386 without root.
#
# Some build environments (CI sandboxes, shared dev boxes) have no sudo and a
# locked apt database, so `apt-get install` fails. This script uses the one
# thing that still works there: `apt-get download` (fetch a .deb, no root
# needed) followed by `dpkg -x` (plain extraction, no root needed) into a
# private prefix. Wrapper scripts in $PREFIX/bin then point the dynamic
# linker and QEMU's firmware search path at the extracted tree.
#
# Usage:
#   sh tools/sandbox_setup.sh [PREFIX]
#
#   PREFIX defaults to $X86DEV_PREFIX, then /work/tools/x86dev.
#   Re-running is safe (idempotent): already-downloaded .debs are kept and
#   extraction simply overwrites the same files.
#
# On success it prints the PATH line to add. On a normal Debian/Ubuntu machine
# where you *can* install packages, you don't need this script at all:
#   apt-get install nasm qemu-system-x86
#
# Requires: apt-get (Debian/Ubuntu, with network access to the mirrors),
# dpkg, sh. Tested on Debian 12 (bookworm) as an unprivileged user.

set -eu

PREFIX="${1:-${X86DEV_PREFIX:-/work/tools/x86dev}}"
ROOT="$PREFIX/root"     # extracted package tree (usr/bin, usr/lib, ...)
DEBS="$PREFIX/debs"     # downloaded .deb cache
BIN="$PREFIX/bin"       # wrapper scripts — put this on your PATH

# Packages needed for `nasm` and a runnable `qemu-system-i386`.
# The library list was derived by iterating `ldd` on the qemu binary and
# resolving each "not found" entry to its owning Debian package.
PACKAGES="
nasm
qemu-system-x86
qemu-system-common
qemu-system-data
seabios
ipxe-qemu
libcapstone4
libfdt1
libpmem1
librdmacm1
libibverbs1
libslirp0
libvdeplug2
libbpf1
liburing2
libfuse3-3
libaio1
libndctl6
libdaxctl1
libnl-3-200
libnl-route-3-200
libkmod2
"

mkdir -p "$ROOT" "$DEBS" "$BIN"

echo "==> Downloading packages into $DEBS (skipping any already present)"
cd "$DEBS"
for pkg in $PACKAGES; do
    # apt-get download names files <pkg>_<version>_<arch>.deb; a glob match
    # means we already have it and can skip the network round trip.
    if ls "${pkg}_"*.deb >/dev/null 2>&1; then
        echo "    [cached] $pkg"
    else
        echo "    [fetch ] $pkg"
        apt-get download "$pkg" >/dev/null
    fi
done

echo "==> Extracting into $ROOT"
for deb in "$DEBS"/*.deb; do
    dpkg -x "$deb" "$ROOT"
done

# Library path for the extracted shared objects.
LIBPATH="$ROOT/usr/lib/x86_64-linux-gnu:$ROOT/lib/x86_64-linux-gnu"

echo "==> Writing wrapper scripts into $BIN"

# nasm is self-contained apart from libc; a thin exec wrapper is enough.
cat > "$BIN/nasm" <<EOF
#!/bin/sh
exec "$ROOT/usr/bin/nasm" "\$@"
EOF
chmod +x "$BIN/nasm"

# Debian ships /usr/bin/qemu-system-i386 as a xen-redirect shell wrapper
# around /usr/libexec/qemu-system-i386, so we exec the real binary directly.
# -L points QEMU's firmware (BIOS/VGA ROM) search at the extracted tree;
# LD_LIBRARY_PATH resolves its shared libraries from the same place.
for q in qemu-system-i386 qemu-system-x86_64; do
    cat > "$BIN/$q" <<EOF
#!/bin/sh
export LD_LIBRARY_PATH="$LIBPATH\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec "$ROOT/usr/libexec/$q" -L "$ROOT/usr/share/qemu" "\$@"
EOF
    chmod +x "$BIN/$q"
done

echo "==> Verifying"
"$BIN/nasm" -v
"$BIN/qemu-system-i386" --version | head -n 1

cat <<EOF

Setup complete. Add the wrappers to your PATH:

    export PATH="$BIN:\$PATH"

Then 'make all' in the repo root will find nasm, and tools/qemu_test.py
will find qemu-system-i386.
EOF
