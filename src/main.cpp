// ============================================================================
//  doppler - a tiny Pexip Pulse video-call demo
// ----------------------------------------------------------------------------
//
//  The whole point of this file is to show, in as few lines as possible,
//  how to make a Pexip Infinity video call using the Pulse C API.
//
//  The flow looks like this:
//
//      1.  pulse_new()                     -> create a Pulse instance.
//      2.  pulse_options_set_*()           -> register a few callbacks so we
//                                             learn about connection state
//                                             changes and async results.
//      3.  pulse_connect_with_rest_async() -> connect to a conference; Pulse
//                                             handles all the REST + media
//                                             setup for us and (because we
//                                             don't override the video window
//                                             handles) auto-spawns its own
//                                             video windows for self-view and
//                                             remote video.
//      4.  pulse_disconnect_async()        -> tear it all down.
//      5.  pulse_free()                    -> release the handle.
//
//  Everything else in this file is just Dear ImGui plumbing to draw a tiny
//  control panel.  If you ever need to write your own Pulse client, you can
//  copy the four steps above almost verbatim.
// ============================================================================

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// Dear ImGui + the two backends we link against in CMake.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

// The single header that pulls in the full Pulse API surface.
#include <pexpulse/pulse.h>

// ----------------------------------------------------------------------------
//  Application state
// ----------------------------------------------------------------------------
//
//  Pulse callbacks fire on internal Pulse threads, so anything they touch has
//  to be thread-safe.  We keep the shared state in a single struct guarded by
//  a mutex and use std::atomic for the few fields that the UI thread polls
//  every frame.
// ----------------------------------------------------------------------------

struct AppState
{
    // The Pulse handle.  Owned by main(), only used from the UI thread for
    // calls into Pulse.  Pulse itself is internally thread-safe.
    Pulse * pulse = nullptr;

    // Form fields (kept as fixed-size buffers because that's what ImGui's
    // InputText API expects).
    char server[256]       = "";          // e.g. "conferencing.example.com"
    char conference[256]   = "";          // e.g. "meet.alice"
    char display_name[128] = "Doppler demo";
    char pin[32]           = "";          // optional, may be empty

    // Latest conference status, written from a Pulse callback thread.
    // `int` because std::atomic<enum> is annoyingly verbose and the values
    // map one-to-one to PulseConnectionStatus.
    std::atomic<int>  connection_status{PULSE_CONNECTION_STATUS_DISCONNECTED};

    // The most recent async-result error code (PULSE_SUCCESS == "all good").
    std::atomic<int>  last_async_error{PULSE_SUCCESS};

    // Free-form status / progress text shown in the UI.  Guarded by mutex.
    std::mutex   text_mutex;
    std::string  status_text  = "Idle. Fill in the form and press Connect.";
    std::string  progress_text;
};

// Small helper - thread-safe update of the status/progress strings.
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

// Pretty-print the connection status enum.
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
//  Pulse callbacks
// ----------------------------------------------------------------------------
//
//  All Pulse callbacks share the same shape:
//
//      void callback(<some_payload>, void * user_context);
//
//  We pass our AppState* as the user_context when we register them and cast
//  it back here.  Keep these implementations short - they run on Pulse's
//  internal worker threads, so blocking them blocks Pulse itself.
// ----------------------------------------------------------------------------

// Fired whenever the conference connection status changes.
static void on_conference_status(const PulseConferenceStatusInfo * info, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    app->connection_status.store(static_cast<int>(info->status));
    set_status(*app, std::string("Conference status: ") + status_to_string(info->status));
}

// Fired when an async operation (connect/disconnect) finishes.
static void on_async_result(const PulseError err, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    app->last_async_error.store(static_cast<int>(err));
    if (err == PULSE_SUCCESS) {
        set_status(*app, "Async operation completed successfully.");
    } else {
        set_status(*app,
                   std::string("Async operation failed: ") + pulse_strerror(err));
    }
    set_progress(*app, "");
}

// Fired periodically during async connect/disconnect so the UI can show a
// human-readable progress message ("Resolving DNS...", "Negotiating media...",
// etc).  We just forward the text into our status panel.
static void on_progress(const PulseOperationProgressInfo * info, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[%3d%%] %s",
                  static_cast<int>(info->progress * 100.0f),
                  info->desc ? info->desc : "");
    set_progress(*app, buf);
}

// Optional logging hook - keeps Pulse's chatter out of stdout unless we want
// it.  Wired into Pulse via pulse_global_logger_callback() from main().
static void on_pulse_log(void * /*user_context*/, PulseDebugLevel level,
                         const char * category, int64_t /*wall_time_us*/,
                         int64_t /*elapsed_nano*/, unsigned int /*pid*/,
                         const char * /*file*/, const char * /*function*/,
                         int /*line*/, const char * /*object_debug_str*/,
                         const char * message)
{
    // Only print warnings and above so the terminal stays readable.
    if (level > PULSE_LEVEL_WARNING) return;
    std::fprintf(stderr, "[pulse:%s] %s\n",
                 category ? category : "?", message ? message : "");
}

// ----------------------------------------------------------------------------
//  Pulse glue
// ----------------------------------------------------------------------------

