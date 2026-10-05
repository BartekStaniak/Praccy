cmake_minimum_required(VERSION 3.20)
project(Praccy VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source files
set(PRACCY_SOURCES
    src/main.cpp
    src/audio/asio_manager.cpp
    src/audio/graph_engine.cpp
    src/plugins/clap_host.cpp
    src/plugins/builtin_dsp.cpp
    src/midi/midi_manager.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/state/scene_manager.cpp
    src/ui/rack_view.cpp

    # Dear ImGui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)

add_executable(Praccy WIN32 ${PRACCY_SOURCES})

target_include_directories(Praccy PRIVATE
    src
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
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/state/scene_manager.cpp
)

add_executable(test_asio_driver
    tests/test_asio_driver.cpp
    src/audio/asio_manager.cpp
)
target_include_directories(test_asio_driver PRIVATE src)
target_link_libraries(test_asio_driver PRIVATE ole32 uuid winmm avrt)
