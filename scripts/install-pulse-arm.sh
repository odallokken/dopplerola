#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# install-pulse-arm.sh — install the arm64 Pexip Pulse SDK from sdk/linux_arm/
#
# For 64-bit ARM boxes running Ubuntu 24.04 — a Raspberry Pi 4 running Ubuntu
# Server, say, which is what demos/headless targets. It dpkg-installs the three
# .deb packages that ship in this repo and lets apt pull in the system libs
# they depend on:
#
#     libpexcommon      the Pulse runtime's private siblings + models
#     libpexpulse       libpexpulse.so itself
#     libpexpulse-dev   the pexpulse/*.h headers
#
# Everything lands under /opt/pexip, which is where cmake/PulseDemo.cmake looks
# by default.
#
# Idempotent: re-running it simply reinstalls the same packages.
#
#     sudo scripts/install-pulse-arm.sh
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEB_DIR="${DEB_DIR:-${REPO_ROOT}/sdk/linux_arm/deb}"

# ---- Sanity ---------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    echo "This script needs to run as root (it installs into /opt/pexip)." >&2
    echo "Try:  sudo $0" >&2
    exit 1
fi

arch="$(dpkg --print-architecture)"
if [ "${arch}" != "arm64" ]; then
    echo "This machine reports dpkg architecture '${arch}', not 'arm64'." >&2
    echo "Use the packages in sdk/linux/debs/ for x86-64 instead." >&2
    exit 1
fi

if [ ! -d "${DEB_DIR}" ]; then
    echo "No .deb directory at ${DEB_DIR}" >&2
    exit 1
fi

# ---- Collect the packages -------------------------------------------------
# Order matters only in that dpkg wants them all on one command line: they
# depend on each other, and apt-get -f install afterwards resolves the rest.
shopt -s nullglob
debs=(
    "${DEB_DIR}"/libpexcommon_*.deb
    "${DEB_DIR}"/libpexpulse_*.deb
    "${DEB_DIR}"/libpexpulse-dev_*.deb
)
shopt -u nullglob

if [ "${#debs[@]}" -eq 0 ]; then
    echo "No .deb packages found in ${DEB_DIR}" >&2
    exit 1
fi

echo "==> Installing the Pulse SDK packages"
printf '    %s\n' "${debs[@]}"
dpkg -i "${debs[@]}" || true   # unmet deps are expected; apt fixes them next

echo "==> Resolving dependencies"
apt-get update
apt-get install -f -y

# ---- The build tools the demos need ---------------------------------------
# Only the headless demo builds without a GUI stack, so keep this to the bare
# minimum; the GUI demos' READMEs list libglfw3-dev/libgl1-mesa-dev.
echo "==> Installing build tools"
apt-get install -y cmake build-essential

echo
echo "Done. Pulse is installed under /opt/pexip:"
echo "    headers : /opt/pexip/include/pexpulse"
echo "    library : /opt/pexip/lib/libpexpulse.so"
echo
echo "Build the headless client with:"
echo "    cmake -S . -B build -DBUILD_DOPPLER=OFF -DBUILD_GATEWAY=OFF -DBUILD_VIDEOWALL=OFF"
echo "    cmake --build build -j"
