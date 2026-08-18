# headless — an unattended Pexip Pulse room client

A video conference client with no user interface at all. It reads a config
file, dials a pre-defined Virtual Meeting Room and sends the webcam picture
into it — then keeps it that way. Built for a **Raspberry Pi 4 running Ubuntu
Server 24.04**: no display, no keyboard, one USB webcam, powered on and left
alone.

Everything is done through the **Pulse C API** — `pulse_connect_with_rest_async()`
talks REST to a Pexip Conferencing Node. There is no SIP stack involved (that is
the separate, opt-in [`sip`](../sip/) demo).

```
   boot ──► systemd ──► pulse-headless ──► wait for webcam ──► dial the VMR
                              ▲                   │                  │
                              └───────── supervise ┴──────────────────┘
                        (camera hotplug, device errors, stalled video,
                         dropped call → re-attach / redial, forever)
```

## What it shows

* Driving Pulse **headlessly** — all four video window handles are pinned to
  `NULL` (`pulse_options_set_self_view_window_handle()` and friends), so Pulse
  never tries to open a native window on a box with no display server.
* Attaching a specific capture device to the outgoing MAIN video with
  `pulse_device_session_connect_device()`.
* Making the camera **survive the real world**, which is the whole point of
  this demo:

  | Failure mode | What the client does |
  | ------------ | -------------------- |
  | Webcam enumerates *after* the network on a cold boot | Waits for it (`camera_wait_secs`, default: forever) instead of joining blind |
  | Webcam plugged in / re-enumerated later | `pulse_register_device_list_changed_callback()` → attach it, no restart needed |
  | Camera errors out mid-call | `pulse_register_device_error_callback()` → re-attach |
  | Device session silently disappears | `pulse_device_session_is_connected_by_id()` polled every 5 s → re-attach |
  | Camera is "attached" but no picture ever reaches the far end | `pulse_media_stats_get()` watches `video_tx.total_packets_sent`; if it stops moving, re-attach the camera, then redial the call |
  | Node unreachable / call dropped | Redials with exponential backoff, forever |

* Answering a PIN-protected meeting room non-interactively from the config file
  (`PulseRestConnectionConfig::pin_code`, plus
  `pulse_options_set_pin_code_request_callbacks()` for when the node asks).
* Re-reading the config file while running, so changing meeting room is a
  one-line edit rather than a service restart.

## Configuration

The client is configured entirely from a file — nothing is ever asked
interactively. See [`headless.conf.example`](headless.conf.example) for the
annotated version; the three keys that matter are:

```ini
host       = pexip.example.com   # the Pexip node to connect to
conference = ola@pexpo.net       # the virtual meeting room URI
pin        = 1234                # optional PIN for the meeting room
```

Everything else has a sensible default: `display_name`, device selection
(`camera` / `microphone` / `speaker` — `auto`, `none`, or part of a device
name), the retry backoff, and the stalled-video watchdog timeout.

The file is re-read while the client runs: change the room (or the camera) and
save, and the change is applied within a couple of seconds — see
[Dialling a different room later](#dialling-a-different-room-later).

> The file can contain a meeting-room PIN — install it as `0600` and owned by
> the account the service runs as. `install.sh` does that for you.

## Install it on the Pi (the short version)

Two commands on a fresh Ubuntu Server 24.04 install — the installer asks for the
server, the room URI and the PIN, then sets everything up:

```bash
git clone https://github.com/odallokken/dopplerola.git
cd dopplerola
sudo ./demos/headless/install.sh
```

That single script installs the Pulse SDK for the machine's architecture (arm64
on the Pi), builds this demo, writes `/etc/pulse-headless.conf`, creates an
unprivileged `pulse` account in the `video` and `audio` groups, and enables the
`pulse-headless` systemd service. From then on the Pi joins the meeting room on
every boot with no human interaction.

```bash
journalctl -u pulse-headless -f      # watch it join
```

**Upgrading** is the same two commands — the config file is never overwritten:

```bash
git pull && sudo ./demos/headless/install.sh
```

### Dialling a different room later

Edit the config file and save it. That is all:

```bash
sudo nano /etc/pulse-headless.conf
```

The running client re-reads the file within a couple of seconds, leaves the old
room and joins the new one. No restart, no reboot, no service commands. A file
that is half-written or has a typo is rejected — the client logs the problem and
carries on with the previous settings.

### Running it by hand

To try it out from the checkout, without systemd (Ctrl-C to stop):

```bash
./demos/headless/start-client.sh                    # or --config <path>
```

It builds the client on first use and picks up `demos/headless/headless.conf`
(created from the example if missing), or `/etc/pulse-headless.conf`.

## Build & run manually

If you would rather do it yourself: this demo needs **no** GLFW/OpenGL/ImGui, so
nothing pulls in a GUI stack.

```bash
sudo scripts/install-pulse-arm.sh                   # arm64; x86-64: sdk/linux/debs/*.deb
cmake -S . -B build -DBUILD_DOPPLER=OFF -DBUILD_GATEWAY=OFF -DBUILD_VIDEOWALL=OFF
cmake --build build -j
cp demos/headless/headless.conf.example headless.conf   # edit host/conference/pin
./build/run-headless.sh --config headless.conf
```

The systemd unit in [`systemd/`](systemd/) is the one `install.sh` deploys; see
its header for the manual steps.

## Notes for the Raspberry Pi

* **The webcam.** The client never assumes the camera is present — that is what
  the table above is about — so you should not need udev rules, sleeps or
  `ExecStartPre` device waits. If the camera has still not appeared, the
  journal says so once every 15 s (`no camera matching 'auto' available yet`).
  `v4l2-ctl --list-devices` (from `v4l-utils`) tells you what the kernel sees;
  put a distinctive part of the name in `camera =` if more than one video
  device is attached (many webcams expose a second, metadata-only node).
* **Do not add `PrivateDevices=yes`** to the systemd unit — it hides
  `/dev/video*` from the service and the webcam will never be found.
* **Audio.** A system service has no PulseAudio/PipeWire user session. If the
  journal shows the microphone or speaker failing to attach and you do not need
  audio, set `microphone = none` and `speaker = none`; video is unaffected.
* **Encoding load.** The Pi 4 encodes in software here. If the far end reports
  a low frame rate, prefer a 720p-capable camera and keep the room layout
  modest.
