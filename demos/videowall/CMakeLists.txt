# ---------------------------------------------------------------------------
# videowall — a multi-instance Pulse compositor. Many Pulse instances (one per
# source) are composited onto a single superwide canvas in our own draw list.
#
# Registered in the root CMakeLists.txt behind the BUILD_VIDEOWALL option.
# Uses the shared helpers from cmake/PulseDemo.cmake.
# ---------------------------------------------------------------------------
add_executable(videowall
    src/main.cpp)

target_link_libraries(videowall PRIVATE
    imgui
    pexip::pulse
    Threads::Threads)

# Find libpexpulse.so at run time, and generate build/run-videowall.sh so the
# demo runs without the user setting LD_LIBRARY_PATH by hand.
pulse_demo_rpath(videowall)
pulse_demo_launcher(videowall run-videowall.sh)
