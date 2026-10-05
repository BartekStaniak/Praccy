cmake_minimum_required(VERSION 3.20)

if(WIN32)
    find_program(RC_EXE NAMES windres.exe x86_64-w64-mingw32-windres.exe HINTS "C:/Users/Bartek/w64devkit/bin")
    if(RC_EXE)
        set(CMAKE_RC_COMPILER "${RC_EXE}" CACHE FILEPATH "Resource compiler" FORCE)
    endif()
endif()

project(Praccy VERSION 1.0.0 LANGUAGES CXX C RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source files
set(PRACCY_SOURCES
    src/main.cpp
    src/audio/asio_manager.cpp
    src/audio/graph_engine.cpp
    src/plugins/clap_host.cpp
    src/plugins/vst3_host.cpp
    src/plugins/plugin_window.cpp
    src/plugins/builtin_dsp.cpp
    src/plugins/plugin_scanner.cpp
    src/midi/midi_manager.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    src/ui/rack_view.cpp
    src/ui/thumbnail_manager.cpp
    resources/praccy.rc

    # Dear ImGui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp

    # VST3 Pluginterfaces Base
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)

add_executable(Praccy WIN32 ${PRACCY_SOURCES})

# Copy bundled resources to build output
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/thumbnails")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/resources/thumbnails" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/resources")
endif()

target_include_directories(Praccy PRIVATE
    src
    resources
    third_party
    third_party/imgui
    third_party/imgui/backends
    third_party/clap/include
    third_party/readerwriterqueue
)

target_link_libraries(Praccy PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dwmapi
    winmm
    avrt
    ole32
    uuid
    gdi32
    gdiplus
    user32
    kernel32
)

# Optimization flags
if(MSVC)
    target_compile_options(Praccy PRIVATE /O2 /W4 /permissive-)
else()
    target_compile_options(Praccy PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)
endif()

# Core Unit Tests
add_executable(test_praccy
    tests/test_praccy.cpp
    src/audio/graph_engine.cpp
    src/plugins/builtin_dsp.cpp
    src/plugins/vst3_host.cpp
    src/plugins/clap_host.cpp
    src/plugins/plugin_window.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)
target_include_directories(test_praccy PRIVATE
    src
    third_party
    third_party/clap/include
)
target_link_libraries(test_praccy PRIVATE ole32 uuid user32 gdi32)


add_executable(test_asio_driver
    tests/test_asio_driver.cpp
    src/audio/asio_manager.cpp
)
target_include_directories(test_asio_driver PRIVATE src)
target_link_libraries(test_asio_driver PRIVATE ole32 uuid winmm avrt)

enable_testing()
add_test(NAME test_praccy COMMAND test_praccy)
