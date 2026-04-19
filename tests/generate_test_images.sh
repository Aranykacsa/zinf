#!/usr/bin/env bash
# generate_test_images.sh — Create 3 synthetic ZINF test images for user testing.
# Each image contains 500 sensor readings with varied temperature/humidity ranges.
#
# Usage:
#   bash tests/generate_test_images.sh [output_dir]
#
# Output:
#   zinf_test_1.img  (temp 20–24°C, humidity 55–65%)
#   zinf_test_2.img  (temp 24–26°C, humidity 65–70%)
#   zinf_test_3.img  (temp 26–28°C, humidity 70–75%)
#
# Requirements: zinf CLI must be installed (sudo make install)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
OUT_DIR="${1:-/tmp}"
ZINF="${ZINF:-zinf}"
IMG_SIZE_MB=8
SECTOR_COUNT=500

check_deps() {
    if ! command -v "$ZINF" &>/dev/null; then
        echo "Error: 'zinf' CLI not found. Run: sudo make install" >&2
        exit 1
    fi
    if ! command -v losetup &>/dev/null; then
        echo "Error: 'losetup' not found (install util-linux)" >&2
        exit 1
    fi
}

write_image() {
    local img="$1"
    local temp_min="$2"
    local temp_max="$3"
    local hum_min="$4"
    local hum_max="$5"

    echo "Creating $img (temp ${temp_min}–${temp_max}°C, humidity ${hum_min}–${hum_max}%)..."

    # Create blank image
    dd if=/dev/zero of="$img" bs=1M count="$IMG_SIZE_MB" status=none

    # Format as ZINF
    "$ZINF" format "$img"

    # Attach as loop device
    local loop
    loop="$(sudo losetup -f --show "$img")"
    echo "  Attached as $loop"

    # Write SECTOR_COUNT sensor readings via shell pipe
    # Each 'raid sensor' writes <count> records with the given values
    # We write in 50-record batches with slightly varied values across the range
    local step_count=10
    local step_size=$(( SECTOR_COUNT / step_count ))

    for i in $(seq 0 $((step_count - 1))); do
        # Linearly interpolate temp and humidity across the range
        local temp
        local hum
        temp=$(awk "BEGIN { printf \"%.2f\", $temp_min + ($temp_max - $temp_min) * $i / ($step_count - 1) }")
        hum=$(awk  "BEGIN { printf \"%.2f\", $hum_min  + ($hum_max  - $hum_min)  * $i / ($step_count - 1) }")

        # Use zinf shell to write records
        echo "raid sensor $step_size $temp $hum" | sudo "$ZINF" shell "$loop" >/dev/null
    done

    # Show sector count for verification
    local info
    info=$(sudo "$ZINF" info "$loop" 2>/dev/null | grep -i "last\|sector" | head -2 || true)
    echo "  $info"

    # Detach loop device
    sudo losetup -d "$loop"
    echo "  Done: $img"
}

check_deps

mkdir -p "$OUT_DIR"

write_image "$OUT_DIR/zinf_test_1.img"  20.0 24.0  55.0 65.0
write_image "$OUT_DIR/zinf_test_2.img"  24.0 26.0  65.0 70.0
write_image "$OUT_DIR/zinf_test_3.img"  26.0 28.0  70.0 75.0

echo ""
echo "Generated 3 test images in $OUT_DIR:"
ls -lh "$OUT_DIR"/zinf_test_*.img
echo ""
echo "Attach for use:"
echo "  sudo losetup -f --show $OUT_DIR/zinf_test_1.img"
echo "  zinf-studio  # → device appears in sidebar"
