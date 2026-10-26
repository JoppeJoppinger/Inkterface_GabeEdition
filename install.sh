#!/usr/bin/env bash
# One-click installer for Inkterface Gabe Edition (Linux / SteamOS).
# Flashes the firmware onto the Feather over USB and installs the host app
# (AppImage + user service) on this machine.
set -euo pipefail

REPO="JoppeJoppinger/Inkterface_GabeEdition"
ESPTOOL_VERSION="v5.6.0"
ESPTOOL_SHA256_AMD64="1590723cfa9675983f49d0bf2895b8507f8d2879082e51bb7ef02a8bca88d354"
ESPTOOL_SHA256_AARCH64="7723120429394b4d6d974d694e5cf378e00259381a7f8856c2c88492041b9f59"

BOARD=""
PORT=""
VERSION="latest"
LOCAL_DIR=""
SKIP_FIRMWARE=0
SKIP_APP=0
DRY_RUN=0
APP_DIR="$HOME/Applications"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/inkterface-installer"

usage() {
    cat <<EOF
Usage: install.sh [options]
  --board v1|v2      Feather version (default: auto-detect from the connected chip)
  --port PORT        Serial port (default: auto-detect)
  --version TAG      Release tag to install (default: latest)
  --local DIR        Use inkterface-firmware-*.bin / Inkterface.AppImage from DIR
  --skip-firmware    Only install the host app
  --skip-app         Only flash the firmware
  --dry-run          Show what would be done without changing anything
  -h, --help         Show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --board) BOARD="${2:-}"; shift 2 ;;
        --port) PORT="${2:-}"; shift 2 ;;
        --version) VERSION="${2:-}"; shift 2 ;;
        --local) LOCAL_DIR="${2:-}"; shift 2 ;;
        --skip-firmware) SKIP_FIRMWARE=1; shift ;;
        --skip-app) SKIP_APP=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
run() {
    if [ "$DRY_RUN" = 1 ]; then printf '[dry-run] %s\n' "$*"; else "$@"; fi
}
need() { command -v "$1" >/dev/null 2>&1 || die "'$1' is required but not installed."; }

case "$BOARD" in ""|v1|v2) ;; *) die "--board must be v1 or v2" ;; esac
[ "$SKIP_FIRMWARE" = 1 ] && [ "$SKIP_APP" = 1 ] && die "Nothing to do."

mkdir -p "$CACHE"
need curl
need sha256sum

if [ "$VERSION" = "latest" ]; then
    BASE_URL="https://github.com/$REPO/releases/latest/download"
else
    BASE_URL="https://github.com/$REPO/releases/download/$VERSION"
fi

fetch() { # fetch <name> -> echoes local path
    local name="$1"
    if [ -n "$LOCAL_DIR" ]; then
        [ -f "$LOCAL_DIR/$name" ] || die "$LOCAL_DIR/$name not found"
        echo "$LOCAL_DIR/$name"
        return
    fi
    local dest="$CACHE/$name"
    if [ "$DRY_RUN" = 1 ]; then
        echo "[dry-run] download $BASE_URL/$name" >&2
        echo "$dest"
        return
    fi
    curl -fL --retry 3 -o "$dest" "$BASE_URL/$name" >&2 || die "Download of $name failed. No release yet? Try --local DIR."
    echo "$dest"
}

verify_release_file() { # verify_release_file <path> <name>
    [ -n "$LOCAL_DIR" ] && return 0
    [ "$DRY_RUN" = 1 ] && return 0
    if [ ! -f "$CACHE/SHA256SUMS" ]; then
        curl -fL --retry 3 -o "$CACHE/SHA256SUMS" "$BASE_URL/SHA256SUMS" || die "Could not download SHA256SUMS"
    fi
    local expected actual
    expected=$(awk -v n="$2" '$2==n || $2=="*"n {print $1}' "$CACHE/SHA256SUMS")
    [ -n "$expected" ] || die "No checksum for $2"
    actual=$(sha256sum "$1" | awk '{print $1}')
    [ "$expected" = "$actual" ] || die "Checksum mismatch for $2"
}

