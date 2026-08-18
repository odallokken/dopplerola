#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# install.sh — one-command install of the headless Pulse room client.
#
#     git clone https://github.com/odallokken/dopplerola.git
#     cd dopplerola
#     sudo ./demos/headless/install.sh
#
# It installs the Pexip Pulse SDK that ships in this repo, builds the client,
# asks for the meeting room details the first time, and enables a systemd
# service so the box joins the room on every boot.
#
# Re-run it after a `git pull` to upgrade: the build is refreshed, the service
# restarted, and your /etc/pulse-headless.conf is left exactly as it is.
#
#     --no-service      build and install the binary only, no systemd
#     --config <path>   use a different config location
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEMO_DIR="${REPO_ROOT}/demos/headless"
BUILD_DIR="${REPO_ROOT}/build"

CONFIG_PATH="/etc/pulse-headless.conf"
BIN_PATH="/usr/local/bin/pulse-headless"
SERVICE_NAME="pulse-headless"
SERVICE_PATH="/etc/systemd/system/${SERVICE_NAME}.service"
SERVICE_USER="pulse"
INSTALL_SERVICE=1

while [ $# -gt 0 ]; do
    case "$1" in
        --no-service) INSTALL_SERVICE=0; shift ;;
        --config)     CONFIG_PATH="${2:?--config needs a path}"; shift 2 ;;
        -h|--help)    sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *)            echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "This installer needs root. Try:  sudo $0" >&2
    exit 1
fi

say() { printf '\n==> %s\n' "$1"; }

# ---- 1. The Pulse SDK ------------------------------------------------------
arch="$(dpkg --print-architecture)"
case "${arch}" in
    arm64)  DEB_DIR="${REPO_ROOT}/sdk/linux_arm/deb" ;;   # Raspberry Pi 4, ...
    amd64)  DEB_DIR="${REPO_ROOT}/sdk/linux/debs"   ;;
    *)      echo "Unsupported architecture '${arch}' - no Pulse packages for it." >&2
            exit 1 ;;
esac

say "Installing the Pexip Pulse runtime for ${arch}"
shopt -s nullglob
debs=("${DEB_DIR}"/libpexcommon_*.deb "${DEB_DIR}"/libpexpulse_*.deb "${DEB_DIR}"/libpexpulse-dev_*.deb)
shopt -u nullglob
if [ "${#debs[@]}" -eq 0 ]; then
    echo "No .deb packages found in ${DEB_DIR}" >&2
    exit 1
fi
dpkg -i "${debs[@]}" || true    # unmet deps are expected; apt fixes them next
apt-get update
apt-get install -f -y

say "Installing build tools"
apt-get install -y cmake build-essential

# ---- 2. Build the client ---------------------------------------------------
# Only this demo: the GUI demos are switched off so nothing pulls in GLFW,
# OpenGL or Dear ImGui (and no network fetch is needed).
say "Building the client"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DPEXIP_PREFIX=/opt/pexip \
      -DBUILD_DOPPLER=OFF -DBUILD_GATEWAY=OFF -DBUILD_VIDEOWALL=OFF -DBUILD_HEADLESS=ON
cmake --build "${BUILD_DIR}" -j"$(nproc)"

install -m 755 "${BUILD_DIR}/demos/headless/pulse_headless" "${BIN_PATH}"
echo "Installed ${BIN_PATH}"

# Leave the build tree owned by whoever invoked sudo, so a later `git pull`
# and rebuild does not need root.
if [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != "root" ]; then
    chown -R "${SUDO_USER}:$(id -gn "${SUDO_USER}")" "${BUILD_DIR}" 2>/dev/null || true
fi

# ---- 3. First-time configuration ------------------------------------------
# An existing config is never touched - that is what makes re-running this
# script after a `git pull` safe.
if [ -f "${CONFIG_PATH}" ]; then
    say "Keeping the existing configuration at ${CONFIG_PATH}"
