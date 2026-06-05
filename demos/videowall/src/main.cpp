// ============================================================================
//  videowall - a multi-instance Pexip Pulse compositor
// ----------------------------------------------------------------------------
//
//  Think of a huge screen in a control room - one superwide canvas (3480x1080
//  by default) projected across several projectors. Onto that canvas we drop
//  any number of *sources* and lay them out wherever we like:
//
//      * cameras            (local capture devices)
//      * RTSP streams       (IP cameras, NVRs, ...)
//      * RTMP streams       (OBS / ffmpeg publishers)
//      * still images        (jpeg / png)
//      * mp4 video files
//      * Pexip video-conferences  (dial "name@server" and show the call)
//
//  This is the same idea as the "compositor" mode inside pexninja, but with one
//  crucial architectural difference:
//
//      pexninja  : ONE Pulse instance, every source becomes a video-mix input,
//                  and Pulse's mixer composites them into a single frame.
//
//      videowall : ONE Pulse instance *per source*. Each source hides a whole
//                  Pulse behind it. We never use Pulse's mixer to combine them
//                  - instead each Pulse renders just its own source, we pull
//                  that single frame out, and *we* paint it onto the canvas at
//                  the requested location. The compositing happens in our own
//                  OpenGL/ImGui draw list, not inside Pulse.
//
//  The per-source recipe (lifted from doppler / pexninja) is uniform:
//
//      1. pulse_new()                       -> a fresh Pulse just for this tile.
//      2. NULL the window handles           -> we render the frames ourselves.
//      3. Point the Pulse's *input* at the source:
//           camera  -> pulse_device_session_connect_device
//           rtsp    -> pulse_rtsp_session_connect_input + bind_to_content(MAIN)
//           rtmp    -> pulse_rtmp_session_connect_input(MAIN)
//           image   -> pulse_video_mix_input_from_file       -> mix onto MAIN
//           mp4     -> pulse_video_mix_input_from_file_with_loop -> mix onto MAIN
//      4. Pull the *self-view* back out with a data-session output and upload
//         it to a GL texture. Self-view is Pulse's local preview of whatever
//         is driving the MAIN input, so it works the same for every source
//         kind above - exactly the "input in, self-view out" trick the brief
//         asks for.
//
//  The one odd one out is the video-conference source. There the interesting
//  picture is not our self-view but the *far end*, so instead of step 3/4 we:
//
//      3'. split "havard@pexipdemo.com" into conference="havard" and
//          server="pexipdemo.com", then pulse_connect_with_rest_async().
//      4'. pull the MAIN (incoming) video out with a data-session output.
//
//  Everything else here is Dear ImGui plumbing: a sources rail on the left to
//  add / configure / remove sources, and a scaled view of the canvas on the
//  right where you can drag the tiles around.
// ============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Dear ImGui + the two backends we link against in CMake.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

// The single header that pulls in the full Pulse API surface.
#include <pexpulse/pulse.h>

// ----------------------------------------------------------------------------
//  Source kinds
// ----------------------------------------------------------------------------

enum class SourceKind
{
    Camera = 0,
    Rtsp,
    Rtmp,
    Image,
    Mp4,
    Conference,
};

static const char * kSourceKindLabels[] = {
    "Camera", "RTSP stream", "RTMP stream", "Image (jpg/png)", "MP4 file", "Video-conference",
};

// ----------------------------------------------------------------------------
//  A single tile on the wall
// ----------------------------------------------------------------------------
//
//  Each Source owns its own Pulse instance and the GL texture we paint onto the
//  canvas. The config fields are kept as fixed-size buffers because that is
//  what ImGui's InputText API wants.
// ----------------------------------------------------------------------------

struct Source
{
    SourceKind kind = SourceKind::Camera;
    char       name[64] = "Source";

    // ---- placement on the canvas, in canvas pixels --------------------------
    float x = 0.0f, y = 0.0f;       // top-left corner
    float w = 640.0f, h = 360.0f;   // size

    // ---- per-kind configuration --------------------------------------------
    char camera_name[128] = "";     // empty => system default / first camera
    char file_path[512]   = "";     // image / mp4 path
    char rtsp_url[512]    = "rtsp://";
    int  rtmp_port        = 1935;   // each RTMP source listens on its own port
    char rtmp_path[64]    = "live";
    char conference_id[256] = "";   // "name@server", e.g. "havard@pexipdemo.com"
    char display_name[128]  = "Video wall";
    bool loop = true;               // mp4 looping

