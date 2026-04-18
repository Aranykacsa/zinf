#!/bin/bash
# install-libblkid-zinf.sh — patch libblkid to recognise the ZINF filesystem
#
# After running this (as root), the standard tools work without extra flags:
#   lsblk -f /dev/sdX     → FSTYPE=zinf
#   blkid /dev/sdX        → TYPE="zinf"
#
# What it does:
#   1. Finds the util-linux version currently installed
#   2. Downloads matching source from kernel.org
#   3. Drops libblkid-zinf.c into the superblocks directory
#   4. Patches the prober list (superblocks.c) to include zinf
#   5. Builds libblkid only (fast — ~30 seconds)
#   6. Installs the new libblkid.so over the system copy
#
# Run as root:
#   sudo bash tools/install-libblkid-zinf.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROBER_SRC="$SCRIPT_DIR/libblkid-zinf.c"

if [[ $EUID -ne 0 ]]; then
    echo "error: run as root (sudo bash $0)" >&2
    exit 1
fi

if [[ ! -f "$PROBER_SRC" ]]; then
    echo "error: $PROBER_SRC not found" >&2
    exit 1
fi

# ── Detect installed version ──────────────────────────────────────────────────
VERSION=$(lsblk --version 2>&1 | grep -oP '\d+\.\d+\.\d+' | head -1)
if [[ -z "$VERSION" ]]; then
    echo "error: could not detect util-linux version" >&2
    exit 1
fi
echo "util-linux version: $VERSION"

MAJOR_MINOR=$(echo "$VERSION" | cut -d. -f1,2)   # e.g. 2.41
TARBALL="util-linux-${VERSION}.tar.xz"
URL="https://www.kernel.org/pub/linux/utils/util-linux/v${MAJOR_MINOR}/${TARBALL}"

WORK_DIR=$(mktemp -d /tmp/zinf-libblkid-XXXXXX)
trap "rm -rf $WORK_DIR" EXIT
echo "working in $WORK_DIR"

# ── Download ──────────────────────────────────────────────────────────────────
echo "downloading $URL ..."
curl -fSL "$URL" -o "$WORK_DIR/$TARBALL"
tar -C "$WORK_DIR" -xf "$WORK_DIR/$TARBALL"
SRC="$WORK_DIR/util-linux-${VERSION}"

# ── Patch ─────────────────────────────────────────────────────────────────────
SBLKS_DIR="$SRC/libblkid/src/superblocks"
echo "adding zinf prober ..."
cp "$PROBER_SRC" "$SBLKS_DIR/zinf.c"

# Add to the superblocks list (superblocks.c)
SBLKS_C="$SBLKS_DIR/superblocks.c"

# extern declaration
if ! grep -q "zinf_idinfo" "$SBLKS_C"; then
    # insert after the last existing 'extern const struct blkid_idinfo' line
    sed -i '/^extern const struct blkid_idinfo.*idinfo;$/{ h; s/.*/extern const struct blkid_idinfo zinf_idinfo;/; H; g }' "$SBLKS_C"
    # add to the idinfos[] array — insert after the first opening brace of the array
    sed -i '/^static const struct blkid_idinfo \*idinfos\[\]/,/^};/{
        /^};/ i\	\&zinf_idinfo,
    }' "$SBLKS_C"
fi

# Add zinf.c to the Makefile.am sources
MAKEFILE_AM="$SBLKS_DIR/Makefile.am"
if [[ -f "$MAKEFILE_AM" ]] && ! grep -q "zinf.c" "$MAKEFILE_AM"; then
    sed -i 's/\(libblkid_la_SOURCES.*\)/\1 zinf.c/' "$MAKEFILE_AM" || true
fi

# Also patch CMakeLists.txt if present
CMAKE="$SRC/libblkid/src/superblocks/CMakeLists.txt"
if [[ -f "$CMAKE" ]] && ! grep -q "zinf.c" "$CMAKE"; then
    sed -i '/^  [a-z].*\.c$/a\  zinf.c' "$CMAKE" || true
fi

# ── Build ─────────────────────────────────────────────────────────────────────
echo "configuring ..."
cd "$SRC"
./configure --disable-all-programs --enable-libblkid \
            --without-python --without-udev \
            CFLAGS="-O2 -w" 2>&1 | tail -3

echo "building libblkid ..."
make -C libblkid -j"$(nproc)" 2>&1 | tail -5

# ── Install ───────────────────────────────────────────────────────────────────
SO_SRC=$(find "$SRC/libblkid" -name "libblkid.so.*" -not -type l | head -1)
if [[ -z "$SO_SRC" ]]; then
    echo "error: could not find built libblkid.so" >&2
    exit 1
fi

SO_DEST=$(ldconfig -p | grep "libblkid.so.1 " | awk '{print $NF}' | head -1)
if [[ -z "$SO_DEST" ]]; then
    SO_DEST=/usr/lib64/libblkid.so.1.1.0
fi

echo "installing $SO_SRC → $SO_DEST"
cp "$SO_SRC" "$SO_DEST"
ldconfig

echo ""
echo "done — test with:"
echo "  blkid /dev/loop0"
echo "  lsblk -f /dev/loop0"