// Wires up the callbacks we care about.  This is the only "set-up" code our
// demo needs beyond pulse_new() - everything else is just business logic.
static void install_callbacks(AppState & app)
{
    PulseConferenceStatusCallbackConfig conf_cb{
        on_conference_status,
        &app,
    };
    pulse_options_set_conference_state_callback(app.pulse, &conf_cb);

    // Tag ourselves so the server-side logs show who connected.
    pulse_options_set_application_user_agent_string(app.pulse, "doppler/0.1");
}

// Kick off an async connect to a Pexip Infinity conference.
static void start_connect(AppState & app)
{
    if (app.server[0] == '\0' || app.conference[0] == '\0') {
        set_status(app, "Please fill in at least 'Server' and 'Conference'.");
        return;
    }

    // The connection config is purely value-based - Pulse copies what it
    // needs internally, so it's safe to let `cfg` go out of scope right
    // after the call.
    PulseRestConnectionConfig cfg{};
    cfg.server_address   = app.server;
    cfg.conference_name  = app.conference;
    cfg.display_name     = app.display_name;
    cfg.pin_code         = app.pin[0] ? app.pin : nullptr;

    PulseAsyncOperationResultCallbackConfig result_cb{
        on_async_result,
        &app,
    };
    PulseOperationProgressCallbackConfig progress_cb{
        on_progress,
        &app,
    };

    set_status(app, std::string("Connecting to ") + app.server
                        + " / " + app.conference + " ...");

    // _async returns immediately; the real outcome arrives via on_async_result.
    PulseError err = pulse_connect_with_rest_async(app.pulse, &cfg,
                                                   &result_cb, &progress_cb);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_connect_with_rest_async failed: ")
                            + pulse_strerror(err));
    }
}

// Tear down the current call.  Safe to call even when nothing is connected.
static void start_disconnect(AppState & app)
{
    PulseAsyncOperationResultCallbackConfig result_cb{
        on_async_result,
        &app,
    };
    PulseOperationProgressCallbackConfig progress_cb{
        on_progress,
        &app,
    };
    set_status(app, "Disconnecting...");
    PulseError err = pulse_disconnect_async(app.pulse, &result_cb, &progress_cb);
    if (err != PULSE_SUCCESS) {
        set_status(app, std::string("pulse_disconnect_async failed: ")
                            + pulse_strerror(err));
    }
}

// ----------------------------------------------------------------------------
//  ImGui control panel
// ----------------------------------------------------------------------------

static void draw_ui(AppState & app)
{
    // Make the panel fill the GLFW window for a clean "single-purpose" feel.
    ImGuiViewport * vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::Begin("Doppler - Pexip Pulse demo", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextUnformatted("Pexip Pulse video-call demo");
    ImGui::Separator();

    ImGui::InputText("Server",       app.server,       sizeof(app.server));
    ImGui::InputText("Conference",   app.conference,   sizeof(app.conference));
    ImGui::InputText("Display name", app.display_name, sizeof(app.display_name));
    ImGui::InputText("PIN (opt.)",   app.pin,          sizeof(app.pin),
                     ImGuiInputTextFlags_Password);

    ImGui::Spacing();

    const int status = app.connection_status.load();
    const bool can_connect    = (status == PULSE_CONNECTION_STATUS_DISCONNECTED);
    const bool can_disconnect = (status == PULSE_CONNECTION_STATUS_CONNECTED  ||
                                 status == PULSE_CONNECTION_STATUS_CONNECTING ||
                                 status == PULSE_CONNECTION_STATUS_RECONNECTING);

    ImGui::BeginDisabled(!can_connect);
    if (ImGui::Button("Connect")) start_connect(app);
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!can_disconnect);
    if (ImGui::Button("Disconnect")) start_disconnect(app);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("State: %s", status_to_string(status));

    // Pull the latest status / progress strings out under the mutex.
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
        "Tip: Pulse auto-spawns its own native windows for self-view and "
        "remote video once a call is up.  This demo deliberately does NOT "
        "embed them into the ImGui window so the example stays minimal - "
        "see pulse_options_set_remote_video_window_handle() if you want "
        "to render into your own window.");

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
    // ---- 1.  Boot GLFW + an OpenGL context for ImGui ---------------------
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }
    // Request a basic OpenGL 3.2 core context - that's what ImGui's GL3
    // backend expects by default.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow * window = glfwCreateWindow(640, 480,
                                           "Doppler - Pulse demo", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    // ---- 2.  Boot Dear ImGui --------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    // ---- 3.  Boot Pulse --------------------------------------------------
    //
    // pulse_global_logger_callback MUST be installed *before* the first
    // pulse_new() call, otherwise the early-startup log lines are lost.
    pulse_global_logger_callback(on_pulse_log, nullptr);

    AppState app;
    app.pulse = pulse_new();
    if (!app.pulse) {
        std::fprintf(stderr, "pulse_new() returned NULL\n");
        return 1;
    }
    install_callbacks(app);

    // ---- 4.  The classic ImGui main loop ---------------------------------
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

    // ---- 5.  Clean shutdown ---------------------------------------------
    //
    // Always ask Pulse to disconnect synchronously on the way out - this
    // makes sure any in-flight media and sockets are torn down properly
    // before pulse_free() releases the handle.
    if (pulse_is_connected(app.pulse))
        pulse_disconnect(app.pulse, nullptr);
    pulse_free(app.pulse);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
