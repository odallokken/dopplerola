# Pexip Pulse — Showcase

A collection of small, self-contained **demo applications** built on the
**Pexip Pulse** SDK. Each demo highlights a slice of what Pulse can do, and is
designed so you can `cd` into it, read a short README, run two or three
commands, and see it work.

New to Pulse? Start with [`doppler`](demos/doppler/) (a video call in one file),
then browse [`pexninja`](demos/pexninja/) when you want to see the full breadth
of the SDK. Extending the repo? Read [`AGENTS.md`](AGENTS.md) and
[`docs/adding-a-demo.md`](docs/adding-a-demo.md).

## The demos

| Demo | What it shows | Platform / build |
| ---- | ------------- | ---------------- |
| [**doppler**](demos/doppler/) | A Pexip Infinity video call in one heavily-commented file: connect over REST, render the remote + self-view yourself, plus RTMP ingest and a camera-on-RTMP "Twitch mix". | C++ / CMake · built by default |
| [**gateway**](demos/gateway/) | Two Pulse instances bridged together, relaying raw audio + video between two conferences using only the data-session input/output APIs — a tiny cross-domain "guard". | C++ / CMake · built by default |
| [**videowall**](demos/videowall/) | A control-room **production switcher**: prepare sources into a live **library**, then point-click-drag them onto a superwide **video wall** *and* a per-conference **Send** canvas (plus a **Presentation** bus) that is composited and pushed *back* to each far end — **one Pulse instance per source**, the inverse of pexninja's single-instance compositor. | C++ / CMake · built by default |
| [**headless**](demos/headless/) | An unattended room client with **no UI at all**: reads a config file (host, meeting-room URI, PIN), waits for the USB webcam, dials the room and keeps the picture flowing — camera hotplug, device errors and stalled video all recover on their own. Built for a **Raspberry Pi 4** running Ubuntu Server. | C++ / CMake · built by default (no GUI deps) |
| [**sip**](demos/sip/) | Pulse used purely as a media engine, with **PJSIP** doing the SIP signalling (manual SDP offer/answer). No Pexip Infinity node required. | C++ / CMake · opt-in (needs PJSIP) |
| [**pexninja**](demos/pexninja/) | The full reference client (~12k LoC) exercising the **widest array** of Pulse functionality — conference control, roster, devices, annotation, media stats and much more. | C++ / CMake (Linux, macOS) · MSVC (Windows, via the Pulse NuGet) · opt-in (extra deps) |
| [**android**](demos/android/) | A Kotlin/Gradle sample app using the **Pulse Android SDK**: preflight, join, roster, chat, screen share, media stats. | Android / Gradle |
| [**windows**](demos/windows/) | A native Windows **WinForms** app that places a call through the **Pexip.Pulse NuGet package**: connect over REST, two-way audio + video using the default devices, with the remote and self-view video rendered inside the window. | Windows / .NET (WinForms) |

## 1. Install the Pexip Pulse runtime

The C/C++ demos link against the Pulse library + headers. The SDK artifacts ship
in this repo under [`sdk/`](sdk/), organised by platform.

### Linux (Ubuntu 24.04)

Install the `.deb` packages — `apt -f install` pulls in the couple of system
libs they need (`libpulse0`, `libspeex1`, `libxv1`, ...):

```bash
sudo dpkg -i sdk/linux/debs/libpexcommon_*.deb \
             sdk/linux/debs/libpexpulse_*.deb  \
             sdk/linux/debs/libpexpulse-dev_*.deb
sudo apt-get install -f
```

This installs the headers under `/opt/pexip/include` and the shared library
under `/opt/pexip/lib`.

### Linux on ARM (Raspberry Pi 4, Ubuntu Server 24.04)

The arm64 packages ship under [`sdk/linux_arm/deb/`](sdk/linux_arm/deb/). The
helper script installs them (and the system libs they need) in one go:

```bash
sudo scripts/install-pulse-arm.sh
```

They install to the same `/opt/pexip` prefix, so the CMake build finds them
without any extra flags. See [`demos/headless/`](demos/headless/) for the demo
that targets these boxes.

### macOS

The Pulse dylibs ship under [`sdk/macos/`](sdk/macos/); point CMake at them with
`-DPEXIP_PREFIX="$(pwd)/sdk/macos"` (see the per-demo READMEs).

## 2. Install the build dependencies

```bash
sudo apt-get install -y cmake build-essential \
                        libglfw3-dev libgl1-mesa-dev
```

`libglfw3-dev` / `libgl1-mesa-dev` are only needed by the demos that draw a UI.
On a headless box, build just [`headless`](demos/headless/) (see below) and
`cmake build-essential` is enough.

Individual demos may need a little more (e.g. `pexninja` wants `libx11-dev`,
`sip` needs PJSIP) — each demo's README spells out its extras.

## 3. Build & run

The build is a thin orchestrator: every demo is a CMake sub-project gated behind
a `BUILD_<DEMO>` option. The lightweight demos build by default:

```bash
cmake -S . -B build
cmake --build build -j
./build/run-doppler.sh          # or ./build/run-pulse-gateway.sh
```

On the first configure, CMake fetches Dear ImGui from GitHub (≈ 5 MB). Enable
the opt-in demos with their options, e.g. `-DBUILD_PEXNINJA=ON` or
`-DBUILD_DOPPLER_SIP=ON` — see each demo's README for details.

Turning all the GUI demos off skips the GLFW/OpenGL/ImGui lookup entirely,
which is how the headless client builds on a display-less box:

```bash
cmake -S . -B build -DBUILD_DOPPLER=OFF -DBUILD_GATEWAY=OFF -DBUILD_VIDEOWALL=OFF
cmake --build build -j
./build/run-headless.sh --config headless.conf
```

Each demo ships a generated `run-<demo>.sh` launcher that puts Pulse's private
sibling libraries on the dynamic-linker search path and exports `PEX_BASE_PATH`
(the Pulse runtime aborts at startup without it). If you'd rather run the
binary directly:

```bash
PEX_BASE_PATH=/opt/pexip LD_LIBRARY_PATH=/opt/pexip/lib ./build/demos/doppler/doppler
# or, system-wide (still set PEX_BASE_PATH in your environment):
echo /opt/pexip/lib | sudo tee /etc/ld.so.conf.d/pexip.conf && sudo ldconfig
```

## Repository layout

```
demos/      One folder per demo (source + README + CMakeLists).
sdk/        The Pulse SDK artifacts: linux/ (debs + extracted), macos/, windows/.
cmake/      Shared CMake helpers (PulseDemo.cmake).
docs/       adding-a-demo.md, a DEMO_TEMPLATE/ skeleton, and api/ — the
            generated Pulse C API reference (open docs/api/index.html).
scripts/    Helper scripts (e.g. install-pjsip.sh).
AGENTS.md   How to relate to the Pulse SDK and extend this repo.
```

## Adding your own demo

The repo is built to grow. Copy [`docs/DEMO_TEMPLATE/`](docs/DEMO_TEMPLATE/),
drop in your source, wire up a few lines of CMake using the shared helpers, and
add a row to the table above. The full walk-through — including the conventions
that keep every demo easy to build and run — is in
[`docs/adding-a-demo.md`](docs/adding-a-demo.md), and the SDK orientation for
agents and humans alike is in [`AGENTS.md`](AGENTS.md).