    // ---- the Pulse instance hiding behind this source ----------------------
    Pulse * pulse   = nullptr;
    bool    started = false;
    std::string last_error;

    // Which media-content slot we pull frames from for rendering. For every
    // local source this is SELFVIEW (Pulse's preview of our own input); for a
    // conference it is MAIN (the far end).
    PulseMediaContent render_content = PULSE_MEDIA_CONTENT_SELFVIEW;

    // Resources we have to release on stop().
    PulseVideoMixInputID mix_input  = PULSE_VIDEO_MIX_INPUT_ID_NONE;  // image / mp4
    PulseRtspSessionID   rtsp_session = 0;                            // rtsp (0 == none)
    bool rtmp_listening = false;

    // Live conference status (written from a Pulse callback thread).
    std::atomic<int> conn_status{PULSE_CONNECTION_STATUS_DISCONNECTED};

    // ---- rendering ----------------------------------------------------------
    GLuint texture  = 0;
    int    tex_w    = 0;
    int    tex_h    = 0;
};

struct AppState
{
    // The virtual canvas. Superwide by default - a control-room video wall.
    int canvas_w = 3480;
    int canvas_h = 1080;

    std::vector<std::unique_ptr<Source>> sources;
    int selected = -1;  // index into sources, or -1

    // A long-lived Pulse instance used only to enumerate capture devices so the
    // "Camera" dropdown has something to show before a source is started.
    Pulse * enum_pulse = nullptr;
    std::vector<std::string> camera_names;

    // The "+ Add source" form lives here so it survives across frames.
    int add_kind = 0;
};

// ----------------------------------------------------------------------------
//  Logging hook (shared by every Pulse instance)
// ----------------------------------------------------------------------------

static void on_pulse_log(void * /*uc*/, PulseDebugLevel level, const char * category,
                         int64_t /*wall*/, int64_t /*elapsed*/, unsigned int /*pid*/,
                         const char * /*file*/, const char * /*func*/, int /*line*/,
                         const char * /*obj*/, const char * message)
{
    if (level > PULSE_LEVEL_WARNING) return;  // warnings and worse only
    std::fprintf(stderr, "[pulse:%s] %s\n", category ? category : "?", message ? message : "");
}

// ----------------------------------------------------------------------------
//  Conference callbacks (one Source* per conference tile)
// ----------------------------------------------------------------------------

static void on_conf_status(const PulseConferenceStatusInfo * info, void * uc)
{
    auto * src = static_cast<Source *>(uc);
    src->conn_status.store(static_cast<int>(info->status));
}

static void on_conf_result(const PulseError err, void * uc)
{
    auto * src = static_cast<Source *>(uc);
    if (err != PULSE_SUCCESS)
        src->last_error = std::string("conference connect failed: ") + pulse_strerror(err);
}

static void on_conf_progress(const PulseOperationProgressInfo * /*info*/, void * /*uc*/)
{
    // The wall does not surface per-step progress; the status enum is enough.
}

// ----------------------------------------------------------------------------
//  Video rendering helpers (data session -> GL texture). Copied from doppler.
// ----------------------------------------------------------------------------

static PulseDataSessionConfig * make_video_data_session_config()
{
    PulseDataSessionConfig * cfg =
        pulse_data_session_config_new(PULSE_DATA_SESSION_VIDEO_FROM_CAPS);
    pulse_data_session_config_video_from_caps(cfg, "video/x-raw, format=RGBA");
    return cfg;
}

// Pull the freshest RGBA frame Pulse has for `render_content` and upload it to
// the source's GL texture. Non-blocking (timeout 0); a no-op when nothing is
// ready yet.
static void pump_frame_into_texture(Source & src)
{
    if (!src.pulse || !src.texture) return;

    PulseDataSessionFrameData * frame = nullptr;
    pulse_data_session_pull_frame_data(src.pulse, PULSE_MEDIA_VIDEO, &frame,
                                       src.render_content, 0);
    if (!frame) return;

    int w = 0, h = 0;
    if (pulse_frame_data_get_resolution(frame, &w, &h) && w > 0 && h > 0) {
        glBindTexture(GL_TEXTURE_2D, src.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, frame->data);
        src.tex_w = w;
        src.tex_h = h;
    }
    pulse_data_session_frame_data_free(frame);
}

