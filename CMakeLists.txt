cmake_minimum_required(VERSION 3.20)

if(WIN32)
    find_program(RC_EXE NAMES windres.exe x86_64-w64-mingw32-windres.exe)
    if(RC_EXE)
        set(CMAKE_RC_COMPILER "${RC_EXE}" CACHE FILEPATH "Resource compiler" FORCE)
    endif()
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" _PRACCY_VER_H)
string(REGEX MATCH "#define PRACCY_VERSION_MAJOR ([0-9]+)" _ "${_PRACCY_VER_H}")
set(PRACCY_VER_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define PRACCY_VERSION_MINOR ([0-9]+)" _ "${_PRACCY_VER_H}")
set(PRACCY_VER_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define PRACCY_VERSION_PATCH ([0-9]+)" _ "${_PRACCY_VER_H}")
set(PRACCY_VER_PATCH ${CMAKE_MATCH_1})

# Version: 2.7-beta3
set(PRACCY_VERSION_STRING "2.7-beta3")

project(Praccy VERSION ${PRACCY_VER_MAJOR}.${PRACCY_VER_MINOR}.${PRACCY_VER_PATCH} LANGUAGES CXX C RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Ensure <pluginterfaces/...> resolves cleanly from third_party/vst3_pluginterfaces
if(NOT EXISTS "${CMAKE_BINARY_DIR}/vst3_sdk/pluginterfaces")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/vst3_sdk")
    file(CREATE_LINK
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3_pluginterfaces"
        "${CMAKE_BINARY_DIR}/vst3_sdk/pluginterfaces"
        COPY_ON_ERROR SYMBOLIC
    )
endif()
include_directories(SYSTEM "${CMAKE_BINARY_DIR}/vst3_sdk")

# Dynamic Sanitizers
option(PRACCY_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PRACCY_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(PRACCY_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(PRACCY_ENABLE_RTSAN "Enable RealTimeSanitizer" OFF)
option(PRACCY_SANITIZER "Enable dynamic sanitizer (address, thread, undefined, realtime)" "")

if(PRACCY_ENABLE_ASAN_UBSAN OR PRACCY_SANITIZER STREQUAL "address")
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()
if(PRACCY_ENABLE_UBSAN OR PRACCY_SANITIZER STREQUAL "undefined")
    if(NOT MSVC)
        add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined)
    endif()
endif()
if(PRACCY_ENABLE_TSAN OR PRACCY_SANITIZER STREQUAL "thread")
    if(NOT MSVC)
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()
if(PRACCY_ENABLE_RTSAN OR PRACCY_SANITIZER STREQUAL "realtime")
    if(NOT MSVC)
        add_compile_options(-fsanitize=realtime -fno-omit-frame-pointer)
        add_link_options(-fsanitize=realtime)
    endif()
endif()

# Strict compiler warning and optimization flags
if(MSVC)
    set(PRACCY_STRICT_FLAGS /O2 /W4 /permissive-)
else()
    set(PRACCY_STRICT_FLAGS -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>)
endif()

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

# ============================================================================
# Core Static Library (praccy_core)
# ============================================================================
set(PRACCY_CORE_SOURCES
    src/audio/asio_manager.cpp
    src/audio/graph_engine.cpp
    src/plugins/clap_host.cpp
    src/plugins/async_plugin_loader.cpp
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
    src/state/undo_manager.cpp
    src/ui/rack_view.cpp
    src/ui/modals/plugin_browser_modal.cpp
    src/ui/modals/settings_modal.cpp
    src/ui/modals/practice_tools_modal.cpp
    src/ui/modals/tuner_settings_modal.cpp
    src/ui/thumbnail_manager.cpp
    src/ui/update_checker.cpp
    src/ui/drag_drop_router.cpp
    src/ui/modals/export_daw_chain_modal.cpp
    src/utils/archive_utils.cpp
    src/utils/xml_parser.cpp
    src/state/daw_interop/reaper_adapter.cpp
    src/state/daw_interop/ableton_adapter.cpp
    src/state/daw_interop/dawproject_adapter.cpp
    src/state/daw_interop/vstpreset_adapter.cpp
    src/state/daw_interop/universal_daw_interop_manager.cpp
    ${THIRD_PARTY_SOURCES}
)

add_library(praccy_core STATIC ${PRACCY_CORE_SOURCES})

target_include_directories(praccy_core PUBLIC
    src
)

target_include_directories(praccy_core SYSTEM PUBLIC
    third_party
    third_party/miniz
    third_party/imgui
    third_party/imgui/backends
    third_party/clap/include
    third_party/readerwriterqueue
)

target_link_libraries(praccy_core PUBLIC
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

target_compile_options(praccy_core PRIVATE ${PRACCY_STRICT_FLAGS})

# ============================================================================
# Main Application Target (Praccy)
# ============================================================================
add_executable(Praccy WIN32
    src/main.cpp
    resources/praccy.rc
)

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
    resources
)

target_link_libraries(Praccy PRIVATE
    praccy_core
)

target_compile_options(Praccy PRIVATE ${PRACCY_STRICT_FLAGS})

# ============================================================================
# Test Targets (17 test targets)
# ============================================================================
enable_testing()

# 1. Core Unit Tests
add_executable(test_praccy tests/test_praccy.cpp)
target_include_directories(test_praccy PRIVATE tests)
target_link_libraries(test_praccy PRIVATE praccy_core)
target_compile_options(test_praccy PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_praccy COMMAND test_praccy)

# 2. ASIO Driver Tests
add_executable(test_asio_driver tests/test_asio_driver.cpp)
target_include_directories(test_asio_driver PRIVATE tests)
target_link_libraries(test_asio_driver PRIVATE praccy_core)
target_compile_options(test_asio_driver PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_asio_driver COMMAND test_asio_driver)

# 3. Challenger M1 Tests
add_executable(test_challenger_m1 tests/test_challenger_m1.cpp)
target_include_directories(test_challenger_m1 PRIVATE tests)
target_link_libraries(test_challenger_m1 PRIVATE praccy_core)
target_compile_options(test_challenger_m1 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_m1 COMMAND test_challenger_m1)

# 4. Challenger M1.2 Tests
add_executable(test_challenger_m1_2 tests/test_challenger_m1_2.cpp)
target_include_directories(test_challenger_m1_2 PRIVATE tests)
target_link_libraries(test_challenger_m1_2 PRIVATE praccy_core)
target_compile_options(test_challenger_m1_2 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_m1_2 COMMAND test_challenger_m1_2)

# 5. Challenger M2 Tests
add_executable(test_challenger_m2 tests/test_challenger_m2.cpp)
target_include_directories(test_challenger_m2 PRIVATE tests)
target_link_libraries(test_challenger_m2 PRIVATE praccy_core)
target_compile_options(test_challenger_m2 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_m2 COMMAND test_challenger_m2)

# 6. Challenger M2.1 Tests
add_executable(test_challenger_m2_1 tests/test_challenger_m2_1.cpp)
target_include_directories(test_challenger_m2_1 PRIVATE tests)
target_link_libraries(test_challenger_m2_1 PRIVATE praccy_core)
target_compile_options(test_challenger_m2_1 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_m2_1 COMMAND test_challenger_m2_1)

# 7. Challenger M2.2 Tests
add_executable(test_challenger_m2_2 tests/test_challenger_m2_2.cpp)
target_include_directories(test_challenger_m2_2 PRIVATE tests)
target_link_libraries(test_challenger_m2_2 PRIVATE praccy_core)
target_compile_options(test_challenger_m2_2 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_m2_2 COMMAND test_challenger_m2_2)

# 8. Challenger M3.1 Tests
add_executable(test_challenger_m3_1 tests/test_challenger_m3_1.cpp)
target_include_directories(test_challenger_m3_1 PRIVATE tests)
target_link_libraries(test_challenger_m3_1 PRIVATE praccy_core)
target_compile_options(test_challenger_m3_1 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_m3_1 COMMAND test_challenger_m3_1)

# 9. Challenger M3.2 Tests
add_executable(test_challenger_m3_2 tests/test_challenger_m3_2.cpp)
target_include_directories(test_challenger_m3_2 PRIVATE tests)
target_link_libraries(test_challenger_m3_2 PRIVATE praccy_core)
target_compile_options(test_challenger_m3_2 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_m3_2 COMMAND test_challenger_m3_2)

# 10. Challenger M4.1 Tests
add_executable(test_challenger_m4_1 tests/test_challenger_m4_1.cpp)
target_include_directories(test_challenger_m4_1 PRIVATE tests)
target_link_libraries(test_challenger_m4_1 PRIVATE praccy_core)
target_compile_options(test_challenger_m4_1 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_m4_1 COMMAND test_challenger_m4_1)

# 11. Challenger M4.2 Tests
add_executable(test_challenger_m4_2 tests/test_challenger_m4_2.cpp)
target_include_directories(test_challenger_m4_2 PRIVATE tests)
target_link_libraries(test_challenger_m4_2 PRIVATE praccy_core)
target_compile_options(test_challenger_m4_2 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_m4_2 COMMAND test_challenger_m4_2)

# 12. Praccy v2.1 Tests
add_executable(test_praccy_v2_1 tests/test_praccy_v2_1.cpp)
target_include_directories(test_praccy_v2_1 PRIVATE tests)
target_link_libraries(test_praccy_v2_1 PRIVATE praccy_core)
target_compile_options(test_praccy_v2_1 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_praccy_v2_1 COMMAND test_praccy_v2_1)

# 13. Interop Fuzz Tests
add_executable(test_interop_fuzz tests/test_interop_fuzz.cpp)
target_include_directories(test_interop_fuzz PRIVATE tests)
target_link_libraries(test_interop_fuzz PRIVATE praccy_core)
target_compile_options(test_interop_fuzz PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_interop_fuzz COMMAND test_interop_fuzz)

# 14. Interop Roundtrip Tests
add_executable(test_interop_roundtrip tests/test_interop_roundtrip.cpp)
target_include_directories(test_interop_roundtrip PRIVATE tests)
target_link_libraries(test_interop_roundtrip PRIVATE praccy_core)
target_compile_options(test_interop_roundtrip PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_interop_roundtrip COMMAND test_interop_roundtrip --all)

# 15. Challenger Stage 1.2 Tests
add_executable(test_challenger_stage1_2 tests/test_challenger_stage1_2.cpp)
target_include_directories(test_challenger_stage1_2 PRIVATE tests)
target_link_libraries(test_challenger_stage1_2 PRIVATE praccy_core)
target_compile_options(test_challenger_stage1_2 PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_stage1_2 COMMAND test_challenger_stage1_2)

# 16. Challenger Stage 1 Audio Tests
add_executable(test_challenger_stage1_audio tests/test_challenger_stage1_audio.cpp)
target_include_directories(test_challenger_stage1_audio PRIVATE tests)
target_link_libraries(test_challenger_stage1_audio PRIVATE praccy_core)
target_compile_options(test_challenger_stage1_audio PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_challenger_stage1_audio COMMAND test_challenger_stage1_audio)

# 17. Remediation Characterization Tests
add_executable(test_remediation_characterization tests/test_remediation_characterization.cpp)
target_include_directories(test_remediation_characterization PRIVATE tests)
target_link_libraries(test_remediation_characterization PRIVATE praccy_core)
target_compile_options(test_remediation_characterization PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_remediation_characterization COMMAND test_remediation_characterization --verify-fixed)

# 18. Async Plugin Loading Tests
add_executable(test_async_plugin_loading tests/test_async_plugin_loading.cpp)
target_include_directories(test_async_plugin_loading PRIVATE tests)
target_link_libraries(test_async_plugin_loading PRIVATE praccy_core)
target_compile_options(test_async_plugin_loading PRIVATE ${PRACCY_STRICT_FLAGS})
add_test(NAME test_async_plugin_loading COMMAND test_async_plugin_loading)

# 19. Audit Remediation Regression Tests
add_executable(test_audit_remediation tests/test_audit_remediation.cpp)
target_include_directories(test_audit_remediation PRIVATE tests)
target_link_libraries(test_audit_remediation PRIVATE praccy_core)
target_compile_options(test_audit_remediation PRIVATE ${PRACCY_STRICT_FLAGS})

add_test(NAME audit_regression_rt COMMAND test_audit_remediation --suite=rt)

add_test(NAME audit_regression_mem COMMAND test_audit_remediation --suite=mem)

add_test(NAME audit_regression_host COMMAND test_audit_remediation --suite=host)

add_test(NAME audit_regression_ui COMMAND test_audit_remediation --suite=ui)
set_tests_properties(audit_regression_ui PROPERTIES WILL_FAIL TRUE)
