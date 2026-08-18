// ============================================================================
//  headless - an unattended Pexip Pulse room client
// ----------------------------------------------------------------------------
//
//  Built for a Raspberry Pi 4 running Ubuntu Server 24.04 with a single USB
//  webcam and no display: the Pi boots, this client starts, dials a
//  pre-defined Virtual Meeting Room and sends the camera picture into it. No
//  keyboard, no mouse, no X server, no human.
//
//  The Pulse flow is exactly the one from `doppler`, minus the UI:
//
//      1.  pulse_new()                       -> create the Pulse instance.
//      2.  pulse_options_set_*_window_handle(NULL)
//                                            -> we have no display, so make
//                                               sure Pulse never tries to
//                                               spawn a native video window.
//      3.  pulse_device_session_connect_device()
//                                            -> bind the webcam to the MAIN
//                                               outgoing video.
//      4.  pulse_connect_with_rest_async()   -> join the meeting room.
//      5.  pulse_disconnect() / pulse_free() -> on SIGTERM.
//
//  Everything else in this file exists for one reason: **the camera must come
//  up on its own, every time, with nobody there to replug it.** A USB webcam
//  on a Pi is routinely slower to enumerate than the network stack, and it can
//  disappear and come back at any point (bus reset, brown-out, uvcvideo
//  hiccup). So the client never assumes the camera is there - it supervises it:
//
//    * At startup it *waits* for a video input device to appear instead of
//      failing (`camera_wait_secs`, default: wait forever).
//    * It subscribes to Pulse's device-list-changed callback, so a webcam
//      plugged in (or re-enumerated) later is picked up and attached without a
//      restart.
//    * It subscribes to the device-error callback, so a camera that dies mid
//      call is re-attached.
//    * It re-verifies the device session every few seconds
//      (pulse_device_session_is_connected_by_id) and re-attaches if the
//      session went away behind our back.
//    * It watches the *outgoing* video statistics (pulse_media_stats_get). If
//      no video packets have left the box for `video_stall_timeout_secs` the
//      camera is re-attached; if that does not help, the call is redialled.
//
//  The same "never give up" logic covers the call itself: connection attempts
//  are retried with exponential backoff, forever.
//
//  Audio is treated as optional in the same spirit: Pulse captures and plays it
//  through PipeWire, and a box with no PipeWire daemon (the normal state of
//  affairs for a system service) must still get its picture into the meeting.
//  See `pipewire_available()` below for why we look before we leap.
// ============================================================================

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// The single header that pulls in the full Pulse API surface.
#include <pexpulse/pulse.h>

#include "config.h"

using Clock      = std::chrono::steady_clock;
using TimePoint  = Clock::time_point;

namespace {

// Where we look for the config file when --config is not given.
constexpr const char * kDefaultConfigPath = "/etc/pulse-headless.conf";

// How often the supervisor loop wakes up. Small enough that SIGTERM is acted
// on promptly, large enough to be invisible on a Pi's CPU budget.
constexpr auto kTick = std::chrono::milliseconds(250);

// How often we re-verify that the camera device session is still alive.
constexpr auto kCameraVerifyInterval = std::chrono::seconds(5);

// How often we sample the outgoing media statistics for the stall watchdog.
constexpr auto kStatsInterval = std::chrono::seconds(5);

// How often we re-try attaching optional audio devices that were not there.
constexpr auto kAudioRetryInterval = std::chrono::seconds(30);

// How often we look at the config file's timestamp. Editing the file is how
// the operator moves the box to a different meeting room, so this wants to be
// responsive without stat()ing in a tight loop.
constexpr auto kConfigCheckInterval = std::chrono::seconds(2);

// ----------------------------------------------------------------------------
//  Logging
// ----------------------------------------------------------------------------
//
//  Everything goes to stdout/stderr; under systemd that lands in the journal
//  with no extra machinery. Pulse callbacks log from their own threads, hence
//  the mutex.
// ----------------------------------------------------------------------------

std::mutex g_log_mutex;

void log_line(const char * level, const char * fmt, ...)
    __attribute__((format(printf, 2, 3)));

void log_line(const char * level, const char * fmt, ...)
{
    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    std::time_t now = std::time(nullptr);
    std::tm      tm_now{};
    char         stamp[32] = "";
    if (localtime_r(&now, &tm_now) != nullptr)
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fprintf(stdout, "%s [%s] %s\n", stamp, level, message);
    std::fflush(stdout);
}

#define LOG_INFO(...)  log_line("info",  __VA_ARGS__)
#define LOG_WARN(...)  log_line("warn",  __VA_ARGS__)
#define LOG_ERROR(...) log_line("error", __VA_ARGS__)

// ----------------------------------------------------------------------------
//  Signals
// ----------------------------------------------------------------------------

volatile std::sig_atomic_t g_stop = 0;

extern "C" void on_signal(int /*signum*/)
{
    g_stop = 1;
}

// ----------------------------------------------------------------------------
//  Application state
// ----------------------------------------------------------------------------
//
//  Pulse callbacks run on Pulse's own threads, so anything they touch is
//  atomic. The supervisor loop in main() owns everything else.
// ----------------------------------------------------------------------------

struct App
{
    // The config file, and the settings read from it. `cfg` is re-read whenever
    // the file changes on disk, so the few Pulse callbacks that need a value
    // out of it take the mutex; the supervisor loop works from its own
    // snapshot. `verbose` is mirrored as an atomic because the log callback is
    // far too hot to take a lock.
    std::string       config_path;
    std::mutex        cfg_mutex;
    headless::Config  cfg;
    std::atomic<bool> verbose{false};

