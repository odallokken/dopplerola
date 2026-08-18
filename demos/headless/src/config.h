// ============================================================================
//  headless - configuration file
// ----------------------------------------------------------------------------
//
//  A tiny "key = value" config reader. The client is unattended, so every
//  knob it has lives in a file on disk; nothing is ever asked interactively.
//
//      # /etc/pulse-headless.conf
//      host       = pexpo.net          # the Pexip Infinity node to call
//      conference = ola@pexpo.net      # the virtual meeting room URI
//      pin        = 1234               # optional PIN for the meeting room
//
//  Blank lines and everything after a '#' are ignored, keys are
//  case-insensitive and surrounding whitespace is trimmed.
// ============================================================================

#pragma once

#include <string>

namespace headless {

// Everything the client needs to know. Defaults are chosen for the target
// deployment: a headless Raspberry Pi with a single USB webcam attached.
struct Config
{
    // ---- Required -------------------------------------------------------
    std::string host;              // "pexpo.net" - the Conferencing Node to call
    std::string conference;        // "ola@pexpo.net" - the virtual meeting room URI

    // ---- Optional -------------------------------------------------------
    std::string pin;               // meeting-room PIN, empty = none
    std::string display_name = "Raspberry Pi";

    // Device selection. "auto" picks the system default (falling back to the
    // first device Pulse enumerates); "none" disables that direction; anything
    // else is matched case-insensitively as a substring of the device name, so
    // "c920" happily matches "HD Pro Webcam C920".
    std::string camera     = "auto";
    std::string microphone = "auto";
    std::string speaker    = "none";   // a headless Pi rarely has one

    // ---- Timing ---------------------------------------------------------
    // How long to wait for the webcam to show up before joining anyway. USB
    // enumeration and the uvcvideo module can easily lag the network stack on
    // a cold boot, so the default is to wait indefinitely rather than to join
    // the meeting with no picture.
    int camera_wait_secs = 0;          // 0 = wait indefinitely

    // Retry/backoff for both the camera and the conference connection.
    int retry_delay_secs     = 5;      // first retry after this many seconds
    int max_retry_delay_secs = 60;     // ...doubling up to this ceiling

    // Watchdog: if the outgoing video stops flowing for this long while we are
    // connected, re-attach the camera; if it is still dead after a second
    // interval, drop the call and redial. 0 disables the watchdog.
    int video_stall_timeout_secs = 30;

    // ---- Misc -----------------------------------------------------------
    bool verbose = false;              // forward Pulse's info logs too
};

// Reads `path` into `cfg`. Returns false and fills `error` when the file
// cannot be read, when a required key (host/conference) is missing, or when a
// value is malformed. Unknown keys are warned about on stderr but are not
// fatal, so a config written for a newer build still starts.
bool load_config(const std::string & path, Config & cfg, std::string & error);

} // namespace headless
