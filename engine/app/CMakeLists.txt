add_executable(framecapture-engine main.cpp)

target_link_libraries(framecapture-engine PRIVATE FrameCapture::core)

set_target_properties(framecapture-engine PROPERTIES FOLDER "engine")

# vcpkg's applocal deployment copies the FFmpeg DLLs next to the exe on install;
# for the build tree it hooks POST_BUILD automatically when the toolchain is in
# use. Nothing to do here.
