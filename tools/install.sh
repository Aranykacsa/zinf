#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
PREFIX="${PREFIX:-/usr/local}"

echo "=== ZINF Studio Installer ==="

# 1. Install zinf CLI + zinf-probe if not already present
if ! command -v zinf &>/dev/null; then
    echo "Installing zinf CLI..."
    make -C "$REPO_ROOT/src" install PREFIX="$PREFIX"
fi

# 2. Install udev rules (probe rule first, then user-access rule)
echo "Installing udev rules..."
sudo install -m 644 "$SCRIPT_DIR/99-zinf.rules"      /etc/udev/rules.d/
sudo install -m 644 "$SCRIPT_DIR/99-zinf-user.rules" /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger

# 3. Add current user to plugdev group (needed for unprivileged device access)
if ! id -nG "$USER" | grep -qw plugdev; then
    echo "Adding $USER to plugdev group..."
    sudo usermod -aG plugdev "$USER"
    echo "  NOTE: Log out and back in for group membership to take effect."
fi

# 4. Install ZINF Studio binary
STUDIO_BIN="$REPO_ROOT/zinf-studio/src-tauri/target/release/zinf-studio"
if [ -f "$STUDIO_BIN" ]; then
    echo "Installing ZINF Studio..."
    sudo install -m 755 "$STUDIO_BIN" "$PREFIX/bin/zinf-studio"
else
    echo "ZINF Studio binary not found at $STUDIO_BIN"
    echo "Build it first with:"
    echo "  cd zinf-studio && bun install && bun run tauri build"
    exit 1
fi

echo ""
echo "ZINF Studio installed successfully."
echo "  Run:  zinf-studio"
echo "  Or open from your application launcher."
echo ""
echo "Quick start:"
echo "  sudo zinf format /dev/sdX    # format an SD card"
echo "  zinf-studio                  # open the GUI"