    Pulse * pulse = nullptr;

    // Written from Pulse callback threads, read by the supervisor loop.
    std::atomic<int>  conference_status{PULSE_CONNECTION_STATUS_DISCONNECTED};
    std::atomic<bool> connect_in_flight{false};
    std::atomic<bool> video_devices_changed{true};  // true => rescan on first tick
    std::atomic<bool> camera_error{false};

    // Camera bookkeeping. Written by the supervisor loop; `camera_attached` and
    // `camera_id` are also read by the device-error callback on a Pulse thread,
    // hence atomic.
    std::atomic<bool>          camera_attached{false};
    std::atomic<PulseDeviceID> camera_id{0};
    std::string                camera_name;  // supervisor thread only

    // Optional audio bookkeeping (supervisor thread only).
    bool microphone_attached = false;
    bool speaker_attached    = false;
};

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const char * status_to_string(PulseConnectionStatus status)
{
    switch (status) {
        case PULSE_CONNECTION_STATUS_DISCONNECTED:  return "disconnected";
        case PULSE_CONNECTION_STATUS_CONNECTING:    return "connecting";
        case PULSE_CONNECTION_STATUS_RECONNECTING:  return "reconnecting";
        case PULSE_CONNECTION_STATUS_CONNECTED:     return "connected";
        case PULSE_CONNECTION_STATUS_DISCONNECTING: return "disconnecting";
        default:                                    return "unknown";
    }
}

// ----------------------------------------------------------------------------
//  Pulse callbacks
// ----------------------------------------------------------------------------

void on_conference_status(const PulseConferenceStatusInfo * info, void * user_context)
{
    auto * app = static_cast<App *>(user_context);
    app->conference_status.store(static_cast<int>(info->status));
    LOG_INFO("conference status: %s", status_to_string(info->status));
}

void on_async_result(const PulseError err, void * user_context)
{
    auto * app = static_cast<App *>(user_context);
    app->connect_in_flight.store(false);
    if (err != PULSE_SUCCESS)
        LOG_WARN("async operation failed: %s", pulse_strerror(err));
}

void on_progress(const PulseOperationProgressInfo * info, void * user_context)
{
    auto * app = static_cast<App *>(user_context);
    if (app->verbose.load())
        LOG_INFO("[%3d%%] %s", static_cast<int>(info->progress * 100.0f),
                 info->desc ? info->desc : "");
}

// The node asks for a PIN when the meeting room is PIN protected. We answer
// straight from the config file - the PIN itself is never logged.
bool on_pin_code_request(bool guest_pin_required, const PulseSetPinCode * set_pin,
                         void * user_context)
{
    auto * app = static_cast<App *>(user_context);
    if (set_pin == nullptr || set_pin->func == nullptr)
        return false;

    std::string pin;
    {
        std::lock_guard<std::mutex> lock(app->cfg_mutex);
        pin = app->cfg.pin;
    }

    if (guest_pin_required && pin.empty()) {
        LOG_ERROR("the meeting room requires a PIN but none is set in the config "
                  "file (add 'pin = <code>')");
        return false;
    }

    LOG_INFO("supplying %s from the config file",
             pin.empty() ? "an empty PIN (host/none required)" : "the configured PIN");
    set_pin->func(set_pin->context, pin.empty() ? nullptr : pin.c_str());
    return true;
}

// Fires whenever Pulse notices the set of devices changed - i.e. exactly when
// our USB webcam finally enumerates, or when it is unplugged and plugged back
// in. All we do is raise a flag; the supervisor loop does the real work.
void on_device_list_changed(PulseMediaType media_type, void * user_context)
{
    auto * app = static_cast<App *>(user_context);
    if (media_type != PULSE_MEDIA_VIDEO)
        return;
    app->video_devices_changed.store(true);
    LOG_INFO("video device list changed");
}

// Fires when a device Pulse is using reports an error (camera yanked out,
// driver hiccup, ...). If it is our camera, force a re-attach.
void on_device_error(void * user_context, PulseDeviceID device_id, PulseError device_error)
{
    auto * app = static_cast<App *>(user_context);
    LOG_WARN("device %u reported an error: %s", device_id, pulse_strerror(device_error));
    if (app->camera_attached.load() && device_id == app->camera_id.load())
        app->camera_error.store(true);
}

void on_pulse_log(void * user_context, PulseDebugLevel level, const char * category,
                  int64_t /*wall_time_us*/, int64_t /*elapsed_nano*/, unsigned int /*pid*/,
                  const char * /*file*/, const char * /*function*/, int /*line*/,
                  const char * /*object_debug_str*/, const char * message)
{
    const auto * app = static_cast<const App *>(user_context);
    const PulseDebugLevel threshold =
        (app != nullptr && app->verbose.load()) ? PULSE_LEVEL_INFO : PULSE_LEVEL_WARNING;
    if (level > threshold)
        return;

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fprintf(stderr, "[pulse:%s] %s\n", category ? category : "?",
                 message ? message : "");
}

// ----------------------------------------------------------------------------
//  Device helpers
// ----------------------------------------------------------------------------

// Picks a device out of Pulse's enumeration. `want` is either "auto" (system
// default, falling back to the first device) or a case-insensitive substring of
// the device name. Returns an owned copy (pulse_device_free) or nullptr.
PulseDevice * find_device(Pulse * pulse, PulseMediaType type, PulseMediaDirection direction,
                          const std::string & want)
{
    PulseDeviceIterator * it = nullptr;
    if (pulse_device_iterator_new(pulse, type, direction, &it) != PULSE_SUCCESS || it == nullptr)
        return nullptr;

    const bool        automatic = (to_lower(want) == "auto");
    const std::string needle    = to_lower(want);

    PulseDevice *       chosen         = nullptr;
    const PulseDevice * first          = nullptr;
    const PulseDevice * system_default = nullptr;

    for (const PulseDevice * d = pulse_device_iterator_first(it); d != nullptr;
         d = pulse_device_iterator_next(it)) {
        if (first == nullptr)
            first = d;
        if (system_default == nullptr && pulse_device_is_system_default(d))
            system_default = d;

        if (!automatic) {
            const char * name = pulse_device_get_name(d);
            if (name != nullptr && to_lower(name).find(needle) != std::string::npos) {
                chosen = pulse_device_copy(d);
                break;
            }
        }
    }

    if (chosen == nullptr && automatic) {
        const PulseDevice * pick = (system_default != nullptr) ? system_default : first;
        if (pick != nullptr)
            chosen = pulse_device_copy(pick);
    }

    pulse_device_iterator_free(it);
    return chosen;
}

// Attaches the webcam to the MAIN outgoing video. Any previous video input
// session is torn down first: after a USB re-enumeration the old session is
// stale, and connecting on top of it is what leaves you in the "camera is
// there but no picture ever arrives" state.
bool attach_camera(App & app, const std::string & want)
{
    PulseDevice * camera = find_device(app.pulse, PULSE_MEDIA_VIDEO, PULSE_MEDIA_INPUT, want);
    if (camera == nullptr)
        return false;

    const char * name = pulse_device_get_name(camera);
    const PulseDeviceID id = pulse_device_get_id(camera);

    pulse_device_session_disconnect_main_video(app.pulse, PULSE_MEDIA_CONTENT_MAIN,
                                              PULSE_MEDIA_INPUT);

    const PulseError err =
        pulse_device_session_connect_device(app.pulse, camera, PULSE_MEDIA_CONTENT_MAIN);
    if (err != PULSE_SUCCESS) {
        LOG_WARN("failed to attach camera '%s': %s", name ? name : "?", pulse_strerror(err));
        pulse_device_free(camera);
        return false;
    }

    app.camera_name = (name != nullptr) ? name : "";
    app.camera_id.store(id);
    app.camera_attached.store(true);
    app.camera_error.store(false);
    LOG_INFO("camera attached: '%s' (id %u)", app.camera_name.c_str(), id);

    pulse_device_free(camera);
    return true;
}

// ----------------------------------------------------------------------------
//  Is there an audio server to talk to?
// ----------------------------------------------------------------------------
//
//  Pulse's audio devices are PipeWire clients. When no PipeWire daemon is
//  reachable the media engine logs "Failed to connect to PipeWire" and then
//  builds its audio element on top of the connection it just failed to make,
//  which segfaults inside libpipewire (pw_stream_new on a NULL core) and takes
//  the whole client down with it. That is exactly the situation of a systemd
//  service: no login session, so no XDG_RUNTIME_DIR and no user PipeWire.
//
//  Video does not go anywhere near PipeWire, so the sane behaviour is to send
//  the picture and skip the audio - but only Pulse's *caller* can decide that,
//  so we check the socket ourselves before asking for any audio device.
//
//  Resolution follows libpipewire's own rules: the runtime directory comes from
//  PIPEWIRE_RUNTIME_DIR, else XDG_RUNTIME_DIR, else USERPROFILE, and the socket
//  in it is named by PIPEWIRE_REMOTE (default "pipewire-0").
// ----------------------------------------------------------------------------

std::string pipewire_socket_path()
{
    auto from_env = [](const char * name) -> const char * {
        const char * value = std::getenv(name);
        return (value != nullptr && *value != '\0') ? value : nullptr;
    };

    const char * remote = from_env("PIPEWIRE_REMOTE");
    if (remote == nullptr)
        remote = "pipewire-0";
    if (remote[0] == '/')
        return remote;  // an absolute path bypasses the runtime directory

    const char * runtime_dir = from_env("PIPEWIRE_RUNTIME_DIR");
    if (runtime_dir == nullptr)
        runtime_dir = from_env("XDG_RUNTIME_DIR");
    if (runtime_dir == nullptr)
        runtime_dir = from_env("USERPROFILE");
    if (runtime_dir == nullptr)
        return {};

    return std::string(runtime_dir) + "/" + remote;
}

// A connect() on the socket rather than a stat(): a leftover socket file from a
// daemon that is no longer running would crash us just as thoroughly.
bool pipewire_available(std::string & where)
{
    where = pipewire_socket_path();
    if (where.empty())
        return false;

    sockaddr_un addr{};
    if (where.size() >= sizeof(addr.sun_path))
        return false;

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;

    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, where.c_str(), where.size() + 1);

