// ============================================================================
//  doppler-sip - tiny Pexip Pulse + PJSIP video-call demo
// ----------------------------------------------------------------------------
//
//  The companion to src/main.cpp from the parent project. That one drives
//  Pulse's *built-in* REST + signalling path (pulse_connect_with_rest_async).
//  This one instead uses Pulse purely as a media engine:
//
//      1. pulse_new_external_rest() + the usual options/callback registration.
//         (external-rest mode tells Pulse "the application owns signalling",
//         which is exactly what we want when PJSIP is driving SIP.)
//      2. pulse_setup_stage_1_from_structure(is_sip=true)
//                          -> Pulse hands us a local SDP offer.
//      3. We give that offer to PJSIP, which sends a SIP INVITE.
//      4. PJSIP receives 200 OK; we feed the remote SDP + the dialog's
//         Call-ID back into pulse_setup_stage_2_from_structure().
//      5. Media flows through Pulse exactly as in the REST demo.
//      6. Hang-up -> SIP BYE via PJSIP, then pulse_disconnect_async().
//      7. pulse_free().
//
//  Everything else is just Dear ImGui plumbing, copied from src/main.cpp.
// ============================================================================

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <pexpulse/pulse.h>
#include <pexpulse/pulse_data_session.h>

#include "app_transport.h"
#include "sip_ua.h"

// ----------------------------------------------------------------------------
//  Application state
// ----------------------------------------------------------------------------
//
//  Same general shape as the REST demo: a single struct shared between the
//  UI thread, Pulse's internal worker threads, and PJSIP's worker thread.
//  Atomics for the polled flags, a mutex for the free-form status strings.
// ----------------------------------------------------------------------------

enum class CallStage {
    Idle = 0,
    Stage1Done,    // Pulse gave us an offer, INVITE sent, waiting for 200 OK.
    Stage2Done,    // SIP answer in, Pulse media engaged.
};

// ---------------------------------------------------------------------------
// STAGED BRING-UP: AppTransport bridge (startup-time decision)
// ---------------------------------------------------------------------------
// The architecture is: Pulse hands its RTP/RTCP packets to the
// `AppTransport` via the pulse_options_set_app_transport() callback API,
// and we relay them over UDP sockets we own ourselves (one per m=/RTCP
// wire). The same sockets receive the peer's RTP/RTCP and feed it back
// into Pulse via pulse_app_transport_push().
//
// IMPORTANT - this CANNOT be a runtime toggle. pulse_options_set_app_transport()
// is rejected with PULSE_ERROR_ALREADY_CONNECTED the moment the client
// enters the connected state, and Pulse enters that state implicitly the
// first time we call ANY *_connect_* API (pulse_device_session_connect_*,
// pulse_data_session_connect_output, pulse_setup_stage_1_*). All of those
// run during normal setup, long before the user could press a UI button.
//
// So the bind happens unconditionally at startup, right after
// pulse_new_external_rest(), before install_callbacks / window-handle
// setters / connect_default_devices / init_video_render_ctx. We respect
// an opt-out via `DOPPLER_SIP_BRIDGE=0` for A/B comparison with Pulse-
// owned sockets, but flipping it requires restarting the binary.
//
// The UI surfaces the latched state as a read-only checkbox + label so
// the operator can see at a glance which transport mode this build is
// using and whether the bind actually succeeded.

struct AppState
{
    Pulse *                  pulse     = nullptr;
    doppler::SipUA *         sip       = nullptr;
    // Non-owning pointer to the AppTransport instance. Populated by main()
    // immediately after `bind_to_pulse()` succeeds at startup, or left as
    // nullptr if DOPPLER_SIP_BRIDGE=0 or the bind itself failed. Every
    // `if (app.transport)` site below short-circuits in that case,
    // routing the call through Pulse-owned sockets unmodified.
    doppler::AppTransport *  transport = nullptr;

    // Bridge state, latched at startup. `bridge_enabled` reflects the
    // DOPPLER_SIP_BRIDGE env var (default = true). `bridge_bound` is set
    // iff pulse_options_set_app_transport() returned success. Neither
    // value changes after startup; the UI checkbox is read-only.
    // Atomics keep the UI-thread reads tidy against the startup writes.
    std::atomic<bool>        bridge_enabled{false};
    std::atomic<bool>        bridge_bound{false};
    // Storage for the AppTransport instance lives in main(); we keep a
    // non-owning pointer here so call-handling code can reach it (e.g.
    // configure_local_offer / configure_remote_answer).
    doppler::AppTransport *  transport_storage = nullptr;

