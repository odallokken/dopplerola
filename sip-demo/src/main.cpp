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

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <pexpulse/pulse.h>

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

struct AppState
{
    Pulse *           pulse = nullptr;
    doppler::SipUA *  sip   = nullptr;

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
    if (level > PULSE_LEVEL_WARNING) return;
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
//  ImGui control panel
// ----------------------------------------------------------------------------

static void draw_ui(AppState & app)
{
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped(
        "Pulse owns all media. PJSIP only signals: it sends INVITE over UDP "
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

    GLFWwindow * window = glfwCreateWindow(680, 520,
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
    install_callbacks(app);
    connect_default_devices(app);

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
        draw_ui(app);
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
    uninstall_callbacks(app);
    pulse_free(app.pulse);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
