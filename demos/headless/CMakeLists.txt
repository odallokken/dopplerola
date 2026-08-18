# ---------------------------------------------------------------------------
# headless — an unattended Pulse room client for a headless Raspberry Pi.
#
# Deliberately links *no* GUI libraries (no GLFW, no OpenGL, no Dear ImGui):
# the target box is an Ubuntu Server install with no display, so the only
# dependencies are Pulse itself and pthreads.
#
# Registered in the root CMakeLists.txt behind the BUILD_HEADLESS option.
# ---------------------------------------------------------------------------
add_executable(pulse_headless
    src/main.cpp
    src/config.cpp)

target_link_libraries(pulse_headless PRIVATE
    pexip::pulse
    Threads::Threads)

# Find libpexpulse.so at run time, and generate build/run-headless.sh so the
# client runs without the user setting LD_LIBRARY_PATH by hand.
pulse_demo_rpath(pulse_headless)
pulse_demo_launcher(pulse_headless run-headless.sh)