    // The single input the demo needs: a SIP URI to call.
    char sip_uri[256]      = "";          // e.g. "havard@pexipdemo.com"
    char display_name[128] = "Doppler SIP demo";

    std::atomic<int>  connection_status{PULSE_CONNECTION_STATUS_DISCONNECTED};
    std::atomic<int>  last_async_error{PULSE_SUCCESS};
    std::atomic<int>  stage{static_cast<int>(CallStage::Idle)};

    std::mutex   text_mutex;
    std::string  status_text = "Idle. Enter a SIP URI and press Call.";
    std::string  progress_text;
};

static void set_status(AppState & app, std::string text)
{
    std::lock_guard<std::mutex> lock(app.text_mutex);
    app.status_text = std::move(text);
}
static void set_progress(AppState & app, std::string text)
{
    std::lock_guard<std::mutex> lock(app.text_mutex);
    app.progress_text = std::move(text);
}

static const char * status_to_string(int status)
{
    switch (status) {
        case PULSE_CONNECTION_STATUS_DISCONNECTED:  return "Disconnected";
        case PULSE_CONNECTION_STATUS_CONNECTING:    return "Connecting...";
        case PULSE_CONNECTION_STATUS_RECONNECTING:  return "Reconnecting...";
        case PULSE_CONNECTION_STATUS_CONNECTED:     return "Connected";
        case PULSE_CONNECTION_STATUS_DISCONNECTING: return "Disconnecting...";
        default:                                    return "Unknown";
    }
}

// ----------------------------------------------------------------------------
//  Pulse callbacks (identical to src/main.cpp - kept verbatim on purpose so
//  it's obvious the only thing that changes between the two demos is the
//  connect/setup path).
// ----------------------------------------------------------------------------

static void on_conference_status(const PulseConferenceStatusInfo * info, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    app->connection_status.store(static_cast<int>(info->status));
    set_status(*app, std::string("Conference status: ") + status_to_string(info->status));
}

static void on_async_result(const PulseError err, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    app->last_async_error.store(static_cast<int>(err));
    if (err == PULSE_SUCCESS) {
        set_status(*app, "Async operation completed successfully.");
    } else {
        set_status(*app, std::string("Async operation failed: ") + pulse_strerror(err));
    }
    set_progress(*app, "");
}

static void on_progress(const PulseOperationProgressInfo * info, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[%3d%%] %s",
                  static_cast<int>(info->progress * 100.0f),
                  info->desc ? info->desc : "");
    set_progress(*app, buf);
}

static void on_pulse_log(void * /*user_context*/, PulseDebugLevel level,
                         const char * category, int64_t /*wall_time_us*/,
                         int64_t /*elapsed_nano*/, unsigned int /*pid*/,
                         const char * /*file*/, const char * /*function*/,
                         int /*line*/, const char * /*object_debug_str*/,
                         const char * message)
{
    //if (level > PULSE_LEVEL_WARNING) return;
    std::fprintf(stderr, "[pulse:%s] %s\n",
                 category ? category : "?", message ? message : "");
}

// Fired by Pulse (external-rest mode) when its local SDP changes after
// stage 2 — e.g. it added a content channel, or renegotiated codecs. A
// real client would forward this as a SIP re-INVITE via PJSIP; for this
// demo we just surface it in the status line so the user can see it
// happen.
static void on_pulse_update_sdp(void * user_context, const char * update_sdp)
{
    auto * app = static_cast<AppState *>(user_context);
    const size_t len = update_sdp ? std::strlen(update_sdp) : 0;
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "Pulse requested SDP update (%zu bytes) - re-INVITE not implemented in demo.",
                  len);
    set_status(*app, buf);
}

// ----------------------------------------------------------------------------
//  Pulse setup (same device bindings as the REST demo).
// ----------------------------------------------------------------------------

static void install_callbacks(AppState & app)
{
    PulseConferenceStatusCallbackConfig conf_cb{ on_conference_status, &app };
    pulse_options_set_conference_state_callback(app.pulse, &conf_cb);
    pulse_options_set_application_user_agent_string(app.pulse, "doppler-sip/0.1");
}