// ----------------------------------------------------------------------------
//  Device enumeration (for the camera dropdown)
// ----------------------------------------------------------------------------

static void refresh_camera_list(AppState & app)
{
    app.camera_names.clear();
    if (!app.enum_pulse) return;

    PulseDeviceIterator * it = nullptr;
    pulse_device_iterator_new(app.enum_pulse, PULSE_MEDIA_VIDEO, PULSE_MEDIA_INPUT, &it);
    if (!it) return;
    for (const PulseDevice * d = pulse_device_iterator_first(it);
         d != nullptr;
         d = pulse_device_iterator_next(it)) {
        const char * n = pulse_device_get_name(d);
        app.camera_names.emplace_back(n ? n : "(unnamed)");
    }
    pulse_device_iterator_free(it);
}

// Bind a camera (by name, falling back to the system default / first one) to a
// source's own Pulse instance as the MAIN video input.
static PulseError connect_camera(Source & src)
{
    PulseDeviceIterator * it = nullptr;
    pulse_device_iterator_new(src.pulse, PULSE_MEDIA_VIDEO, PULSE_MEDIA_INPUT, &it);
    if (!it) return PULSE_ERROR_NOT_CONFIGURED;

    const PulseDevice * chosen   = nullptr;
    const PulseDevice * fallback = nullptr;
    for (const PulseDevice * d = pulse_device_iterator_first(it);
         d != nullptr;
         d = pulse_device_iterator_next(it)) {
        if (!fallback) fallback = d;
        if (pulse_device_is_system_default(d) && !chosen) chosen = d;
        const char * n = pulse_device_get_name(d);
        if (src.camera_name[0] != '\0' && n && std::strcmp(n, src.camera_name) == 0) {
            chosen = d;
            break;
        }
    }
    if (!chosen) chosen = fallback;

    PulseError err = PULSE_ERROR_NOT_CONFIGURED;
    if (chosen) {
        err = pulse_device_session_connect_device(src.pulse, chosen,
                                                  PULSE_MEDIA_CONTENT_MAIN);
    }
    pulse_device_iterator_free(it);
    return err;
}

// Acquire a still image / mp4 as a video-mix input and connect a one-input mix
// onto MAIN, so the source's self-view previews it. Returns the input id (which
// must be released on stop) via src.mix_input.
static PulseError connect_file_via_mix(Source & src, bool loop)
{
    PulseVideoMixInputID id = PULSE_VIDEO_MIX_INPUT_ID_NONE;
    PulseError err = loop
        ? pulse_video_mix_input_from_file_with_loop(src.pulse, src.file_path, true, &id)
        : pulse_video_mix_input_from_file(src.pulse, src.file_path, &id);
    if (err != PULSE_SUCCESS) return err;

    PulseVideoMixInput in{};
    in.input_id       = id;
    in.layer          = 0;
    in.width_ratio    = 0.0;   // 0 == fill the layer
    in.height_ratio   = 0.0;
    in.x_centrepoint  = 0.5;
    in.y_centrepoint  = 0.5;
    in.videoproc_mask = PULSE_VIDEO_PROCESS_TYPE_NONE;

    PulseVideoMixConfig cfg{};
    cfg.num_inputs = 1;
    cfg.inputs     = &in;

    err = pulse_video_mix_connect(src.pulse, &cfg, PULSE_MEDIA_CONTENT_MAIN);
    if (err != PULSE_SUCCESS) {
        pulse_video_mix_input_release(src.pulse, id);
        return err;
    }
    src.mix_input = id;
    return PULSE_SUCCESS;
}

// Split "name@server" into its two halves. Returns false if the '@' is missing.
static bool split_conference_id(const char * id, std::string & conference, std::string & server)
{
    const char * at = std::strchr(id, '@');
    if (!at || at == id || at[1] == '\0') return false;
    conference.assign(id, at);
    server.assign(at + 1);
    return true;
}

// ----------------------------------------------------------------------------
//  Source lifecycle
// ----------------------------------------------------------------------------