else
    say "First-time configuration"
    host=""; conference=""; pin=""
    if [ -t 0 ]; then
        while [ -z "${host}" ]; do
            read -r -p "Pexip server hostname (e.g. pexip.example.com): " host
        done
        while [ -z "${conference}" ]; do
            read -r -p "Meeting room URI      (e.g. ola@pexpo.net)    : " conference
        done
        read -r -p "Meeting room PIN        (blank if none)           : " pin
        read -r -p "Name shown in the meeting [Raspberry Pi]          : " display_name
        display_name="${display_name:-Raspberry Pi}"

        # Start from the annotated example so every option stays documented,
        # then fill in the four answers. awk (not sed) so that characters like
        # '&' or '\' in a PIN or hostname are copied through verbatim.
        tmp="$(mktemp)"
        HL_HOST="${host}" HL_CONF="${conference}" HL_PIN="${pin}" HL_NAME="${display_name}" \
        awk '
            /^[[:space:]]*host[[:space:]]*=/         { print "host = "         ENVIRON["HL_HOST"]; next }
            /^[[:space:]]*conference[[:space:]]*=/   { print "conference = "   ENVIRON["HL_CONF"]; next }
            /^[[:space:]]*pin[[:space:]]*=/          { print "pin = "          ENVIRON["HL_PIN"];  next }
            /^[[:space:]]*display_name[[:space:]]*=/ { print "display_name = " ENVIRON["HL_NAME"]; next }
            { print }
        ' "${DEMO_DIR}/headless.conf.example" > "${tmp}"
        install -m 600 "${tmp}" "${CONFIG_PATH}"
        rm -f "${tmp}"
    else
        install -m 600 "${DEMO_DIR}/headless.conf.example" "${CONFIG_PATH}"
        echo "Wrote a template to ${CONFIG_PATH} - edit host/conference/pin before starting."
    fi
fi

# ---- 4. The service --------------------------------------------------------
if [ "${INSTALL_SERVICE}" -eq 1 ]; then
    say "Installing the ${SERVICE_NAME} service"

    # An unprivileged account that is allowed to open /dev/video* and the
    # sound devices.
    if ! id -u "${SERVICE_USER}" >/dev/null 2>&1; then
        useradd --system --user-group --groups video,audio \
                --shell /usr/sbin/nologin "${SERVICE_USER}"
        echo "Created the '${SERVICE_USER}' system account"
    else
        usermod -aG video,audio "${SERVICE_USER}"
    fi
    SERVICE_GROUP="$(id -gn "${SERVICE_USER}")"
    chown "${SERVICE_USER}:${SERVICE_GROUP}" "${CONFIG_PATH}"
    chmod 600 "${CONFIG_PATH}"

    HL_EXEC="ExecStart=${BIN_PATH} --config ${CONFIG_PATH}" HL_GROUP="Group=${SERVICE_GROUP}" \
    awk '/^ExecStart=/ { print ENVIRON["HL_EXEC"];  next }
         /^Group=/     { print ENVIRON["HL_GROUP"]; next }
         { print }' \
        "${DEMO_DIR}/systemd/${SERVICE_NAME}.service" > "${SERVICE_PATH}"
    chmod 644 "${SERVICE_PATH}"

    systemctl daemon-reload
    systemctl enable "${SERVICE_NAME}"
    systemctl restart "${SERVICE_NAME}"
fi

# ---- Done ------------------------------------------------------------------
cat <<EOF

============================================================================
 The Pexip Pulse room client is installed.

 Configuration : ${CONFIG_PATH}
 Binary        : ${BIN_PATH}
EOF

if [ "${INSTALL_SERVICE}" -eq 1 ]; then
cat <<EOF
 Service       : ${SERVICE_NAME} (starts automatically on every boot)

 Dial a different room later — edit the file and save it, nothing else:

     sudo nano ${CONFIG_PATH}

 The change is picked up within a couple of seconds; the client leaves the
 old room and joins the new one on its own.

 Watch what it is doing:   journalctl -u ${SERVICE_NAME} -f
 Stop / start:             sudo systemctl stop|start ${SERVICE_NAME}
EOF
else
cat <<EOF

 Run it in the foreground with:

     ${REPO_ROOT}/demos/headless/start-client.sh
EOF
fi

cat <<EOF

 Upgrade later with:       git pull && sudo ./demos/headless/install.sh
============================================================================
EOF