static void connect_default_devices(AppState & app)
{
    struct Binding {
        const char *        name;
        PulseMediaType      type;
        PulseMediaDirection direction;
    };
    const Binding bindings[] = {
        { "camera",     PULSE_MEDIA_VIDEO, PULSE_MEDIA_INPUT  },
        { "microphone", PULSE_MEDIA_AUDIO, PULSE_MEDIA_INPUT  },
        { "speaker",    PULSE_MEDIA_AUDIO, PULSE_MEDIA_OUTPUT },
    };
    for (const Binding & b : bindings) {
        PulseError err = pulse_device_session_connect_system_default(
            app.pulse, PULSE_MEDIA_CONTENT_MAIN, b.type, b.direction);
        if (err != PULSE_SUCCESS) {
            std::fprintf(stderr, "Failed to attach default %s: %s\n",
                         b.name, pulse_strerror(err));
        }
    }
}

static void uninstall_callbacks(AppState & app)
{
    pulse_options_set_conference_state_callback(app.pulse, nullptr);
}

// ----------------------------------------------------------------------------
//  The SIP-driven call flow
// ----------------------------------------------------------------------------

// Stage 2 of Pulse setup, fired from the SIP worker thread once we have
// the 200 OK / answer SDP.
static void on_sip_answer(AppState & app, const doppler::SipAnswer & ans)
{
    // Hand the answer's IP + port table to the app-transport bridge so its
    // outbound callback knows where to sendto(). Must happen before
    // pulse_setup_stage_2_from_structure() returns, because Pulse will
    // start invoking the outbound callback as soon as stage 2 succeeds.
    if (app.transport) {
        std::string err = app.transport->configure_remote_answer(ans.remote_sdp);
        if (!err.empty()) {
            set_status(app, std::string("app-transport: ") + err);
            return;
        }
    }

    // The Call-ID is what we (for now) feed into Pulse's call_uuid slot —
    // see the README for the open question on this.
    PulseSetupStage2Config cfg{};
    cfg.call_uuid  = ans.call_id.c_str();
    cfg.remote_sdp = ans.remote_sdp.c_str();

    PulseError err = pulse_setup_stage_2_from_structure(app.pulse, &cfg);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_setup_stage_2_from_structure failed: ")
                            + pulse_strerror(err));
        return;
    }
    app.stage.store(static_cast<int>(CallStage::Stage2Done));
    set_status(app, "SIP 200 OK received - Pulse media engaged.");
}

static void on_sip_failure(AppState & app, const std::string & reason)
{
    set_status(app, std::string("SIP call failed: ") + reason);
    set_progress(app, "");
    // Pulse is in the middle of stage-1; tear it down so we can try again.
    // We're on PJSIP's worker thread here, so use the async variant - a
    // synchronous pulse_disconnect() would block the SIP event loop until
    // Pulse's teardown is done.
    if (pulse_is_connected(app.pulse)) {
        PulseAsyncOperationResultCallbackConfig result_cb{ on_async_result, &app };
        pulse_disconnect_async(app.pulse, &result_cb, nullptr);
    }
    app.stage.store(static_cast<int>(CallStage::Idle));
}

static void on_sip_ended(AppState & app, const std::string & reason)
{
    set_status(app, std::string("SIP call ended: ") + reason);
    set_progress(app, "");
    // Walk Pulse back to the idle state so the user can place another call.
    if (pulse_is_connected(app.pulse)) {
        PulseAsyncOperationResultCallbackConfig result_cb{ on_async_result, &app };
        pulse_disconnect_async(app.pulse, &result_cb, nullptr);
    }
    app.stage.store(static_cast<int>(CallStage::Idle));
}

