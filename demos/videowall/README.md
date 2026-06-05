# videowall — many Pulse instances, one giant canvas

A control-room **video wall**: one superwide canvas (3480×1080 by default) onto
which you drop any number of **sources** and lay them out wherever you like —
cameras, RTSP/RTMP streams, still images, mp4 files, and live Pexip
video-conferences.

It is the same idea as the "compositor" mode in
[`pexninja`](../pexninja/), but built the other way round. Where pexninja runs a
**single** Pulse instance and lets Pulse's video mixer composite every source
into one frame, `videowall` runs **one Pulse instance per source** and does the
compositing itself:

```
  ┌── Pulse #1 ──┐   input: camera        selfview ┐
  ┌── Pulse #2 ──┐   input: rtsp://…       selfview ┤
  ┌── Pulse #3 ──┐   input: image.png      selfview ┼─►  we paint each frame
  ┌── Pulse #4 ──┐   input: clip.mp4       selfview ┤    onto the canvas at the
  ┌── Pulse #5 ──┐   dial havard@pex…   MAIN (far) ─┘    source's x/y/w/h
            …                                              (our own draw list)
```

For every *local* source the recipe is uniform: point the Pulse instance's
**input** at the source, then pull its **self-view** back out and upload it to a
GL texture. The one exception is the **video-conference** source: there we split
`havard@pexipdemo.com` into conference `havard` on server `pexipdemo.com`, dial
it over REST, and render the **MAIN** (far-end) video instead of the self-view.

## What it highlights

* One `Pulse *` **per source** — `pulse_new()` / `pulse_free()` many times over,
  the multi-instance pattern from [`gateway`](../gateway/) taken to its limit.
* Driving each instance's input per source kind:
  * `pulse_device_session_connect_device` (camera)
  * `pulse_rtsp_session_connect_input` + `pulse_rtsp_session_bind_to_content` (RTSP)
  * `pulse_rtmp_session_connect_input` (RTMP)
  * `pulse_video_mix_input_from_file` / `…_from_file_with_loop` (image / mp4)
  * `pulse_connect_with_rest_async` (video-conference)
* Pulling frames out with `pulse_data_session_connect_output` +
  `pulse_data_session_pull_frame_data` (self-view for local sources, MAIN for
  the conference) and compositing them ourselves.

## Build & run

From the repository root (install the Pulse runtime first — see the
[repository README](../../README.md#1-install-the-pexip-pulse-runtime)). The
demo builds by default:

```bash
cmake -S . -B build
cmake --build build -j --target videowall
./build/run-videowall.sh
```

(Pass `-DBUILD_VIDEOWALL=OFF` to skip it.)

## Using it

1. Set the canvas size (top-left), or keep the 3480×1080 default.
2. Pick a kind in **Add source** and hit **+ Add**. Configure it (camera,
   URL, file path, or `name@server` conference id) in the panel that appears.
3. Press **Start** — a Pulse instance spins up behind that tile and its video
   appears on the canvas.
4. **Drag** tiles on the canvas to move them, or fine-tune position/size with
   the numeric fields. Add as many sources as you like.

## Code tour

It is all in [`src/main.cpp`](src/main.cpp), heavily commented:

* `Source` — one tile: its kind, placement, config, **its own `Pulse *`**, and
  the GL texture we paint.
* `start_source` / `stop_source` — bring a source's Pulse instance up/down and
  wire its input per kind.
* `connect_camera` / `connect_file_via_mix` / `split_conference_id` — the
  per-kind input plumbing.
* `pump_frame_into_texture` — pull the latest RGBA frame (self-view, or MAIN for
  a conference) into the source's texture.
* `draw_canvas` — scale the canvas to the window and composite every source's
  texture at its placement, with drag-to-move.
