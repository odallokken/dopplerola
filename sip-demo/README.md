# doppler sip-demo

A tiny demo that places a **SIP** video call using the Pexip **Pulse** C API
as the media engine, with **PJSIP** doing the SIP signalling and **Dear ImGui**
as the application layer.

The companion to `src/main.cpp` (which talks to a Pexip Infinity conference
directly via Pulse's built-in REST client). Here Pulse never speaks to a
conferencing node itself — we use the two-stage manual setup path so we own
the SDP exchange, and we hand that SDP off to PJSIP which puts it on the
wire in an `INVITE`.

```
┌──────────────────────────────┐
│   ImGui (GLFW + OpenGL3)     │   <-- single field:
│                              │       "SIP URI to call"
│  havard@pexipdemo.com        │
│  [Call]  [Hang up]           │
│                              │
│  State: In call              │
└──────────────┬───────────────┘
               │
               │ 1. pulse_setup_stage_1_from_structure(is_sip=true)
               │    -> local SDP offer
               ▼
        ┌──────────────┐                   ┌────────────┐
        │ libpexpulse  │                   │   PJSIP    │
        │  (media)     │                   │ (signalling)
        └──────┬───────┘                   └─────┬──────┘
               │  2. offer SDP ───────────────▶  │
               │                                 │  INVITE → pexipdemo.com
               │                                 │  ◀── 200 OK (answer SDP)
               │  4. answer SDP + Call-ID ◀───── │
               │                                 │
               │ 5. pulse_setup_stage_2_from_structure
               ▼
        media flows
```

## What's in the box

| File                  | Purpose                                                 |
| --------------------- | ------------------------------------------------------- |
| `src/main.cpp`        | GLFW + ImGui + Pulse glue, almost identical to the REST demo. |
| `src/sip_ua.h/.cpp`   | Minimal PJSIP wrapper. TCP only, no REGISTER, one outbound call at a time. |
| `CMakeLists.txt`      | Build glue. Reuses the parent project's `pexip::pulse` imported target and Dear ImGui FetchContent. |

## Prerequisites

In addition to the [main repository's build deps](../README.md), you need
PJSIP. **PJSIP is *not* in the Ubuntu 24.04 archive**, so we ship a small
helper script that downloads, builds and installs it under `/usr/local`:

```bash
sudo ../scripts/install-pjsip.sh
```

(The script is idempotent. It installs `libpjproject.pc` into pkg-config's
search path, which is how CMake finds it.)

## Build

The demo is opt-in so the lean default `doppler` build keeps working without
any extra dependencies. From the **repository root**:

```bash
cmake -S . -B build -DBUILD_DOPPLER_SIP=ON
cmake --build build -j --target doppler-sip
./build/run-doppler-sip.sh
```

## Run

Type a SIP URI (`alice@example.com` or `sip:alice@example.com`) and press
**Call**. PJSIP resolves the host, sends `INVITE`, and once the 200 OK
arrives the answer SDP is handed back to Pulse via
`pulse_setup_stage_2_from_structure()`. From that point on media flows
through Pulse exactly as in the REST demo — same camera/mic/speaker
bindings, same auto-spawned video windows.

**Hang up** sends a SIP `BYE` and tears the Pulse session down.

## Notes / things to investigate

* `PulseSetupStage2Config::call_uuid` is set to the SIP **Call-ID** of the
  outgoing dialog. This is a placeholder choice — Pulse's REST path normally
  uses the Infinity-side call UUID. Revisit once we know what Pulse actually
  uses this field for in pure-SIP mode.
* Transport is TCP only and there's no SIP REGISTER — this is a "place a
  direct INVITE" demo, nothing more. TCP is the deliberate choice (Pexip
  Infinity prefers it, and a video INVITE easily exceeds PJSIP's UDP MTU
  threshold). Add TLS / REGISTER if/when needed.
* The user agent identifies as `doppler-sip/<version>` on both the SIP
  `User-Agent` header and Pulse's application user-agent string.