// "Call" button handler. UI thread only.
static void start_call(AppState & app)
{
    if (app.sip_uri[0] == '\0') {
        set_status(app, "Please enter a SIP URI.");
        return;
    }
    if (app.stage.load() != static_cast<int>(CallStage::Idle)) {
        set_status(app, "A call is already in progress.");
        return;
    }

    set_status(app, "Asking Pulse for a SIP-mode SDP offer...");

    // ---- Stage 1: get a local SDP offer from Pulse ---------------------
    // Note: the AppTransport bridge (if enabled via DOPPLER_SIP_BRIDGE) was
    // already bound to Pulse at startup, inside main(). It HAS to be — Pulse
    // rejects pulse_options_set_app_transport() with ALREADY_CONNECTED once
    // any *_connect_* API has run, which by this point includes device-session
    // and data-session connects. So all we have to do here is read the result:
    // app.transport is non-null iff the startup bind succeeded, in which case
    // we'll rewrite the SDP below; otherwise Pulse drives its own UDP sockets
    // directly and the SDP it hands us is what PJSIP puts on the wire.
    PulseSetupStage1Config cfg{};
    cfg.is_sip              = true;
    cfg.disable_trickle_ice = true;   // simpler exchange for direct SIP
    cfg.stun_config         = nullptr;
    cfg.turn_config         = nullptr;

    const char * local_sdp_ptr = nullptr;
    PulseError err = pulse_setup_stage_1_from_structure(app.pulse, &cfg,
                                                        &local_sdp_ptr);
    if (err != PULSE_SUCCESS || !local_sdp_ptr) {
        set_status(app, std::string("pulse_setup_stage_1_from_structure failed: ")
                            + pulse_strerror(err));
        return;
    }
    // Copy the SDP out immediately — Pulse owns the underlying buffer.
    std::string local_sdp = local_sdp_ptr;

    // Replace the m=/c=/a=rtcp ports in Pulse's offer with the ports of
    // the UDP sockets the AppTransport owns, so the SIP peer sends RTP to
    // *us* and we relay it back into Pulse via pulse_app_transport_push().
    //
    // Only runs when the bridge was opted into (DOPPLER_SIP_BRIDGE != 0)
    // AND bind_to_pulse() succeeded at startup. Otherwise app.transport
    // is nullptr and Pulse's SDP is forwarded to PJSIP unmodified, with
    // Pulse's own UDP sockets advertised in the INVITE.
    if (app.transport) {
        std::string xport_err;
        std::string rewritten = app.transport->configure_local_offer(local_sdp, xport_err);
        if (!xport_err.empty()) {
            set_status(app, std::string("app-transport: ") + xport_err);
            app.stage.store(static_cast<int>(CallStage::Idle));
            return;
        }
        local_sdp = std::move(rewritten);
    }

    app.stage.store(static_cast<int>(CallStage::Stage1Done));

    // ---- Send the SIP INVITE ------------------------------------------
    set_status(app, std::string("Calling ") + app.sip_uri + " ...");
    const std::string target = app.sip_uri;
    const std::string from   = app.display_name;
    bool ok = app.sip->place_call(
        target, local_sdp, from,
        [app_ptr = &app](const doppler::SipAnswer & ans) { on_sip_answer(*app_ptr, ans); },
        [app_ptr = &app](const std::string & reason)     { on_sip_failure(*app_ptr, reason); },
        [app_ptr = &app](const std::string & reason)     { on_sip_ended(*app_ptr, reason); });

    if (!ok) {
        // place_call has already invoked on_sip_failure via its callback,
        // which has tidied up Pulse - nothing left to do here.
    }
}

// "Hang up" button handler. UI thread only.
static void start_hangup(AppState & app)
{
    set_status(app, "Hanging up...");
    if (app.sip)
        app.sip->hangup();
    // The SIP "ended" callback will drive pulse_disconnect_async; but if
    // we're somehow in stage-1 without a confirmed dialog, ask Pulse to
    // disconnect anyway so we don't leak its media setup.
    if (app.stage.load() == static_cast<int>(CallStage::Stage1Done)
            && pulse_is_connected(app.pulse)) {
        pulse_disconnect(app.pulse, nullptr);
        app.stage.store(static_cast<int>(CallStage::Idle));
    }
}

// ----------------------------------------------------------------------------
//  Video rendering (Pulse data-session -> GL texture -> ImGui::Image)
// ----------------------------------------------------------------------------
//
//  Lifted (with credit) from src/main.cpp's GLTextureContext pipeline,
//  which is itself a slimmed-down copy of pexninja's render_gl_ctx_image.
//
//  We want a visible preview of what Pulse is producing once a SIP call
//  goes through, both so the operator can verify that media really is
//  flowing and so we have a fast feedback loop while debugging the
//  Pulse/PJSIP/AppTransport plumbing. Same recipe as the REST demo:
//
//      1. Tell Pulse NOT to spawn its own native windows for self-view,
//         far-end and presentation (set_*_window_handle(nullptr) BEFORE
//         the first connect, otherwise Pulse will pop them up the moment
//         media flows).
//      2. Open a PulseDataSession output per content slot we want to
//         render, asking for `video/x-raw, format=RGBA` so the frames
//         are already in a format glTexImage2D understands.
//      3. Every ImGui frame, pull the freshest decoded frame
//         (timeout=0 - non-blocking; we'd rather skip a frame than
//         stall the UI) and re-upload it into a GL texture.
//      4. ImGui::Image draws the texture inside a tile.
// ----------------------------------------------------------------------------

