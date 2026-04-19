# Task 4: Linux Installer & Udev Setup

**Context:**
Create the one-click deployment for ZINF Studio on Linux. After running `install.sh`, the Tauri app has permission to read raw block devices without `sudo`, and ZINF-formatted SD cards are detected automatically when inserted.

> **Note:** The `zinf` CLI and `zinf-probe` are installed separately via `sudo make install` (handled by `src/Makefile`). This task is exclusively about installing the **ZINF Studio desktop app** and the user-space udev rule that grants it unprivileged block device access.

> **Tauri built-in updater:** Deferred. Leave the `updater` key absent from `tauri.conf.json` for now. Add it when a release endpoint (GitHub Releases or custom server) is available.

---

## Step-by-Step Instructions for LLM

### 1. Create `tools/99-zinf-user.rules`

This is a *separate* rule from the existing `tools/99-zinf.rules` (which populates `ID_FS_TYPE` via `zinf-probe`). This new rule grants the `plugdev` group read/write access to ZINF block devices so ZINF Studio can open them without `sudo`.

```
# /etc/udev/rules.d/99-zinf-user.rules
# Grant plugdev group access to ZINF-formatted block devices.
# Requires zinf-probe to be installed (sets ID_FS_TYPE=zinf via 99-zinf.rules).

ENV{ID_FS_TYPE}=="zinf", GROUP="plugdev", MODE="0660"
```

**Dependency:** `99-zinf.rules` must already be installed (it sets `ID_FS_TYPE=zinf`). The installer checks for this and installs `99-zinf.rules` first if missing.

### 2. Write `tools/install.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
PREFIX="${PREFIX:-/usr/local}"

echo "=== ZINF Studio Installer ==="

# 1. Install zinf-probe + zinf CLI if not already present
if ! command -v zinf &>/dev/null; then
    echo "Installing zinf CLI..."
    make -C "$REPO_ROOT/src" install PREFIX="$PREFIX"
fi

# 2. Install udev rules
echo "Installing udev rules..."
sudo install -m 644 "$SCRIPT_DIR/99-zinf.rules"      /etc/udev/rules.d/
sudo install -m 644 "$SCRIPT_DIR/99-zinf-user.rules" /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger

# 3. Add current user to plugdev group
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
    echo "Build it first with: cd zinf-studio && npm install && npm run tauri build"
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
```

Make it executable: `chmod +x tools/install.sh`

### 3. Update Root `Makefile` — Add `install-studio` Target

In the repo-root `Makefile`, add:

```makefile
install-studio:
	@echo "Building ZINF Studio..."
	cd zinf-studio && npm install && npm run tauri build
	tools/install.sh

.PHONY: all install uninstall clean install-studio
```

This means a researcher can run:
```bash
sudo make install          # CLI + zinf-probe + udev probe rule
make install-studio        # Studio app + plugdev udev rule
```

### 4. `tauri.conf.json` Permissions

In `zinf-studio/src-tauri/tauri.conf.json`, configure the minimum necessary permissions:

```json
{
  "tauri": {
    "allowlist": {
      "fs": {
        "all": true,
        "scope": ["/dev/**", "/sys/block/**", "$HOME/**"]
      },
      "dialog": { "save": true, "open": true }
    },
    "bundle": {
      "identifier": "org.zinf.studio",
      "icon": ["icons/32x32.png", "icons/128x128.png"]
    }
  }
}
```

Do NOT add an `"updater"` key — deferred until a release endpoint exists.

### 5. Verify Installation Flow

After `make install-studio` completes, a researcher should be able to:

```bash
# Insert ZINF-formatted SD card → udev fires both rules:
udevadm info /dev/sdb | grep ID_FS  # → ID_FS_TYPE=zinf
ls -l /dev/sdb                      # → crw-rw---- root plugdev

# Open Studio without sudo
zinf-studio
# → Device list shows /dev/sdb (zinf v4)
```

---

## Deliverables Checklist

- [ ] `tools/99-zinf-user.rules` — plugdev access rule
- [ ] `tools/install.sh` — installs Studio binary + both udev rules + plugdev group
- [ ] Root `Makefile` — `install-studio` target
- [ ] `tauri.conf.json` — fs permissions, no updater key
- [ ] Tested: non-sudo user in `plugdev` can open `/dev/sdX` from ZINF Studio
