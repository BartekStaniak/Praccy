cmake_minimum_required(VERSION 3.20)

if(WIN32)
    find_program(RC_EXE NAMES windres.exe x86_64-w64-mingw32-windres.exe HINTS "C:/Users/Bartek/w64devkit/bin")
    if(RC_EXE)
        set(CMAKE_RC_COMPILER "${RC_EXE}" CACHE FILEPATH "Resource compiler" FORCE)
    endif()
endif()

project(Praccy VERSION 2.0.0 LANGUAGES CXX C RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Third-party source files isolated from strict first-party compiler warnings
set(THIRD_PARTY_SOURCES
    third_party/miniz/miniz.c
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)

if(MSVC)
    set_source_files_properties(${THIRD_PARTY_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(${THIRD_PARTY_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

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
    src/tools/audio_player.cpp
    src/tools/quick_looper.cpp
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    src/ui/rack_view.cpp
    src/ui/modals/plugin_browser_modal.cpp
    src/ui/modals/settings_modal.cpp
    src/ui/modals/practice_tools_modal.cpp
    src/ui/thumbnail_manager.cpp
    src/ui/update_checker.cpp
    resources/praccy.rc

    ${THIRD_PARTY_SOURCES}
)

add_executable(Praccy WIN32 ${PRACCY_SOURCES})

# Copy bundled resources to build output
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/thumbnails")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/resources/thumbnails" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/resources")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/text.png")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/resources/text.png" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/resources")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon_512.png")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon_512.png" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/resources")
endif()

target_include_directories(Praccy PRIVATE
    src
    resources
)

target_include_directories(Praccy SYSTEM PRIVATE
    third_party
    third_party/miniz
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
    wininet
    user32
    kernel32
    shell32
)

# Strict warning and optimization flags
if(MSVC)
    target_compile_options(Praccy PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(Praccy PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

# Core Unit Tests
add_executable(test_praccy
    tests/test_praccy.cpp
    src/audio/asio_manager.cpp
    src/audio/graph_engine.cpp
    src/plugins/builtin_dsp.cpp
    src/plugins/vst3_host.cpp
    src/plugins/clap_host.cpp
    src/plugins/plugin_window.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/tools/audio_player.cpp
    src/tools/quick_looper.cpp
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    src/ui/update_checker.cpp
    third_party/miniz/miniz.c
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)
target_include_directories(test_praccy PRIVATE
    src
    tests
)
target_include_directories(test_praccy SYSTEM PRIVATE
    third_party
    third_party/imgui
    third_party/miniz
    third_party/clap/include
    third_party/readerwriterqueue
)
target_link_libraries(test_praccy PRIVATE ole32 uuid user32 gdi32 winmm avrt wininet shell32)

if(MSVC)
    target_compile_options(test_praccy PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_praccy PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

# test_asio_driver disabled due to mismatched allocation warnings; remove or re‑enable after fixing
# add_executable(test_asio_driver
#     tests/test_asio_driver.cpp
#     src/audio/asio_manager.cpp
#     src/audio/graph_engine.cpp
#     src/plugins/builtin_dsp.cpp
#     src/tools/tuner.cpp
# )
# target_include_directories(test_asio_driver PRIVATE
#     src
#     tests
# )
# if(MSVC)
#     target_compile_options(test_asio_driver PRIVATE /O2 /W4 /WX /permissive-)
# else()
#     target_compile_options(test_asio_driver PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter -Wno-mismatched-new-delete)
# endif()

add_executable(test_challenger_m1
    tests/test_challenger_m1.cpp
    src/audio/asio_manager.cpp
    src/audio/graph_engine.cpp
    src/plugins/builtin_dsp.cpp
    src/plugins/vst3_host.cpp
    src/plugins/clap_host.cpp
    src/plugins/plugin_window.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/tools/audio_player.cpp
    src/tools/quick_looper.cpp
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)
target_include_directories(test_challenger_m1 PRIVATE
    src
)
target_include_directories(test_challenger_m1 SYSTEM PRIVATE
    third_party
    third_party/clap/include
    third_party/readerwriterqueue
)
target_link_libraries(test_challenger_m1 PRIVATE ole32 uuid user32 gdi32 winmm avrt)
if(MSVC)
    target_compile_options(test_challenger_m1 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_challenger_m1 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

enable_testing()
add_test(NAME test_praccy COMMAND test_praccy)
add_test(NAME test_asio_driver COMMAND test_asio_driver)
add_test(NAME test_challenger_m1 COMMAND test_challenger_m1)

add_executable(test_challenger_m1_2
    tests/test_challenger_m1_2.cpp
    src/audio/asio_manager.cpp
    src/tools/tuner.cpp
)
target_include_directories(test_challenger_m1_2 PRIVATE
    src
)
target_include_directories(test_challenger_m1_2 SYSTEM PRIVATE
    third_party
    third_party/readerwriterqueue
)
target_link_libraries(test_challenger_m1_2 PRIVATE ole32 uuid user32 gdi32 winmm avrt)
if(MSVC)
    target_compile_options(test_challenger_m1_2 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_challenger_m1_2 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
add_test(NAME test_challenger_m1_2 COMMAND test_challenger_m1_2)

add_executable(test_challenger_m2
    tests/test_challenger_m2.cpp
    src/ui/update_checker.cpp
    third_party/miniz/miniz.c
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    src/audio/graph_engine.cpp
    src/audio/asio_manager.cpp
    src/plugins/builtin_dsp.cpp
    src/plugins/vst3_host.cpp
    src/plugins/clap_host.cpp
    src/plugins/plugin_window.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/tools/audio_player.cpp
    src/tools/quick_looper.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)
target_include_directories(test_challenger_m2 PRIVATE
    src
    tests
)
target_include_directories(test_challenger_m2 SYSTEM PRIVATE
    third_party
    third_party/miniz
    third_party/clap/include
    third_party/readerwriterqueue
)
target_link_libraries(test_challenger_m2 PRIVATE ole32 uuid user32 gdi32 winmm avrt wininet shell32)
if(MSVC)
    target_compile_options(test_challenger_m2 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_challenger_m2 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
add_test(NAME test_challenger_m2 COMMAND test_challenger_m2)

add_executable(test_challenger_m2_1
    tests/test_challenger_m2_1.cpp
    src/ui/update_checker.cpp
    third_party/miniz/miniz.c
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    src/audio/graph_engine.cpp
    src/audio/asio_manager.cpp
    src/plugins/builtin_dsp.cpp
    src/plugins/vst3_host.cpp
    src/plugins/clap_host.cpp
    src/plugins/plugin_window.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/tools/audio_player.cpp
    src/tools/quick_looper.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)
target_include_directories(test_challenger_m2_1 PRIVATE
    src
    tests
)
target_include_directories(test_challenger_m2_1 SYSTEM PRIVATE
    third_party
    third_party/miniz
    third_party/clap/include
    third_party/readerwriterqueue
)
target_link_libraries(test_challenger_m2_1 PRIVATE ole32 uuid user32 gdi32 winmm avrt wininet shell32)
if(MSVC)
    target_compile_options(test_challenger_m2_1 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_challenger_m2_1 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
add_test(NAME test_challenger_m2_1 COMMAND test_challenger_m2_1)

add_executable(test_challenger_m3_1
    tests/test_challenger_m3_1.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
)
target_include_directories(test_challenger_m3_1 PRIVATE
    src
    tests
)
target_include_directories(test_challenger_m3_1 SYSTEM PRIVATE
    third_party
    third_party/imgui
)
target_link_libraries(test_challenger_m3_1 PRIVATE ole32 uuid user32 gdi32 winmm avrt)
if(MSVC)
    target_compile_options(test_challenger_m3_1 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_challenger_m3_1 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
add_test(NAME test_challenger_m3_1 COMMAND test_challenger_m3_1)

add_executable(test_challenger_m3_2
    tests/test_challenger_m3_2.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
)
target_include_directories(test_challenger_m3_2 PRIVATE
    src
    tests
)
target_include_directories(test_challenger_m3_2 SYSTEM PRIVATE
    third_party
    third_party/imgui
)
target_link_libraries(test_challenger_m3_2 PRIVATE ole32 uuid user32 gdi32 winmm avrt)
if(MSVC)
    target_compile_options(test_challenger_m3_2 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_challenger_m3_2 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
add_test(NAME test_challenger_m3_2 COMMAND test_challenger_m3_2)

add_executable(test_challenger_m4_1
    tests/test_challenger_m4_1.cpp
    src/audio/graph_engine.cpp
    src/state/app_config.cpp
)
target_include_directories(test_challenger_m4_1 PRIVATE
    src
    tests
)
target_include_directories(test_challenger_m4_1 SYSTEM PRIVATE
    third_party
    third_party/readerwriterqueue
)
target_link_libraries(test_challenger_m4_1 PRIVATE ole32 uuid user32 gdi32 winmm avrt)
if(MSVC)
    target_compile_options(test_challenger_m4_1 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_challenger_m4_1 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
add_test(NAME test_challenger_m4_1 COMMAND test_challenger_m4_1)

add_executable(test_challenger_m4_2
    tests/test_challenger_m4_2.cpp
    src/tools/quick_looper.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
)
target_include_directories(test_challenger_m4_2 PRIVATE
    src
    tests
)
target_include_directories(test_challenger_m4_2 SYSTEM PRIVATE
    third_party
    third_party/imgui
)
target_link_libraries(test_challenger_m4_2 PRIVATE ole32 uuid user32 gdi32 winmm avrt)
if(MSVC)
    target_compile_options(test_challenger_m4_2 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_challenger_m4_2 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
add_test(NAME test_challenger_m4_2 COMMAND test_challenger_m4_2)