struct GLTextureContext
{
    GLuint            texture       = 0;
    PulseMediaContent media_content = PULSE_MEDIA_CONTENT_MAIN;
    int               last_width    = 0;
    int               last_height   = 0;
};

static PulseDataSessionConfig * make_video_data_session_config()
{
    PulseDataSessionConfig * cfg =
        pulse_data_session_config_new(PULSE_DATA_SESSION_VIDEO_FROM_CAPS);
    pulse_data_session_config_video_from_caps(cfg, "video/x-raw, format=RGBA");
    return cfg;
}

static void init_video_render_ctx(Pulse * pulse, GLTextureContext & ctx,
                                  PulseMediaContent media_content)
{
    glGenTextures(1, &ctx.texture);
    glBindTexture(GL_TEXTURE_2D, ctx.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ctx.media_content = media_content;

    PulseDataSessionConfig * cfg = make_video_data_session_config();
    pulse_data_session_connect_output(pulse, cfg, media_content);
    pulse_data_session_config_free(cfg);
}

static void shutdown_video_render_ctx(Pulse * pulse, GLTextureContext & ctx)
{
    pulse_data_session_disconnect(pulse, PULSE_MEDIA_VIDEO,
                                  PULSE_MEDIA_OUTPUT, ctx.media_content);
    if (ctx.texture) {
        glDeleteTextures(1, &ctx.texture);
        ctx.texture = 0;
    }
}

// Try to pull a fresh RGBA frame and upload it to the GL texture. No-op if
// nothing is ready yet (we poll with timeout=0 so we never block the UI).
// If the resolution changes mid-call (re-INVITE, layout change, ...) the
// next glTexImage2D will resize the texture automatically.
static void pump_frame_into_texture(Pulse * pulse, GLTextureContext & ctx)
{
    PulseDataSessionFrameData * frame = nullptr;
    pulse_data_session_pull_frame_data(pulse, PULSE_MEDIA_VIDEO, &frame,
                                       ctx.media_content, 0);
    if (!frame) return;

    int w = 0, h = 0;
    if (pulse_frame_data_get_resolution(frame, &w, &h) && w > 0 && h > 0) {
        glBindTexture(GL_TEXTURE_2D, ctx.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, frame->data);
        ctx.last_width  = w;
        ctx.last_height = h;
    }
    pulse_data_session_frame_data_free(frame);
}

// ----------------------------------------------------------------------------
//  ImGui control panel
// ----------------------------------------------------------------------------

static void draw_ui(AppState & app, GLTextureContext & remote_ctx,
                    GLTextureContext & selfview_ctx)
{
    // Pull the freshest frame from each Pulse output session BEFORE we
    // start drawing - the upload itself is a GL state mutation, so we
    // want it sandwiched between glfwMakeContextCurrent (already done
    // by the GLFW backend before draw_ui) and ImGui::Image. The pull
    // is non-blocking (timeout=0) and harmless if no session has
    // produced a frame yet - the texture just keeps showing whatever
    // it last had.
    pump_frame_into_texture(app.pulse, remote_ctx);
    pump_frame_into_texture(app.pulse, selfview_ctx);

    ImGuiViewport * vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::Begin("Doppler - Pexip Pulse + SIP demo", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextUnformatted("Pexip Pulse video-call demo (signalling via PJSIP)");
    ImGui::Separator();

    ImGui::InputText("SIP URI",      app.sip_uri,      sizeof(app.sip_uri));
    ImGui::InputText("Display name", app.display_name, sizeof(app.display_name));
    ImGui::TextDisabled("e.g. havard@pexipdemo.com");

    ImGui::Spacing();

    const int stage = app.stage.load();
    const bool can_call   = (stage == static_cast<int>(CallStage::Idle));
    const bool can_hangup = (stage != static_cast<int>(CallStage::Idle));

    ImGui::BeginDisabled(!can_call);
    if (ImGui::Button("Call")) start_call(app);
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!can_hangup);
    if (ImGui::Button("Hang up")) start_hangup(app);
    ImGui::EndDisabled();

    // ---- AppTransport bridge status (read-only) -----------------------
    // The bridge decision is latched at startup from DOPPLER_SIP_BRIDGE
    // because pulse_options_set_app_transport() must run before any
    // *_connect_* API touches the Pulse handle. We expose the resulting
    // state here as a non-interactive checkbox + label so the operator
    // can see at a glance which transport mode this build is using.
    ImGui::SameLine();
    bool bridge_enabled_view = app.bridge_enabled.load();
    ImGui::BeginDisabled(true);
    ImGui::Checkbox("AppTransport bridge", &bridge_enabled_view);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (app.bridge_bound.load()) {
        ImGui::TextDisabled("(bound at startup)");
    } else if (bridge_enabled_view) {
        ImGui::TextDisabled("(bind failed - see status)");
    } else {
        ImGui::TextDisabled("(disabled - DOPPLER_SIP_BRIDGE=0)");
    }

    ImGui::Separator();
    ImGui::Text("Pulse state: %s", status_to_string(app.connection_status.load()));
    const char * stage_str = "Idle";
    switch (static_cast<CallStage>(stage)) {
        case CallStage::Idle:        stage_str = "Idle";                    break;
        case CallStage::Stage1Done:  stage_str = "INVITE sent (waiting 200)"; break;
        case CallStage::Stage2Done:  stage_str = "In call";                  break;
    }
    ImGui::Text("SIP stage:   %s", stage_str);

    std::string status_text, progress_text;
    {
        std::lock_guard<std::mutex> lock(app.text_mutex);
        status_text   = app.status_text;
        progress_text = app.progress_text;
    }
    ImGui::TextWrapped("%s", status_text.c_str());
    if (!progress_text.empty())
        ImGui::TextWrapped("%s", progress_text.c_str());

    // ---- Video tiles --------------------------------------------------
    // Two side-by-side tiles in 16:9: "Remote" shows what Pulse decoded
    // from the SIP peer's RTP (MAIN content slot - this is the whole
    // point: "did we actually receive media?"), "Self-view" shows our
    // local camera feed (SELFVIEW slot) as a sanity check that capture
    // is working. Each tile draws an empty grey rectangle placeholder
    // until its first frame arrives so the layout doesn't jump.
    ImGui::Spacing();
    ImGui::Separator();
    {
        const float avail  = ImGui::GetContentRegionAvail().x;
        const float tile_w = (avail - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const float tile_h = tile_w * 9.0f / 16.0f;

        auto draw_tile = [&](const char * label, GLTextureContext & ctx) {
            ImGui::BeginGroup();
            ImGui::TextUnformatted(label);
            if (ctx.texture && ctx.last_width > 0 && ctx.last_height > 0) {
                ImGui::Image((ImTextureID)(uintptr_t)ctx.texture,
                             ImVec2(tile_w, tile_h));
            } else {
                ImGui::Dummy(ImVec2(tile_w, tile_h));
                ImGui::GetWindowDrawList()->AddRect(
                    ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                    IM_COL32(80, 80, 80, 255));
            }
            ImGui::EndGroup();
        };

        draw_tile("Remote (what we are receiving)", remote_ctx);
        ImGui::SameLine();
        draw_tile("Self-view (local camera)",       selfview_ctx);
    }

    // ---- Live per-bridge UDP counters ---------------------------------
    // One row per AppTransport channel (= one UDP socket), built fresh
    // every frame so the panel auto-resizes from one m= section up to
    // however many wires Pulse negotiated.
    //
    // `app.transport` is non-null iff the startup bind succeeded (see
    // DOPPLER_SIP_BRIDGE). If the user opted out we render a one-line
    // notice; if the bind failed we render a different one so the cause
    // is obvious. Once a call sets up SDP, the snapshot table lights up.
    ImGui::Spacing();
    ImGui::Separator();
    if (!app.transport) {
        if (app.bridge_enabled.load())
            ImGui::TextDisabled(
                "UDP bridges: AppTransport bind failed at startup - "
                "Pulse owns sockets directly. See status text above.");
        else
            ImGui::TextDisabled(
                "UDP bridges: AppTransport disabled at startup "
                "(DOPPLER_SIP_BRIDGE=0). Pulse owns sockets directly. "
                "Restart without that env var to engage the bridge.");
    } else {
        std::vector<doppler::BridgeStat> stats = app.transport->snapshot();
        if (stats.empty()) {
            ImGui::TextDisabled("UDP bridges: none yet (no SDP offered).");
        } else {
            ImGui::Text("UDP bridges (%zu):", stats.size());
            const ImGuiTableFlags flags = ImGuiTableFlags_Borders
                                        | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("udp_bridges", 7, flags)) {
                ImGui::TableSetupColumn("Media");
                ImGui::TableSetupColumn("Kind");
                ImGui::TableSetupColumn("Local");
                ImGui::TableSetupColumn("Remote");
                ImGui::TableSetupColumn("TX (pkts / bytes)");
                ImGui::TableSetupColumn("RX (pkts / bytes)");
                ImGui::TableSetupColumn("Direction");
                ImGui::TableHeadersRow();

                for (const auto & s : stats) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(s.media.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(s.kind.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(s.local_endpoint.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(s.remote_endpoint.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu / %llu",
                                static_cast<unsigned long long>(s.tx_packets),
                                static_cast<unsigned long long>(s.tx_bytes));
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu / %llu",
                                static_cast<unsigned long long>(s.rx_packets),
                                static_cast<unsigned long long>(s.rx_bytes));
                    ImGui::TableNextColumn();
                    // Directional indicator: arrows light up as soon as the
                    // first packet flows in each direction, so at a glance
                    // you can see which legs are "live".
                    const char * tx_arrow = (s.tx_packets > 0) ? "TX>" : "tx ";
                    const char * rx_arrow = (s.rx_packets > 0) ? "<RX" : " rx";
                    ImGui::Text("%s %s", tx_arrow, rx_arrow);
                }
                ImGui::EndTable();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped(
        "Pulse owns all media. PJSIP only signals: it sends INVITE over TCP "
        "with Pulse's SDP offer and pipes the 200 OK's answer SDP straight "
        "back into pulse_setup_stage_2_from_structure(). No REGISTER, no "
        "credentials, no TLS - this is the most minimal SIP UA we could "
        "build.");

    ImGui::End();
}

// ----------------------------------------------------------------------------
//  main()
// ----------------------------------------------------------------------------

static void glfw_error_callback(int error, const char * description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

int main()
{
    // ---- GLFW + ImGui (same as src/main.cpp) ----------------------------
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow * window = glfwCreateWindow(960, 720,
                                           "Doppler - Pulse + SIP demo",
                                           nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // ---- Boot Pulse ------------------------------------------------------
    pulse_global_logger_callback(on_pulse_log, nullptr);

    AppState app;

    // Latch the AppTransport bridge decision at startup. Pulse's
    // pulse_options_set_app_transport() returns PULSE_ERROR_ALREADY_CONNECTED
    // the moment the client enters the connected state, and that happens
    // implicitly the first time we touch a `*_connect_*` API
    // (pulse_device_session_connect_system_default, pulse_data_session_connect_output,
    // pulse_setup_stage_1_*). So the bind has to be the *first* thing we do
    // on a freshly-created Pulse handle - well before connect_default_devices,
    // before init_video_render_ctx, and obviously before start_call. That
    // means the bridge can't be a runtime UI toggle: by the time the user
    // ticks a checkbox, Pulse is long since past the point of no return.
    //
    // We therefore latch the decision once, at startup, from the
    // DOPPLER_SIP_BRIDGE env var. Default = on, because routing media
    // through our app-owned UDP sockets is the whole point of this demo;
    // set DOPPLER_SIP_BRIDGE=0 to fall back to Pulse-owned sockets for
    // direct comparison.
    bool bridge_wanted = true;
    if (const char * env = std::getenv("DOPPLER_SIP_BRIDGE")) {
        bridge_wanted = !(env[0] == '0' && env[1] == '\0');
    }
    app.bridge_enabled.store(bridge_wanted);

    // External-rest mode: PJSIP owns SIP signalling, Pulse is purely a
    // media engine. The update_sdp callback fires if Pulse later wants
    // to renegotiate (e.g. add a content stream); we register it against
    // `&app` so it can post into the UI status line.
    PulseExternalRestCallbackConfig ext_rest_cfg{};
    ext_rest_cfg.update_sdp_callback      = on_pulse_update_sdp;
    ext_rest_cfg.update_sdp_user_context  = &app;
    app.pulse = pulse_new_external_rest(ext_rest_cfg);
    if (!app.pulse) {
        std::fprintf(stderr, "pulse_new_external_rest() returned NULL\n");
        return 1;
    }

    // ---- Boot the application-owned RTP/RTCP transport (FIRST) -----------
    // This must be the very first thing we do on the fresh Pulse handle,
    // before install_callbacks, before any pulse_options_set_* call, and
    // critically before connect_default_devices and init_video_render_ctx.
    // pulse_options_set_app_transport() is rejected with
    // PULSE_ERROR_ALREADY_CONNECTED once any *_connect_* API has put the
    // client into the connected state, so there is no "bind later" path.
    // If the user opted out via DOPPLER_SIP_BRIDGE=0 we skip the bind and
    // leave app.transport == nullptr, which short-circuits every later
    // `if (app.transport)` site and lets Pulse drive its own UDP sockets.
    doppler::AppTransport transport;
    app.transport_storage = &transport;
    if (bridge_wanted) {
        PulseError xport_err = transport.bind_to_pulse(app.pulse);
        if (xport_err != PULSE_SUCCESS) {
            std::fprintf(stderr,
                "pulse_options_set_app_transport failed at startup: %s "
                "- continuing with Pulse-owned sockets.\n",
                pulse_strerror(xport_err));
            app.bridge_enabled.store(false);
        } else {
            app.transport = &transport;
            app.bridge_bound.store(true);
        }
    }

    install_callbacks(app);

    // We render the video ourselves below (see GLTextureContext) by pulling
    // RGBA frames out of Pulse via the data-session API. Tell Pulse NOT
    // to also spawn its own native windows for self-view, the far end
    // or presentation - these have to be cleared BEFORE the first
    // connect (i.e. before stage_1), otherwise Pulse pops them up the
    // moment media starts flowing. Mirrors src/main.cpp.
    pulse_options_set_self_view_window_handle         (app.pulse, nullptr);
    pulse_options_set_remote_video_window_handle      (app.pulse, nullptr);
    pulse_options_set_presentation_video_window_handle(app.pulse, nullptr);

    connect_default_devices(app);

    // Open RGBA data-session outputs for the streams we want to render
    // inline in the ImGui window: MAIN (incoming far-end video - this
    // answers "are we actually receiving anything?") and SELFVIEW (our
    // own camera feed - a quick capture sanity check). Pulse starts
    // feeding frames in as soon as media exists; pump_frame_into_texture
    // picks them up each ImGui frame from inside draw_ui().
    GLTextureContext remote_ctx;
    GLTextureContext selfview_ctx;
    init_video_render_ctx(app.pulse, remote_ctx,   PULSE_MEDIA_CONTENT_MAIN);
    init_video_render_ctx(app.pulse, selfview_ctx, PULSE_MEDIA_CONTENT_SELFVIEW);

    // ---- Boot PJSIP ------------------------------------------------------
    doppler::SipUA sip;
    std::string sip_err = sip.start("doppler-sip/0.1", /*local_port=*/0);
    if (!sip_err.empty()) {
        std::fprintf(stderr, "PJSIP start failed: %s\n", sip_err.c_str());
        // Carry on without SIP so the UI still surfaces the error to the user.
    }
    app.sip = &sip;

    // ---- Main loop -------------------------------------------------------
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        draw_ui(app, remote_ctx, selfview_ctx);
        ImGui::Render();
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // ---- Shutdown --------------------------------------------------------
    sip.stop();                     // sends BYE if needed
    if (pulse_is_connected(app.pulse))
        pulse_disconnect(app.pulse, nullptr);
    // Disconnect the video data-sessions and delete the GL textures
    // before we tear Pulse down. The GL context is still current here
    // (glfwDestroyWindow happens later), so glDeleteTextures is safe.
    shutdown_video_render_ctx(app.pulse, remote_ctx);
    shutdown_video_render_ctx(app.pulse, selfview_ctx);
    // Stop the reader thread, close sockets, clear the app-transport
    // binding - all before pulse_free() so Pulse never sees a dangling
    // callback while it's tearing the media engine down. Safe (no-op)
    // when the bridge checkbox stayed off and we never bound it.
    transport.shutdown();
    uninstall_callbacks(app);
    pulse_free(app.pulse);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
