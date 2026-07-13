#!/bin/sh
# token2-fido-bridge installer.
#
# Usage (as root):
#   curl -sSL https://token2.com/path/install.sh | sudo sh
#
# Detects the distro and installs the matching prebuilt package if BASE_URL is
# set to where the .deb/.rpm are hosted; otherwise builds from source in the
# current directory. Idempotent and safe to re-run.
set -eu

# Where prebuilt packages live (override by exporting BASE_URL before running).
BASE_URL="${BASE_URL:-}"
VERSION="${VERSION:-0.1.0}"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$1"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$1" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$1" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "Please run as root (use sudo)."

# --- detect package manager -------------------------------------------------
PKG=""
if command -v apt-get >/dev/null 2>&1; then PKG="deb"
elif command -v dnf >/dev/null 2>&1;    then PKG="rpm"; RPM_TOOL="dnf"
elif command -v zypper >/dev/null 2>&1; then PKG="rpm"; RPM_TOOL="zypper"
elif command -v yum >/dev/null 2>&1;    then PKG="rpm"; RPM_TOOL="yum"
fi

install_prebuilt() {
    [ -n "$BASE_URL" ] || return 1
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    case "$PKG" in
        deb)
            arch="$(dpkg --print-architecture)"
            f="token2-fido-bridge_${VERSION}-1_${arch}.deb"
            log "Downloading $f"
            curl -fsSL "$BASE_URL/$f" -o "$tmp/$f" || return 1
            log "Installing via apt"
            apt-get update -qq || true
            apt-get install -y "$tmp/$f"
            ;;
        rpm)
            arch="$(uname -m)"
            f="token2-fido-bridge-${VERSION}-1.${arch}.rpm"
            log "Downloading $f"
            curl -fsSL "$BASE_URL/$f" -o "$tmp/$f" || return 1
            log "Installing via $RPM_TOOL"
            "$RPM_TOOL" install -y "$tmp/$f"
            ;;
        *) return 1 ;;
    esac
}

build_from_source() {
    log "Building from source"
    case "$PKG" in
        deb) apt-get update -qq
             apt-get install -y build-essential cmake libpcsclite-dev pcscd ;;
        rpm) "$RPM_TOOL" install -y gcc-c++ cmake pcsc-lite-devel pcsc-lite ;;
        *)   die "Unsupported distro: install build-essential, cmake, and the "\
                 "pcsc-lite dev package manually." ;;
    esac

    [ -f CMakeLists.txt ] || die "Run this from the source directory, or set BASE_URL."
    cmake -B build -S . >/dev/null
    cmake --build build -j"$(nproc)" >/dev/null
    cmake --install build

    log "Enabling module + service"
    modprobe uhid 2>/dev/null || true
    udevadm control --reload-rules || true
    udevadm trigger || true
    systemctl daemon-reload || true
    systemctl enable --now token2-fido-bridge.service || true
}

log "Detected package type: ${PKG:-unknown}"
if install_prebuilt; then
    log "Installed prebuilt package."
else
    [ -n "$BASE_URL" ] && warn "Prebuilt download failed; falling back to source build."
    build_from_source
fi

# --- verify -----------------------------------------------------------------
if systemctl is-active --quiet token2-fido-bridge.service; then
    log "Service is running."
else
    warn "Service not active yet. Check: journalctl -u token2-fido-bridge -n 20"
fi

log "Ensure pcscd is running and a reader/card is present, then test at"
log "  https://webauthn.io  (Chrome recommended)."
