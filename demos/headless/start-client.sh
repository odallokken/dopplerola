#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# start-client.sh — start the headless Pulse room client in the foreground.
#
#     ./demos/headless/start-client.sh                  # uses the config below
#     ./demos/headless/start-client.sh --config my.conf
#
# Handy for a first try, for testing a camera, or for running without systemd.
# It builds the client if it is not built yet, points the dynamic linker at the
# Pulse runtime, and runs until you press Ctrl-C.
#
# Config file, first one that exists wins:
#     1. --config <path>
#     2. demos/headless/headless.conf   (created from the example on first run)
#     3. /etc/pulse-headless.conf       (what install.sh sets up)
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEMO_DIR="${REPO_ROOT}/demos/headless"
BUILD_DIR="${REPO_ROOT}/build"
BINARY="${BUILD_DIR}/demos/headless/pulse_headless"

PEXIP_PREFIX="${PEXIP_PREFIX:-/opt/pexip}"
CONFIG=""

while [ $# -gt 0 ]; do
    case "$1" in
        --config|-c) CONFIG="${2:?--config needs a path}"; shift 2 ;;
        -h|--help)   sed -n '2,16p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *)           echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

# ---- The Pulse runtime -----------------------------------------------------
if [ ! -f "${PEXIP_PREFIX}/lib/libpexpulse.so" ]; then
    echo "The Pexip Pulse runtime is not installed under ${PEXIP_PREFIX}." >&2
    echo "Run this once:  sudo ${DEMO_DIR}/install.sh" >&2
    exit 1
fi

# ---- Build on demand -------------------------------------------------------
if [ ! -x "${BINARY}" ]; then
    echo "==> Building the client (first run)"
    cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DPEXIP_PREFIX="${PEXIP_PREFIX}" \
          -DBUILD_DOPPLER=OFF -DBUILD_GATEWAY=OFF -DBUILD_VIDEOWALL=OFF -DBUILD_HEADLESS=ON
    cmake --build "${BUILD_DIR}" -j"$(nproc)"
fi

# ---- Pick a config ---------------------------------------------------------
if [ -z "${CONFIG}" ]; then
    if [ -f "${DEMO_DIR}/headless.conf" ]; then
        CONFIG="${DEMO_DIR}/headless.conf"
    elif [ -f /etc/pulse-headless.conf ]; then
        CONFIG="/etc/pulse-headless.conf"
    else
        install -m 600 "${DEMO_DIR}/headless.conf.example" "${DEMO_DIR}/headless.conf"
        echo "Created ${DEMO_DIR}/headless.conf — set 'host', 'conference' and"
        echo "'pin' in it, then run this script again."
        exit 1
    fi
fi

if [ ! -r "${CONFIG}" ]; then
    echo "Cannot read config file '${CONFIG}'." >&2
    exit 1
fi

# ---- Run -------------------------------------------------------------------
# LD_LIBRARY_PATH finds Pulse's private sibling libraries (libpexlgpl,
# libonnxruntime.so.1, ...); the runtime aborts at startup without
# PEX_BASE_PATH. Editing the config file while this runs moves the client to
# another meeting room - no restart needed.
export LD_LIBRARY_PATH="${PEXIP_PREFIX}/lib:${LD_LIBRARY_PATH:-}"
export PEX_BASE_PATH="${PEX_BASE_PATH:-${PEXIP_PREFIX}}"

echo "==> Starting the client with ${CONFIG} (Ctrl-C to stop)"
exec "${BINARY}" --config "${CONFIG}"