setup_esptool() {
    local arch asset sha
    case "$(uname -m)" in
        x86_64|amd64) arch=amd64; sha="$ESPTOOL_SHA256_AMD64" ;;
        aarch64|arm64) arch=aarch64; sha="$ESPTOOL_SHA256_AARCH64" ;;
        *) die "Unsupported CPU architecture: $(uname -m)" ;;
    esac
    asset="esptool-$ESPTOOL_VERSION-linux-$arch.tar.gz"
    ESPTOOL="$CACHE/esptool-linux-$arch/esptool"
    [ -x "$ESPTOOL" ] && return
    log "Downloading esptool $ESPTOOL_VERSION"
    if [ "$DRY_RUN" = 1 ]; then echo "[dry-run] download and extract $asset"; return; fi
    curl -fL --retry 3 -o "$CACHE/$asset" \
        "https://github.com/espressif/esptool/releases/download/$ESPTOOL_VERSION/$asset"
    echo "$sha  $CACHE/$asset" | sha256sum -c - >/dev/null || die "esptool checksum mismatch"
    tar -xzf "$CACHE/$asset" -C "$CACHE"
}

flash_firmware() {
    log "Flashing firmware"
    setup_esptool
    local port_args=()
    [ -n "$PORT" ] && port_args=(--port "$PORT")

    if [ -z "$BOARD" ]; then
        if [ "$DRY_RUN" = 1 ]; then
            BOARD=v2
            echo "[dry-run] board auto-detect via esptool chip-id (assuming v2)"
        else
            echo "Detecting board (connect the Feather via USB)..."
            local info
            info=$("$ESPTOOL" "${port_args[@]}" chip-id 2>&1) || {
                echo "$info" >&2
                if ! id -nG | tr ' ' '\n' | grep -qx -e dialout -e uucp -e plugdev; then
                    echo "Hint: add yourself to the serial group: sudo usermod -aG dialout \$USER (then log in again)" >&2
                fi
                die "No Feather found. Use a data USB cable; hold BOOT and press RESET to enter the bootloader."
            }
            case "$info" in
                *ESP32-S3*) BOARD=v1 ;;
                *ESP32*) BOARD=v2 ;;
                *) die "Unrecognised chip; use --board v1|v2" ;;
            esac
            if [ -z "$PORT" ]; then
                PORT=$(printf '%s\n' "$info" | sed -n 's/^Serial port //p' | head -n1)
                [ -n "$PORT" ] && port_args=(--port "$PORT")
            fi
            echo "Detected board: $BOARD on ${PORT:-auto}"
        fi
    fi

    local name="inkterface-firmware-feather$BOARD.bin" file
    file=$(fetch "$name")
    verify_release_file "$file" "$name"
    [ "$BOARD" = v1 ] && echo "Note: the V1 firmware has not been tested on hardware."
    run "$ESPTOOL" "${port_args[@]}" write-flash 0x0 "$file"
    echo "Firmware flashed. The display shows 'Waiting on connection' until the host app connects."
}

install_app() {
    log "Installing host app"
    local file
    file=$(fetch "Inkterface.AppImage")
    verify_release_file "$file" "Inkterface.AppImage"

    local target="$APP_DIR/Inkterface.AppImage"
    run mkdir -p "$APP_DIR"
    if command -v systemctl >/dev/null 2>&1 && [ "$DRY_RUN" = 0 ]; then
        systemctl --user stop inkterface.service 2>/dev/null || true
    fi
    run install -m 755 "$file" "$target"

    if ! command -v systemctl >/dev/null 2>&1; then
        echo "systemctl not found: start '$target --headless' manually."
        return
    fi

    local unit_dir="$HOME/.config/systemd/user"
    local unit="$unit_dir/inkterface.service"
    run mkdir -p "$unit_dir"
    if [ "$DRY_RUN" = 1 ]; then
        echo "[dry-run] write $unit (ExecStart=$target --headless)"
    else
        cat >"$unit" <<EOF
[Unit]
Description=inkterface
After=network.target

[Service]
Type=simple
ExecStart=$target --headless
Restart=on-failure

[Install]
WantedBy=default.target
EOF
    fi
    run systemctl --user daemon-reload
    run systemctl --user enable inkterface.service
    run systemctl --user restart inkterface.service
    echo "Service installed and started. Check: systemctl --user status inkterface.service"
    echo "Make sure Bluetooth is enabled. Do NOT pair the Inkterface in the Bluetooth settings."
    echo "Run '$target' once in Desktop Mode to select your panel (INKTF-xxxxxx) and display mode."
}

[ "$SKIP_FIRMWARE" = 1 ] || flash_firmware
[ "$SKIP_APP" = 1 ] || install_app
log "Done"