static void stop_source(Source & src);  // fwd

// Spin up the Pulse instance behind a source and point its input at the chosen
// source (or, for a conference, dial the call). Idempotent: calling start on an
// already-started source is a no-op.
static void start_source(Source & src)
{
    if (src.started) return;
    src.last_error.clear();

    src.pulse = pulse_new();
    if (!src.pulse) {
        src.last_error = "pulse_new() returned NULL";
        return;
    }

    // We paint the frames ourselves, so make sure Pulse never spawns its own
    // native windows. Must happen before any media flows.
    pulse_options_set_self_view_window_handle(src.pulse, nullptr);
    pulse_options_set_remote_video_window_handle(src.pulse, nullptr);
    pulse_options_set_presentation_video_window_handle(src.pulse, nullptr);
    pulse_options_set_application_user_agent_string(src.pulse, "videowall/0.1");

    PulseError err = PULSE_SUCCESS;

    switch (src.kind) {
        case SourceKind::Camera:
            src.render_content = PULSE_MEDIA_CONTENT_SELFVIEW;
            err = connect_camera(src);
            break;

        case SourceKind::Rtsp: {
            src.render_content = PULSE_MEDIA_CONTENT_SELFVIEW;
            if (src.rtsp_url[0] == '\0') { err = PULSE_ERROR_INVALID_PARAMETER; break; }
            PulseRtspInputConfig cfg{};
            cfg.location   = src.rtsp_url;
            cfg.transport  = PULSE_RTSP_TRANSPORT_TCP;
            cfg.latency_ms = 200;
            PulseRtspSessionID session = 0;
            err = pulse_rtsp_session_connect_input(src.pulse, &cfg, &session);
            if (err == PULSE_SUCCESS) {
                src.rtsp_session = session;
                // Publish the camera's streams onto MAIN so self-view previews it.
                err = pulse_rtsp_session_bind_to_content(src.pulse, session,
                                                         PULSE_MEDIA_CONTENT_MAIN);
            }
            break;
        }

        case SourceKind::Rtmp: {
            src.render_content = PULSE_MEDIA_CONTENT_SELFVIEW;
            PulseRtmpInputConfig cfg{};
            cfg.path           = src.rtmp_path;
            cfg.listening_port = static_cast<uint16_t>(src.rtmp_port);
            cfg.use_tls        = false;
            cfg.support_audio  = true;
            cfg.support_video  = true;
            // Each RTMP source owns its own listener (its own port), so unlike
            // pexninja there is no shared-listener contention to manage.
            err = pulse_rtmp_session_connect_input(src.pulse, PULSE_MEDIA_CONTENT_MAIN, &cfg);
            if (err == PULSE_SUCCESS) src.rtmp_listening = true;
            break;
        }

        case SourceKind::Image:
            src.render_content = PULSE_MEDIA_CONTENT_SELFVIEW;
            if (src.file_path[0] == '\0') { err = PULSE_ERROR_INVALID_PARAMETER; break; }
            err = connect_file_via_mix(src, /*loop=*/false);
            break;

        case SourceKind::Mp4:
            src.render_content = PULSE_MEDIA_CONTENT_SELFVIEW;
            if (src.file_path[0] == '\0') { err = PULSE_ERROR_INVALID_PARAMETER; break; }
            err = connect_file_via_mix(src, /*loop=*/src.loop);
            break;

        case SourceKind::Conference: {
            // The far end is the interesting picture here, so render MAIN.
            src.render_content = PULSE_MEDIA_CONTENT_MAIN;
            std::string conference, server;
            if (!split_conference_id(src.conference_id, conference, server)) {
                err = PULSE_ERROR_INVALID_PARAMETER;
                src.last_error = "conference id must look like name@server";
                break;
            }
            PulseConferenceStatusCallbackConfig conf_cb{ on_conf_status, &src };
            pulse_options_set_conference_state_callback(src.pulse, &conf_cb);

            PulseRestConnectionConfig cfg{};
            cfg.server_address  = server.c_str();
            cfg.conference_name = conference.c_str();
            cfg.display_name    = src.display_name;
            cfg.pin_code        = nullptr;

            PulseAsyncOperationResultCallbackConfig result_cb{ on_conf_result, &src };
            PulseOperationProgressCallbackConfig    progress_cb{ on_conf_progress, &src };
            err = pulse_connect_with_rest_async(src.pulse, &cfg, &result_cb, &progress_cb);
            break;
        }
    }

    if (err != PULSE_SUCCESS) {
        if (src.last_error.empty())
            src.last_error = std::string("start failed: ") + pulse_strerror(err);
        stop_source(src);
        return;
    }

    // Open the RGBA output we will pull frames from, and a GL texture to hold
    // them. Pulse starts feeding the session as soon as media is available.
    glGenTextures(1, &src.texture);
    glBindTexture(GL_TEXTURE_2D, src.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    PulseDataSessionConfig * dcfg = make_video_data_session_config();
    pulse_data_session_connect_output(src.pulse, dcfg, src.render_content);
    pulse_data_session_config_free(dcfg);

    src.started = true;
}

// Tear a source's Pulse instance down and free every resource it acquired.
// Safe to call on a never-started or half-started source.
static void stop_source(Source & src)
{
    if (!src.pulse) {
        src.started = false;
        return;
    }

    // Disconnect the data-session output (mirrors connect_output).
    pulse_data_session_disconnect(src.pulse, PULSE_MEDIA_VIDEO, PULSE_MEDIA_OUTPUT,
                                  src.render_content);

    switch (src.kind) {
        case SourceKind::Camera:
            pulse_device_session_disconnect_main_video(src.pulse, PULSE_MEDIA_CONTENT_MAIN,
                                                       PULSE_MEDIA_INPUT);
            break;
        case SourceKind::Rtsp:
            if (src.rtsp_session != 0) {
                pulse_rtsp_session_disconnect_input(src.pulse, src.rtsp_session);
                src.rtsp_session = 0;
            }
            break;
        case SourceKind::Rtmp:
            if (src.rtmp_listening) {
                pulse_rtmp_session_disconnect_input(src.pulse, PULSE_MEDIA_CONTENT_MAIN);
                src.rtmp_listening = false;
            }
            break;
        case SourceKind::Image:
        case SourceKind::Mp4:
            pulse_video_mix_disconnect(src.pulse, PULSE_MEDIA_CONTENT_MAIN);
            if (src.mix_input != PULSE_VIDEO_MIX_INPUT_ID_NONE) {
                pulse_video_mix_input_release(src.pulse, src.mix_input);
                src.mix_input = PULSE_VIDEO_MIX_INPUT_ID_NONE;
            }
            break;
        case SourceKind::Conference:
            pulse_options_set_conference_state_callback(src.pulse, nullptr);
            if (pulse_is_connected(src.pulse))
                pulse_disconnect(src.pulse, nullptr);
            break;
    }

    pulse_free(src.pulse);
    src.pulse = nullptr;

    if (src.texture) {
        glDeleteTextures(1, &src.texture);
        src.texture = 0;
    }
    src.tex_w = src.tex_h = 0;
    src.conn_status.store(PULSE_CONNECTION_STATUS_DISCONNECTED);
    src.started = false;
}

// ----------------------------------------------------------------------------
//  Canvas rendering
// ----------------------------------------------------------------------------
//
//  We draw a scaled-down view of the whole canvas inside an ImGui child window,
//  paint each source's texture at its (scaled) placement, and let the user drag
//  tiles around. The scale factor maps canvas pixels onto screen pixels.
// ----------------------------------------------------------------------------

static void draw_canvas(AppState & app)
{
    ImGui::BeginChild("canvas", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float scale   = (app.canvas_w > 0) ? (avail_w / static_cast<float>(app.canvas_w)) : 1.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_px(app.canvas_w * scale, app.canvas_h * scale);

    ImDrawList * dl = ImGui::GetWindowDrawList();

    // The canvas backdrop.
    dl->AddRectFilled(origin, ImVec2(origin.x + canvas_px.x, origin.y + canvas_px.y),
                      IM_COL32(20, 20, 24, 255));
    dl->AddRect(origin, ImVec2(origin.x + canvas_px.x, origin.y + canvas_px.y),
                IM_COL32(90, 90, 110, 255));

    // Reserve the canvas area so the child scrolls correctly.
    ImGui::Dummy(canvas_px);

    auto to_screen = [&](float cx, float cy) {
        return ImVec2(origin.x + cx * scale, origin.y + cy * scale);
    };

    for (int i = 0; i < static_cast<int>(app.sources.size()); ++i) {
        Source & src = *app.sources[i];

        pump_frame_into_texture(src);

        const ImVec2 tl = to_screen(src.x, src.y);
        const ImVec2 br = to_screen(src.x + src.w, src.y + src.h);

        if (src.texture && src.tex_w > 0 && src.tex_h > 0) {
            dl->AddImage((ImTextureID)(uintptr_t)src.texture, tl, br);
        } else {
            dl->AddRectFilled(tl, br, IM_COL32(40, 40, 48, 255));
        }

        const bool selected = (i == app.selected);
        dl->AddRect(tl, br, selected ? IM_COL32(80, 180, 250, 255) : IM_COL32(120, 120, 140, 255),
                    0.0f, 0, selected ? 2.0f : 1.0f);

        // A little caption.
        dl->AddText(ImVec2(tl.x + 4, tl.y + 4), IM_COL32(230, 230, 230, 255), src.name);

        // Hit-test / drag handling: an invisible button over the tile lets us
        // select it and drag it around. Later tiles sit on top, matching the
        // draw order, so the topmost tile wins the click.
        ImGui::SetCursorScreenPos(tl);
        ImGui::PushID(i);
        ImGui::InvisibleButton("tile", ImVec2(std::max(br.x - tl.x, 1.0f),
                                              std::max(br.y - tl.y, 1.0f)));
        if (ImGui::IsItemActivated()) app.selected = i;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            src.x += d.x / scale;
            src.y += d.y / scale;
            // Keep the tile from wandering entirely off the canvas.
            if (src.x < 0) src.x = 0;
            if (src.y < 0) src.y = 0;
            if (src.x > app.canvas_w - 1) src.x = static_cast<float>(app.canvas_w - 1);
            if (src.y > app.canvas_h - 1) src.y = static_cast<float>(app.canvas_h - 1);
        }
        ImGui::PopID();
    }

    ImGui::EndChild();
}

// ----------------------------------------------------------------------------
//  Per-kind configuration UI
// ----------------------------------------------------------------------------

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

// Draw the editable fields for whichever source is selected.
static void draw_source_config(AppState & app, Source & src)
{
    ImGui::SeparatorText("Selected source");
    ImGui::InputText("Name", src.name, sizeof(src.name));
    ImGui::Text("Kind: %s", kSourceKindLabels[static_cast<int>(src.kind)]);

    // Editing the wiring after start would desync Pulse, so lock the source-
    // specific fields while it is running.
    ImGui::BeginDisabled(src.started);
    switch (src.kind) {
        case SourceKind::Camera: {
            // Build a combo of camera names plus a "(default)" entry.
            std::string current = src.camera_name[0] ? src.camera_name : "(system default)";
            if (ImGui::BeginCombo("Camera", current.c_str())) {
                if (ImGui::Selectable("(system default)", src.camera_name[0] == '\0'))
                    src.camera_name[0] = '\0';
                for (const std::string & n : app.camera_names) {
                    bool sel = (std::strcmp(n.c_str(), src.camera_name) == 0);
                    if (ImGui::Selectable(n.c_str(), sel))
                        std::snprintf(src.camera_name, sizeof(src.camera_name), "%s", n.c_str());
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Rescan cameras")) refresh_camera_list(app);
            break;
        }
        case SourceKind::Rtsp:
            ImGui::InputText("RTSP URL", src.rtsp_url, sizeof(src.rtsp_url));
            break;
        case SourceKind::Rtmp:
            ImGui::InputInt("Listen port", &src.rtmp_port);
            ImGui::InputText("Path", src.rtmp_path, sizeof(src.rtmp_path));
            ImGui::TextWrapped("Publish to rtmp://<host>:%d/%s", src.rtmp_port, src.rtmp_path);
            break;
        case SourceKind::Image:
            ImGui::InputText("Image path", src.file_path, sizeof(src.file_path));
            break;
        case SourceKind::Mp4:
            ImGui::InputText("MP4 path", src.file_path, sizeof(src.file_path));
            ImGui::Checkbox("Loop", &src.loop);
            break;
        case SourceKind::Conference:
            ImGui::InputText("Conference id", src.conference_id, sizeof(src.conference_id));
            ImGui::InputText("Display name", src.display_name, sizeof(src.display_name));
            ImGui::TextWrapped("e.g. havard@pexipdemo.com -> conference \"havard\" on \"pexipdemo.com\"");
            break;
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::DragFloat2("Position (px)", &src.x, 1.0f);
    ImGui::DragFloat2("Size (px)", &src.w, 1.0f, 16.0f, 100000.0f);

    ImGui::Spacing();
    if (!src.started) {
        if (ImGui::Button("Start")) start_source(src);
    } else {
        if (ImGui::Button("Stop")) stop_source(src);
        if (src.kind == SourceKind::Conference) {
            ImGui::SameLine();
            ImGui::Text("[%s]", status_to_string(src.conn_status.load()));
        }
    }
    if (!src.last_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", src.last_error.c_str());
    }
}

// ----------------------------------------------------------------------------
//  Sources rail (add / list / remove)
// ----------------------------------------------------------------------------

static void draw_sources_rail(AppState & app)
{
    ImGui::SeparatorText("Canvas");
    ImGui::InputInt("Width",  &app.canvas_w);
    ImGui::InputInt("Height", &app.canvas_h);
    if (app.canvas_w < 16)  app.canvas_w = 16;
    if (app.canvas_h < 16)  app.canvas_h = 16;

    ImGui::SeparatorText("Add source");
    ImGui::Combo("##addkind", &app.add_kind, kSourceKindLabels,
                 IM_ARRAYSIZE(kSourceKindLabels));
    ImGui::SameLine();
    if (ImGui::Button("+ Add")) {
        auto src = std::make_unique<Source>();
        src->kind = static_cast<SourceKind>(app.add_kind);
        std::snprintf(src->name, sizeof(src->name), "%s %d",
                      kSourceKindLabels[app.add_kind],
                      static_cast<int>(app.sources.size()) + 1);
        // Stagger new tiles so they don't all land on top of each other.
        const int n = static_cast<int>(app.sources.size());
        src->x = static_cast<float>((n % 4) * 660);
        src->y = static_cast<float>((n / 4) * 380);
        app.sources.push_back(std::move(src));
        app.selected = static_cast<int>(app.sources.size()) - 1;
    }

    ImGui::SeparatorText("Sources");
    for (int i = 0; i < static_cast<int>(app.sources.size()); ++i) {
        Source & src = *app.sources[i];
        ImGui::PushID(i);
        char label[128];
        std::snprintf(label, sizeof(label), "%s%s##sel", src.name, src.started ? " *" : "");
        if (ImGui::Selectable(label, i == app.selected))
            app.selected = i;
        ImGui::PopID();
    }

    if (app.selected >= 0 && app.selected < static_cast<int>(app.sources.size())) {
        Source & src = *app.sources[app.selected];
        ImGui::Spacing();
        draw_source_config(app, src);

        ImGui::Spacing();
        if (ImGui::Button("Remove source")) {
            stop_source(src);
            app.sources.erase(app.sources.begin() + app.selected);
            app.selected = -1;
        }
    }
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
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow * window = glfwCreateWindow(1440, 810, "videowall - Pulse compositor",
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

    // Install the logger before the first pulse_new() so we catch early chatter.
    pulse_global_logger_callback(on_pulse_log, nullptr);

    AppState app;
    // A throwaway Pulse instance whose only job is to enumerate cameras for the
    // UI. Real media always flows through the per-source instances.
    app.enum_pulse = pulse_new();
    if (app.enum_pulse) refresh_camera_list(app);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport * vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("videowall", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        // Left rail: source management. Right: the canvas.
        ImGui::BeginChild("rail", ImVec2(360, 0), true);
        draw_sources_rail(app);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("stage", ImVec2(0, 0), false);
        ImGui::Text("Canvas %d x %d  (drag tiles to move them)", app.canvas_w, app.canvas_h);
        draw_canvas(app);
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Tear every source (and its Pulse instance) down cleanly.
    for (auto & src : app.sources)
        stop_source(*src);
    app.sources.clear();
    if (app.enum_pulse) {
        pulse_free(app.enum_pulse);
        app.enum_pulse = nullptr;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