    const bool connected =
        ::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return connected;
}

// Best-effort audio. A webcam-only Pi may well have a microphone (most webcams
// do) and no speaker at all, so neither is allowed to hold up the call.
bool attach_audio_device(App & app, PulseMediaDirection direction, const std::string & want,
                         const char * what)
{
    if (to_lower(want) == "none")
        return true;  // deliberately disabled - nothing to retry

    if (to_lower(want) == "auto") {
        const PulseError err = pulse_device_session_connect_system_default(
            app.pulse, PULSE_MEDIA_CONTENT_MAIN, PULSE_MEDIA_AUDIO, direction);
        if (err != PULSE_SUCCESS) {
            LOG_WARN("no default %s available (%s) - continuing without it", what,
                     pulse_strerror(err));
            return false;
        }
        LOG_INFO("%s attached (system default)", what);
        return true;
    }

    PulseDevice * device = find_device(app.pulse, PULSE_MEDIA_AUDIO, direction, want);
    if (device == nullptr) {
        LOG_WARN("no %s matching '%s' - continuing without it", what, want.c_str());
        return false;
    }

    const PulseError err =
        pulse_device_session_connect_device(app.pulse, device, PULSE_MEDIA_CONTENT_MAIN);
    if (err != PULSE_SUCCESS)
        LOG_WARN("failed to attach %s '%s': %s", what, pulse_device_get_name(device),
                 pulse_strerror(err));
    else
        LOG_INFO("%s attached: '%s'", what, pulse_device_get_name(device));

    const bool ok = (err == PULSE_SUCCESS);
    pulse_device_free(device);
    return ok;
}

// ----------------------------------------------------------------------------
//  Conference
// ----------------------------------------------------------------------------

bool start_connect(App & app, const headless::Config & cfg)
{
    PulseRestConnectionConfig conn{};
    conn.server_address  = cfg.host.c_str();
    conn.conference_name = cfg.conference.c_str();
    conn.display_name    = cfg.display_name.c_str();
    conn.pin_code        = cfg.pin.empty() ? nullptr : cfg.pin.c_str();

    PulseAsyncOperationResultCallbackConfig result_cb{on_async_result, &app};
    PulseOperationProgressCallbackConfig    progress_cb{on_progress, &app};

    LOG_INFO("dialling %s via %s as '%s'", cfg.conference.c_str(), cfg.host.c_str(),
             cfg.display_name.c_str());

    app.connect_in_flight.store(true);
    const PulseError err =
        pulse_connect_with_rest_async(app.pulse, &conn, &result_cb, &progress_cb);
    if (err != PULSE_SUCCESS) {
        app.connect_in_flight.store(false);
        LOG_WARN("pulse_connect_with_rest_async failed: %s", pulse_strerror(err));
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
//  Supervisor
// ----------------------------------------------------------------------------
//
//  One loop, four jobs: keep a camera attached, keep the call up, notice when
//  video silently stops flowing, and pick up edits to the config file. Each job
//  has its own "not before" time point so a failing one backs off without
//  stalling the others.
//
//  The supervisor owns a *snapshot* of the config (`cfg_`). The shared copy in
//  App is only touched here, under its mutex, when the file on disk changes.
// ----------------------------------------------------------------------------

class Supervisor
{
public:
    explicit Supervisor(App & app)
        : app_(app)
        , cfg_(app.cfg)
        , started_(Clock::now())
        , camera_backoff_(app.cfg.retry_delay_secs)
        , connect_backoff_(app.cfg.retry_delay_secs)
    {
        remember_config_stamp();
    }

    void tick()
    {
        const TimePoint now = Clock::now();
        reload_config_if_changed(now);
        ensure_camera(now);
        ensure_audio(now);
        ensure_call(now);
        watch_outgoing_video(now);
    }

private:
    int bump(int & backoff) const
    {
        const int current = backoff;
        backoff = std::min(backoff * 2, cfg_.max_retry_delay_secs);
        return current;
    }

    // Note the config file's mtime + size so we can spot an edit later.
    void remember_config_stamp()
    {
        struct stat st{};
        if (::stat(app_.config_path.c_str(), &st) == 0) {
            config_mtime_ = st.st_mtime;
            config_size_  = st.st_size;
        }
    }

    // The operator changes meeting rooms by editing the config file, so watch
    // it and act on the change: no restart, no service commands. A file that is
    // mid-edit (or momentarily missing while an editor renames it into place)
    // simply fails to parse, is logged, and the previous settings stay live.
    void reload_config_if_changed(const TimePoint & now)
    {
        if (now < next_config_check_)
            return;
        next_config_check_ = now + kConfigCheckInterval;

        struct stat st{};
        if (::stat(app_.config_path.c_str(), &st) != 0)
            return;
        if (st.st_mtime == config_mtime_ && st.st_size == config_size_)
            return;

        // Record the new stamp either way, so a broken file is complained
        // about once rather than every two seconds.
        config_mtime_ = st.st_mtime;
        config_size_  = st.st_size;

        headless::Config fresh;
        std::string      error;
        if (!headless::load_config(app_.config_path, fresh, error)) {
            LOG_ERROR("config reload failed: %s (keeping the previous settings)",
                      error.c_str());
            return;
        }

        const bool room_changed   = fresh.host != cfg_.host
                                    || fresh.conference != cfg_.conference
                                    || fresh.pin != cfg_.pin
                                    || fresh.display_name != cfg_.display_name;
        const bool camera_changed = fresh.camera != cfg_.camera;
        const bool audio_changed  = fresh.microphone != cfg_.microphone
                                    || fresh.speaker != cfg_.speaker;

        {
            std::lock_guard<std::mutex> lock(app_.cfg_mutex);
            app_.cfg = fresh;
        }
        app_.verbose.store(fresh.verbose);
        cfg_ = fresh;
        pulse_options_set_verbose_logging(app_.pulse, fresh.verbose);

        LOG_INFO("config file changed - reloaded");

        if (camera_changed) {
            LOG_INFO("camera selection changed to '%s' - re-attaching", cfg_.camera.c_str());
            // Tear the old session down here as well: switching to "none" must
            // actually stop the picture, not just stop supervising it.
            pulse_device_session_disconnect_main_video(app_.pulse, PULSE_MEDIA_CONTENT_MAIN,
                                                      PULSE_MEDIA_INPUT);
            app_.camera_attached.store(false);
            camera_disabled_logged_ = false;
            camera_backoff_      = cfg_.retry_delay_secs;
            next_camera_attempt_ = now;
        }

        if (audio_changed) {
            // Rebuild both directions from scratch: the audio session API only
            // offers an all-or-nothing disconnect for the MAIN content.
            //
            // Every audio call into Pulse builds a PipeWire element - the
            // *disconnect* included - so it is only safe with a daemon behind
            // it. Without one nothing was ever attached, so skipping it costs
            // nothing and saves the segfault described at pipewire_available().
            std::string socket_path;
            if (pipewire_available(socket_path))
                pulse_device_session_disconnect_main_audio(app_.pulse);
            app_.microphone_attached = false;
            app_.speaker_attached    = false;
            audio_unavailable_logged_ = false;
            next_audio_attempt_      = now;
        }

        if (room_changed) {
            LOG_INFO("meeting room changed - moving to %s via %s", cfg_.conference.c_str(),
                     cfg_.host.c_str());
            connect_backoff_      = cfg_.retry_delay_secs;
            next_connect_attempt_ = now;
            leave_call();
        }
    }

    // Drop the current call (if any) so ensure_call() redials with the new
    // settings. Harmless when we are already disconnected.
    void leave_call()
    {
        const auto status =
            static_cast<PulseConnectionStatus>(app_.conference_status.load());
        if (status == PULSE_CONNECTION_STATUS_DISCONNECTED
            || status == PULSE_CONNECTION_STATUS_DISCONNECTING)
            return;

        PulseAsyncOperationResultCallbackConfig result_cb{on_async_result, &app_};
        app_.connect_in_flight.store(true);
        if (pulse_disconnect_async(app_.pulse, &result_cb, nullptr) != PULSE_SUCCESS)
            app_.connect_in_flight.store(false);
    }

    bool camera_disabled() const { return to_lower(cfg_.camera) == "none"; }

    // True once we have waited longer than `camera_wait_secs` for a camera
    // that never showed up. From then on we join the meeting regardless (being
    // in the room without a picture beats not being there at all) while the
    // camera keeps being retried in the background.
    bool camera_wait_expired(const TimePoint & now) const
    {
        if (cfg_.camera_wait_secs <= 0)
            return false;
        return now - started_ >= std::chrono::seconds(cfg_.camera_wait_secs);
    }

    void ensure_camera(const TimePoint & now)
    {
        // "camera = none" is a deliberate choice (audio-only endpoint), so
        // stop supervising: no waiting, no retries, and the call is free to
        // go ahead without a picture.
        if (camera_disabled()) {
            app_.camera_error.exchange(false);
            app_.video_devices_changed.exchange(false);
            app_.camera_attached.store(false);
            if (!camera_disabled_logged_) {
                camera_disabled_logged_ = true;
                LOG_WARN("camera is set to 'none' - joining without sending video");
            }
            return;
        }

        // A device-list change or a device error invalidates whatever we think
        // we know about the camera.
        if (app_.camera_error.exchange(false) && app_.camera_attached.load()) {
            LOG_WARN("camera '%s' reported an error - re-attaching",
                     app_.camera_name.c_str());
            app_.camera_attached.store(false);
            next_camera_attempt_ = now;
        }

        if (app_.video_devices_changed.exchange(false)) {
            // Re-attach even if we believe we are attached: the same webcam
            // coming back after a bus reset gets a fresh device session.
            if (app_.camera_attached.load())
                LOG_INFO("re-attaching camera after a device list change");
            app_.camera_attached.store(false);
            next_camera_attempt_ = now;
        }

        // Cheap periodic health check of the session we think we own.
        if (app_.camera_attached.load() && now >= next_camera_verify_) {
            next_camera_verify_ = now + kCameraVerifyInterval;
            bool is_connected   = false;
            const PulseError err = pulse_device_session_is_connected_by_id(
                app_.pulse, app_.camera_id.load(), PULSE_MEDIA_VIDEO, PULSE_MEDIA_INPUT,
                PULSE_MEDIA_CONTENT_MAIN, &is_connected);
            if (err == PULSE_SUCCESS && !is_connected) {
                LOG_WARN("camera session for '%s' went away - re-attaching",
                         app_.camera_name.c_str());
                app_.camera_attached.store(false);
                next_camera_attempt_ = now;
            }
        }

        if (app_.camera_attached.load() || now < next_camera_attempt_)
            return;

        if (attach_camera(app_, cfg_.camera)) {
            camera_backoff_      = cfg_.retry_delay_secs;
            next_camera_verify_  = now + kCameraVerifyInterval;
            reset_stall_watchdog(now);
            return;
        }

        // No camera (yet). Say so at a sane rate, then back off and retry.
        if (now >= next_camera_complaint_) {
            next_camera_complaint_ = now + std::chrono::seconds(15);
            LOG_WARN("no camera matching '%s' available yet - waiting",
                     cfg_.camera.c_str());
        }
        next_camera_attempt_ = now + std::chrono::seconds(bump(camera_backoff_));
    }

    void ensure_audio(const TimePoint & now)
    {
        if (now < next_audio_attempt_)
            return;
        next_audio_attempt_ = now + kAudioRetryInterval;

        const bool want_microphone = to_lower(cfg_.microphone) != "none";
        const bool want_speaker    = to_lower(cfg_.speaker) != "none";
        if (!want_microphone && !want_speaker)
            return;

        // Never ask Pulse for an audio device without a PipeWire daemon behind
        // it - that is a segfault, not an error code. Re-checked on every retry
        // so audio is picked up if a daemon turns up later.
        std::string socket_path;
        if (!pipewire_available(socket_path)) {
            if (!audio_unavailable_logged_) {
                audio_unavailable_logged_ = true;
                LOG_WARN("no PipeWire audio server at '%s' - continuing without audio "
                         "(video is unaffected); see the demo README, or set "
                         "'microphone = none' and 'speaker = none' to stop looking",
                         socket_path.empty() ? "<no runtime directory>" : socket_path.c_str());
            }
            return;
        }

        if (audio_unavailable_logged_) {
            audio_unavailable_logged_ = false;
            LOG_INFO("PipeWire is available at '%s' - attaching audio", socket_path.c_str());
        }

        if (!app_.microphone_attached)
            app_.microphone_attached =
                attach_audio_device(app_, PULSE_MEDIA_INPUT, cfg_.microphone, "microphone");
        if (!app_.speaker_attached)
            app_.speaker_attached =
                attach_audio_device(app_, PULSE_MEDIA_OUTPUT, cfg_.speaker, "speaker");
    }

    void ensure_call(const TimePoint & now)
    {
        const auto status =
            static_cast<PulseConnectionStatus>(app_.conference_status.load());

        if (status == PULSE_CONNECTION_STATUS_CONNECTED) {
            connect_backoff_ = cfg_.retry_delay_secs;
            return;
        }

        // Pulse handles its own reconnection while it is CONNECTING /
        // RECONNECTING / DISCONNECTING - only redial from a standstill.
        if (status != PULSE_CONNECTION_STATUS_DISCONNECTED || app_.connect_in_flight.load())
            return;

        // Don't join before the camera is up, unless the operator configured a
        // deadline and it has passed (or asked for no camera at all).
        if (!camera_disabled() && !app_.camera_attached.load() && !camera_wait_expired(now))
            return;

        if (now < next_connect_attempt_)
            return;

        if (!camera_disabled() && !app_.camera_attached.load())
            LOG_WARN("joining without a camera - still looking for one");

        start_connect(app_, cfg_);
        next_connect_attempt_ = now + std::chrono::seconds(bump(connect_backoff_));
    }

    void reset_stall_watchdog(const TimePoint & now)
    {
        last_video_progress_ = now;
        stall_escalation_    = 0;
    }

    // Are we actually *sending* video? Everything above this point can look
    // perfectly healthy while the far end sees a black square, so this is the
    // check that ultimately matters.
    void watch_outgoing_video(const TimePoint & now)
    {
        if (cfg_.video_stall_timeout_secs <= 0)
            return;

        const auto status =
            static_cast<PulseConnectionStatus>(app_.conference_status.load());
        if (status != PULSE_CONNECTION_STATUS_CONNECTED || !app_.camera_attached.load()) {
            reset_stall_watchdog(now);
            return;
        }

        if (now < next_stats_sample_)
            return;
        next_stats_sample_ = now + kStatsInterval;

        PulseMediaStats * stats = pulse_media_stats_get(app_.pulse, 10, false);
        if (stats == nullptr)
            return;
        const uint64_t sent = stats->video_tx.total_packets_sent;
        pulse_media_stats_free(stats);

        if (sent > last_video_packets_) {
            last_video_packets_ = sent;
            reset_stall_watchdog(now);
            return;
        }

        if (now - last_video_progress_ < std::chrono::seconds(cfg_.video_stall_timeout_secs))
            return;

        if (stall_escalation_ == 0) {
            LOG_WARN("no outgoing video for %ds - re-attaching the camera",
                     cfg_.video_stall_timeout_secs);
            app_.camera_attached.store(false);
            next_camera_attempt_ = now;
            last_video_progress_ = now;
            stall_escalation_    = 1;
        } else {
            LOG_WARN("outgoing video still dead - redialling");
            last_video_progress_ = now;
            stall_escalation_    = 0;
            leave_call();
        }
    }

    App &            app_;
    headless::Config cfg_;      // snapshot of app_.cfg, refreshed on reload
    TimePoint        started_;
    int              camera_backoff_;
    int              connect_backoff_;
    bool             camera_disabled_logged_   = false;
    bool             audio_unavailable_logged_ = false;

    time_t    config_mtime_ = 0;
    off_t     config_size_  = 0;

    TimePoint next_config_check_{};
    TimePoint next_camera_attempt_{};
    TimePoint next_camera_verify_{};
    TimePoint next_camera_complaint_{};
    TimePoint next_audio_attempt_{};
    TimePoint next_connect_attempt_{};
    TimePoint next_stats_sample_{};

    TimePoint last_video_progress_{Clock::now()};
    uint64_t  last_video_packets_ = 0;
    int       stall_escalation_   = 0;
};

// ----------------------------------------------------------------------------
//  Wiring
// ----------------------------------------------------------------------------

void install_callbacks(App & app)
{
    PulseConferenceStatusCallbackConfig conf_cb{on_conference_status, &app};
    pulse_options_set_conference_state_callback(app.pulse, &conf_cb);

    PulsePinCodeRequestCallbackConfig pin_cb{on_pin_code_request, &app};
    pulse_options_set_pin_code_request_callbacks(app.pulse, &pin_cb);

    // The two callbacks that make unattended camera handling possible.
    pulse_register_device_list_changed_callback(app.pulse, PULSE_MEDIA_VIDEO,
                                                on_device_list_changed, &app);
    pulse_register_device_error_callback(app.pulse, on_device_error, &app);

    pulse_options_set_application_user_agent_string(app.pulse, "pulse-headless/1.0");
    pulse_options_set_verbose_logging(app.pulse, app.cfg.verbose);

    // No display: make sure Pulse never opens a video window of its own.
    pulse_options_set_self_view_window_handle(app.pulse, nullptr);
    pulse_options_set_remote_video_window_handle(app.pulse, nullptr);
    pulse_options_set_presentation_video_window_handle(app.pulse, nullptr);
    pulse_options_set_preflight_video_window_handle(app.pulse, nullptr);
}

// Must run before pulse_free(): an in-flight callback firing into a destroyed
// App would take the process down with it.
void uninstall_callbacks(App & app)
{
    pulse_options_set_conference_state_callback(app.pulse, nullptr);
    pulse_options_set_pin_code_request_callbacks(app.pulse, nullptr);
    pulse_deregister_device_list_changed_callback(app.pulse, PULSE_MEDIA_VIDEO);
    pulse_deregister_device_error_callback(app.pulse);
}

void print_usage(const char * argv0)
{
    std::fprintf(stderr,
                 "Usage: %s [--config <file>]\n"
                 "\n"
                 "  -c, --config <file>   Config file to read (default: %s)\n"
                 "  -h, --help            Show this help\n",
                 argv0, kDefaultConfigPath);
}

} // namespace

int main(int argc, char ** argv)
{
    std::string config_path = kDefaultConfigPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unrecognised argument: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 2;
        }
    }

    App         app;
    std::string error;
    app.config_path = config_path;
    if (!headless::load_config(config_path, app.cfg, error)) {
        std::fprintf(stderr, "Config error: %s\n", error.c_str());
        return 1;
    }
    app.verbose.store(app.cfg.verbose);

    LOG_INFO("pulse-headless starting (config: %s)", config_path.c_str());
    LOG_INFO("meeting room: %s via %s%s", app.cfg.conference.c_str(), app.cfg.host.c_str(),
             app.cfg.pin.empty() ? "" : " (PIN configured)");
    LOG_INFO("edit %s at any time - the change is picked up without a restart",
             config_path.c_str());

    // Terminate cleanly so systemd's stop/restart leaves the conference
    // properly rather than waiting for the server-side timeout.
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Must be installed before the first pulse_new(), or the early start-up log
    // lines are lost.
    pulse_global_logger_callback(on_pulse_log, &app);

    app.pulse = pulse_new();
    if (app.pulse == nullptr) {
        std::fprintf(stderr, "pulse_new() returned NULL\n");
        return 1;
    }

    install_callbacks(app);

    Supervisor supervisor(app);
    while (g_stop == 0) {
        supervisor.tick();
        std::this_thread::sleep_for(kTick);
    }

    LOG_INFO("shutting down");
    uninstall_callbacks(app);
    if (pulse_is_connected(app.pulse))
        pulse_disconnect(app.pulse, nullptr);
    pulse_free(app.pulse);
    LOG_INFO("stopped");
    return 0;
}
