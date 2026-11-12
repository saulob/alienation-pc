cmake_minimum_required(VERSION 3.24)
project(alienation_pc VERSION 0.1.0 LANGUAGES C CXX)
set(ALN_PORT_VERSION "${PROJECT_VERSION}")

if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Build with Clang: MSYS2 CLANG64 (tools\\build.ps1) on Windows, tools/build.sh on Linux.")
endif()
if (NOT WIN32 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"))
    message(FATAL_ERROR "Supported targets: Windows x64 and Linux x86_64.")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if (WIN32)
    set(CMAKE_C_EXTENSIONS OFF)
else()
    set(CMAKE_C_EXTENSIONS ON) # POSIX interfaces (sigjmp_buf, memfd_create) are hidden in strict ISO mode
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Repository paths in __FILE__ and std::source_location stay out of the binary; debug info keeps them.
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=>")
if (NOT WIN32)
    # Non-PIE: the heap handed to the guest (thread handles, TLS) must stay below 1 TiB (loader.c).
    set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fno-pie>")
    add_link_options(-no-pie)
endif()

if (NOT PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "project VERSION must start with MAJOR.MINOR.PATCH")
endif()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/aln_version.h CONTENT "#define ALN_VERSION_NUMBERS \
${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0\n#define ALN_FILE_VERSION_STRING \
\"${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.0\"\n#define ALN_VERSION_STRING \"${PROJECT_VERSION}\"\n")

find_package(SDL3 CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)

# Keyboard bindings shared by the launcher, the runtime pad and the video core.
add_library(aln_keys STATIC src/aln_keys.c)
target_include_directories(aln_keys PUBLIC src)
target_link_libraries(aln_keys PUBLIC SDL3::SDL3)
target_compile_options(aln_keys PRIVATE -Wall -Wextra -Werror)

# The online layer of the launcher's Online page and the game's online play (identity, lobby link, STUN, UPnP, peer
# transport); nothing in it runs unless Online play is on.
add_library(aln_net STATIC src/net/addr.cpp src/net/connection_test.cpp src/net/crypto.cpp src/net/identity.cpp
    src/net/json.cpp src/net/lobby_link.cpp src/net/online_config.cpp src/net/online_version.cpp src/net/peer_socket.cpp
    src/net/relay_frame.cpp src/net/stun.cpp src/net/upnp.cpp src/net/websocket.cpp
    $<IF:$<BOOL:${WIN32}>,src/net/websocket_win.cpp,src/net/websocket_posix.cpp>)
target_include_directories(aln_net PUBLIC src)
target_compile_definitions(aln_net PRIVATE ALN_PORT_VERSION="${ALN_PORT_VERSION}")
set_target_properties(aln_net PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln_net PRIVATE -Wall -Wextra)
if (WIN32)
    target_link_libraries(aln_net PUBLIC winhttp ws2_32 bcrypt)
else()
    find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
    target_link_libraries(aln_net PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
endif()

# The stage the process is in, for the crash report: used by the launcher, the loader and the window thread.
add_library(aln_startup_phase STATIC src/startup_phase.c)
target_include_directories(aln_startup_phase PUBLIC src)
target_compile_options(aln_startup_phase PRIVATE -Wall -Wextra -Werror)

# The port's own text, shared by the launcher and the in-game menus.
add_library(aln_ui_text STATIC launcher/localization/ui_text.cpp)
target_include_directories(aln_ui_text PUBLIC launcher src)
set_target_properties(aln_ui_text PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_subdirectory(gpu)
add_subdirectory(launcher)
find_package(ZLIB REQUIRED)

set(ALN_RUNTIME_SOURCES
    src/boot_diagnostic.c
    src/exit_game.c
    src/guest_hook.c
    src/runtime.c
    src/runtime_ajm.c
    src/runtime_json.cpp
    src/runtime_aln.c
    src/runtime_audio.c
    src/runtime_audio3d.c
    src/runtime_content.c
    src/runtime_eventflag.c
    src/runtime_fiber.c
    src/runtime_file.c
    src/runtime_font.c
    src/runtime_kernel.c
    src/runtime_matching2.cpp
    src/runtime_memory.c
    src/runtime_mutex.c
    src/runtime_np.c
    src/runtime_online.cpp
    src/runtime_p2p.cpp
    src/runtime_pad.c
    src/runtime_rtc.c
    src/runtime_rwlock.c
    src/runtime_savedata.c
    src/runtime_session_log.c
    src/runtime_sema.c
    src/runtime_services.c
    src/runtime_thread.c
    src/runtime_time.c
    src/runtime_users.c
    src/runtime_webapi.cpp
    src/online_status.c
    src/skip_intro.c)
# The built-in system fonts are assembled into runtime_font.c (.incbin): rebuild it when a font changes.
set(ALN_NOTO_SANS ${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSans-Regular.ttf)
set(ALN_NOTO_SANS_TC ${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSansTC-Regular.otf)
set_source_files_properties(src/runtime_font.c PROPERTIES
    COMPILE_DEFINITIONS "ALN_NOTO_SANS_PATH=\"${ALN_NOTO_SANS}\";ALN_NOTO_SANS_TC_PATH=\"${ALN_NOTO_SANS_TC}\""
    OBJECT_DEPENDS "${ALN_NOTO_SANS};${ALN_NOTO_SANS_TC}")

if (WIN32)
    enable_language(RC)
    # Focused tests of the sparse guest memory backing (tools/test.ps1); not part of the default build.
    add_executable(aln-memory-windows-tests EXCLUDE_FROM_ALL tests/memory_windows_tests.c)
    target_include_directories(aln-memory-windows-tests PRIVATE src)
    target_compile_definitions(aln-memory-windows-tests PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_compile_options(aln-memory-windows-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(aln-memory-windows-tests PRIVATE onecore -pthread)
    target_link_options(aln-memory-windows-tests PRIVATE -Wl,--disable-high-entropy-va)
    # Relink when the application icon, the manifest or the version changes.
    set_source_files_properties(src/alienation.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/packaging/icons/app-icon.ico;${CMAKE_SOURCE_DIR}/src/alienation.manifest;${CMAKE_BINARY_DIR}/generated/aln_version.h")
    set(ALN_PLATFORM_SOURCES src/compat_win.c src/crash_guard.c src/module_diagnostics.c src/module_history.c src/call_site.c src/runtime_log.c src/alienation.rc)
else()
    set(ALN_PLATFORM_SOURCES src/runtime_log_posix.c)
endif()
set(ALN_MAIN_SOURCES src/loader.c src/isa_emulate.c ${ALN_RUNTIME_SOURCES} ${ALN_PLATFORM_SOURCES})
if (WIN32)
    add_executable(alienation WIN32 ${ALN_MAIN_SOURCES})
else()
    add_executable(alienation ${ALN_MAIN_SOURCES})
endif()
set_target_properties(alienation PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(alienation PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
target_compile_options(alienation PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
target_compile_options(alienation PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
if (WIN32)
    # Windows 10 1903+ APIs (VirtualAlloc2, MapViewOfFile3).
    target_compile_definitions(alienation PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    # The PS4 user range (64 GiB - 1 TiB) must stay free of host allocations: no high-entropy ASLR.
    # The guest main thread runs on the process stack: 16 MiB, committed (guest code has no stack probes).
    target_link_options(alienation PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    target_link_libraries(alienation PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB onecore bcrypt ws2_32 -pthread)
else()
    # -rdynamic: fault reports name host symbols (dladdr).
    target_link_options(alienation PRIVATE -rdynamic)
    target_link_libraries(alienation PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# The launcher reads its images next to the executable.
add_custom_command(TARGET alienation POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:alienation>/assets/launcher"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/sponsor-heart.png"
        "$<TARGET_FILE_DIR:alienation>/assets/launcher/sponsor-heart.png"
    VERBATIM)

if (WIN32)
    # GUI-subsystem processes start with closed standard streams; the log must still work. Not part of the default build.
    add_executable(aln-log-tests EXCLUDE_FROM_ALL tests/log_tests.c src/runtime_log.c src/runtime_session_log.c)
    target_include_directories(aln-log-tests PRIVATE src)
    target_compile_options(aln-log-tests PRIVATE -Wall -Wextra -Werror)
endif()

# The pure parts of the crash report: module history, call site decoding, fault ownership and the startup phase
# (tools/test.ps1); not part of the default build.
add_executable(aln-module-history-tests EXCLUDE_FROM_ALL tests/module_history_tests.c src/module_history.c src/call_site.c src/startup_phase.c)
target_include_directories(aln-module-history-tests PRIVATE src)
target_compile_options(aln-module-history-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the boot image and content profile contract between the producers and the loader (tools/test.ps1);
# not part of the default build.
add_executable(aln-image-format-tests EXCLUDE_FROM_ALL tests/image_format_tests.c)
target_include_directories(aln-image-format-tests PRIVATE src)
target_compile_definitions(aln-image-format-tests PRIVATE ALN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(aln-image-format-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the render resolution and frame rate patches (tools/test.ps1); not part of the default build.
add_executable(aln-resolution-patch-tests EXCLUDE_FROM_ALL tests/resolution_patch_tests.c launcher/resolution.c launcher/frame_rate.c)
target_include_directories(aln-resolution-patch-tests PRIVATE launcher)
target_compile_options(aln-resolution-patch-tests PRIVATE -Wall -Wextra -Werror)

# The real Options and Select Campaign code of each prepared build, run before and after the Exit Game patch
# (tools/test.ps1); not part of the default build.
add_executable(aln-exit-game-menu-tests EXCLUDE_FROM_ALL tests/exit_game_menu_tests.c launcher/exit_game_script.c)
target_include_directories(aln-exit-game-menu-tests PRIVATE launcher)
target_compile_definitions(aln-exit-game-menu-tests PRIVATE ALN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_options(aln-exit-game-menu-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(aln-exit-game-menu-tests PRIVATE ZLIB::ZLIB)

add_executable(aln-text-fix-tests EXCLUDE_FROM_ALL tests/text_fix_tests.c launcher/text_fix.c)
target_include_directories(aln-text-fix-tests PRIVATE launcher)
target_compile_definitions(aln-text-fix-tests PRIVATE ALN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_options(aln-text-fix-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(aln-text-fix-tests PRIVATE ZLIB::ZLIB)

# Focused tests of Asian Menu Buttons, the menus' Confirm and Cancel per build (tools/test.ps1); not part of the default build.
add_executable(aln-menu-buttons-tests EXCLUDE_FROM_ALL tests/menu_buttons_tests.c launcher/menu_buttons.c)
target_include_directories(aln-menu-buttons-tests PRIVATE launcher)
target_compile_definitions(aln-menu-buttons-tests PRIVATE ALN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_options(aln-menu-buttons-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the Skip Intro hook site checks and the startup video tracking (tools/test.ps1); not part of the default build.
add_executable(aln-skip-intro-tests EXCLUDE_FROM_ALL tests/skip_intro_tests.c src/skip_intro.c src/guest_hook.c)
target_include_directories(aln-skip-intro-tests PRIVATE src)
target_compile_options(aln-skip-intro-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the Exit Game bytecode patch, hook site checks and row text (tools/test.ps1, tools/build.sh --test);
# not part of the default build.
add_executable(aln-exit-game-tests EXCLUDE_FROM_ALL tests/exit_game_tests.c src/exit_game.c src/guest_hook.c
    launcher/exit_game_script.c)
target_include_directories(aln-exit-game-tests PRIVATE src launcher)
target_compile_definitions(aln-exit-game-tests PRIVATE ALN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_options(aln-exit-game-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the Windows crash reporting: each case crashes an isolated child process through the production
# handlers and checks the log, the minidump and the exit code (tools/test.ps1); not part of the default build.
if (WIN32)
    add_executable(aln-crash-guard-tests EXCLUDE_FROM_ALL tests/crash_guard_tests.c src/crash_guard.c src/runtime_log.c
        src/runtime_session_log.c src/startup_phase.c)
    target_include_directories(aln-crash-guard-tests PRIVATE src)
    target_compile_options(aln-crash-guard-tests PRIVATE -Wall -Wextra -Werror -fno-omit-frame-pointer)
endif()

# Focused tests of libSceRtc through the import resolver (tools/test.ps1, tools/build.sh --test); not part of the default
# build.
add_executable(aln-rtc-tests EXCLUDE_FROM_ALL tests/rtc_tests.c ${ALN_RUNTIME_SOURCES}
    $<$<BOOL:${WIN32}>:src/compat_win.c src/runtime_log.c> $<$<NOT:$<BOOL:${WIN32}>>:src/runtime_log_posix.c>)
target_include_directories(aln-rtc-tests PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
set_target_properties(aln-rtc-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln-rtc-tests PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
if (WIN32)
    target_compile_definitions(aln-rtc-tests PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_link_libraries(aln-rtc-tests PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB onecore bcrypt ws2_32 -pthread)
else()
    target_link_libraries(aln-rtc-tests PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# Focused tests of libSceIme keyboard functions through the import resolver (tools/test.ps1, tools/build.sh --test); not part of the default
# build.
add_executable(aln-ime-keyboard-tests EXCLUDE_FROM_ALL tests/ime_keyboard_tests.c ${ALN_RUNTIME_SOURCES}
    $<$<BOOL:${WIN32}>:src/compat_win.c src/runtime_log.c> $<$<NOT:$<BOOL:${WIN32}>>:src/runtime_log_posix.c>)
target_include_directories(aln-ime-keyboard-tests PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
set_target_properties(aln-ime-keyboard-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln-ime-keyboard-tests PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
if (WIN32)
    target_compile_definitions(aln-ime-keyboard-tests PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_link_libraries(aln-ime-keyboard-tests PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB onecore bcrypt ws2_32 -pthread)
else()
    target_link_libraries(aln-ime-keyboard-tests PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# Focused tests of libSceJson2 objects through the import resolver (tools/test.ps1, tools/build.sh --test); not part of the default
# build.
add_executable(aln-json2-tests EXCLUDE_FROM_ALL tests/json2_tests.c ${ALN_RUNTIME_SOURCES}
    $<$<BOOL:${WIN32}>:src/compat_win.c src/runtime_log.c> $<$<NOT:$<BOOL:${WIN32}>>:src/runtime_log_posix.c>)
target_include_directories(aln-json2-tests PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
set_target_properties(aln-json2-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln-json2-tests PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
if (WIN32)
    target_compile_definitions(aln-json2-tests PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_link_libraries(aln-json2-tests PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB onecore bcrypt ws2_32 -pthread)
else()
    target_link_libraries(aln-json2-tests PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# Focused tests of scePthreadCancel against every blocking wait (tools/test.ps1, tools/build.sh --test); not part of the default
# build.
add_executable(aln-thread-cancel-tests EXCLUDE_FROM_ALL tests/thread_cancel_tests.c ${ALN_RUNTIME_SOURCES}
    $<$<BOOL:${WIN32}>:src/compat_win.c src/runtime_log.c> $<$<NOT:$<BOOL:${WIN32}>>:src/runtime_log_posix.c>)
target_include_directories(aln-thread-cancel-tests PRIVATE src launcher ${CMAKE_BINARY_DIR}/generated)
set_target_properties(aln-thread-cancel-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln-thread-cancel-tests PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
if (WIN32)
    target_compile_definitions(aln-thread-cancel-tests PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
    target_link_libraries(aln-thread-cancel-tests PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB onecore bcrypt ws2_32 -pthread)
else()
    target_link_libraries(aln-thread-cancel-tests PRIVATE aln_launcher alngpu SDL3::SDL3 Freetype::Freetype ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# Focused tests of the GPU tiling parameters and macro-tiled addresses (tools/test.ps1); not part of the default build.
add_executable(aln-tiling-tests EXCLUDE_FROM_ALL tests/tiling_tests.cpp)
set_target_properties(aln-tiling-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-tiling-tests PRIVATE gpu/shim gpu/shadps4 tests src ${CMAKE_BINARY_DIR}/gpu/include
    gpu/third_party/gcn/include gpu/third_party/half/include gpu/third_party/xbyak)
target_compile_options(aln-tiling-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(aln-tiling-tests PRIVATE alngpu)

# Focused tests of the GDS counter lane offset removal on hand-built IR (tools/test.ps1); not part of the default build.
add_executable(aln-gds-lane-offset-tests EXCLUDE_FROM_ALL tests/gds_lane_offset_tests.cpp)
set_target_properties(aln-gds-lane-offset-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-gds-lane-offset-tests PRIVATE gpu/shim gpu/shadps4 tests src ${CMAKE_BINARY_DIR}/gpu/include
    gpu/third_party/gcn/include gpu/third_party/half/include gpu/third_party/xbyak)
target_link_libraries(aln-gds-lane-offset-tests PRIVATE alngpu)

# Focused tests of the geometry shader output bound and the GPU crash trace (tools/test.ps1); not part of the
# default build.
add_executable(aln-gpu-crash-trace-tests EXCLUDE_FROM_ALL tests/gpu_crash_trace_tests.cpp)
set_target_properties(aln-gpu-crash-trace-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-gpu-crash-trace-tests PRIVATE gpu/shim gpu/shadps4 tests src ${CMAKE_BINARY_DIR}/gpu/include
    gpu/third_party/gcn/include gpu/third_party/half/include gpu/third_party/xbyak)
target_link_libraries(aln-gpu-crash-trace-tests PRIVATE alngpu)

# Focused tests of the deferred Vulkan command recording (tools/test.ps1); not part of the default build.
add_executable(aln-gpu-recording-tests EXCLUDE_FROM_ALL tests/gpu_recording_tests.cpp)
set_target_properties(aln-gpu-recording-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-gpu-recording-tests PRIVATE gpu/shim gpu/shadps4 tests src ${CMAKE_BINARY_DIR}/gpu/include
    gpu/third_party/gcn/include gpu/third_party/half/include gpu/third_party/xbyak)
target_link_libraries(aln-gpu-recording-tests PRIVATE alngpu)

# Focused tests of the Renderer setting and the OpenGL requirements (tools/test.ps1); not part of the default build.
add_executable(aln-renderer-selection-tests EXCLUDE_FROM_ALL tests/renderer_selection_tests.cpp)
set_target_properties(aln-renderer-selection-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-renderer-selection-tests PRIVATE gpu/shim gpu/shadps4)
target_compile_definitions(aln-renderer-selection-tests PRIVATE ALN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(aln-renderer-selection-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the OpenGL detiler and of the emitted GLSL through glslangValidator (tools/test.ps1); no OpenGL
# context. Not part of the default build.
add_executable(aln-opengl-backend-tests EXCLUDE_FROM_ALL tests/opengl_backend_tests.cpp)
set_target_properties(aln-opengl-backend-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-opengl-backend-tests PRIVATE gpu/shim gpu/shadps4 tests src ${CMAKE_BINARY_DIR}/gpu/include
    gpu/third_party/gcn/include gpu/third_party/half/include gpu/third_party/xbyak)
target_compile_definitions(aln-opengl-backend-tests PRIVATE ALN_GLSLANG="${GLSLANG_COMPILER}")
target_link_libraries(aln-opengl-backend-tests PRIVATE alngpu $<$<BOOL:${WIN32}>:onecore>)

# Focused tests of the OpenGL write watch against real page faults, and of the state tracker and stream ring against
# recording entry points (tools/test.ps1); no OpenGL context. Not part of the default build.
add_executable(aln-opengl-perf-tests EXCLUDE_FROM_ALL tests/opengl_perf_tests.cpp)
set_target_properties(aln-opengl-perf-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-opengl-perf-tests PRIVATE gpu/shim gpu/shadps4)
target_compile_definitions(aln-opengl-perf-tests PRIVATE ALN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(aln-opengl-perf-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(aln-opengl-perf-tests PRIVATE SDL3::SDL3 Threads::Threads)

# Focused tests of the Proton/Wine SDL HIDAPI default; not part of the default build.
add_executable(aln-sdl-compat-tests EXCLUDE_FROM_ALL tests/sdl_compat_tests.cpp)
set_target_properties(aln-sdl-compat-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-sdl-compat-tests PRIVATE src)
target_compile_definitions(aln-sdl-compat-tests PRIVATE ALN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(aln-sdl-compat-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the game window's cursor visibility (tools/test.ps1); not part of the default build.
add_executable(aln-cursor-tests EXCLUDE_FROM_ALL tests/cursor_tests.cpp)
set_target_properties(aln-cursor-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-cursor-tests PRIVATE gpu/shim)
target_compile_definitions(aln-cursor-tests PRIVATE ALN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(aln-cursor-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the windowed-mode size and centring arithmetic; not part of the default build.
add_executable(aln-window-tests EXCLUDE_FROM_ALL tests/window_fit_tests.cpp)
set_target_properties(aln-window-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-window-tests PRIVATE src)
target_compile_options(aln-window-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the swapchain image count, present mode, Gamescope detection and the Gamescope-only flip pacing;
# not part of the default build.
add_executable(aln-present-tests EXCLUDE_FROM_ALL tests/present_tests.cpp)
set_target_properties(aln-present-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-present-tests PRIVATE gpu/shim)
target_compile_definitions(aln-present-tests PRIVATE ALN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(aln-present-tests PRIVATE -Wall -Wextra -Werror)

# Focused tests of the AvPlayer worker threads joining themselves and each other (tools/test.ps1); not part of the default build.
add_executable(aln-avplayer-thread-tests EXCLUDE_FROM_ALL tests/avplayer_thread_tests.cpp)
set_target_properties(aln-avplayer-thread-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(aln-avplayer-thread-tests PRIVATE gpu/shadps4)
target_compile_options(aln-avplayer-thread-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(aln-avplayer-thread-tests PRIVATE Threads::Threads)

# Focused tests of the controller prompt style, asset patch identity and generated Xbox glyph table (tools/test.ps1,
# tools/build.sh --test); not part of the default build.
add_executable(aln-controller-prompts-tests EXCLUDE_FROM_ALL tests/controller_prompts_tests.c
    launcher/controller_prompts_style.c launcher/controller_prompts_data.c)
target_include_directories(aln-controller-prompts-tests PRIVATE launcher)
target_compile_options(aln-controller-prompts-tests PRIVATE -Wall -Wextra -Werror)

# Focused test of the borrowed overlay data in the guest file layer (tools/test.ps1); not part of the default build.
add_executable(aln-runtime-overlay-tests EXCLUDE_FROM_ALL tests/runtime_file_overlay_tests.c src/runtime_file.c
    $<$<BOOL:${WIN32}>:src/compat_win.c>)
target_include_directories(aln-runtime-overlay-tests PRIVATE src)
target_compile_definitions(aln-runtime-overlay-tests PRIVATE ALN_OVERLAY_TEST_ROOT="${CMAKE_BINARY_DIR}/runtime-overlay-tests")
if (WIN32)
    target_link_libraries(aln-runtime-overlay-tests PRIVATE bcrypt -pthread)
else()
    target_link_libraries(aln-runtime-overlay-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# Focused tests of the save data commit and profile isolation, and of the local NP identity layer (tools/test.ps1);
# not part of the default build.
add_executable(aln-savedata-tests EXCLUDE_FROM_ALL tests/savedata_tests.c src/runtime_savedata.c src/runtime_file.c
    $<$<BOOL:${WIN32}>:src/compat_win.c>)
target_include_directories(aln-savedata-tests PRIVATE src)
target_compile_definitions(aln-savedata-tests PRIVATE ALN_SAVEDATA_TEST_ROOT="${CMAKE_BINARY_DIR}/savedata-tests")
target_compile_options(aln-savedata-tests PRIVATE -Wall -Wextra -Werror)
add_executable(aln-np-tests EXCLUDE_FROM_ALL tests/np_tests.c src/runtime_np.c)
target_include_directories(aln-np-tests PRIVATE src)
target_compile_options(aln-np-tests PRIVATE -Wall -Wextra -Werror)
# The network status box: its states, texts per language, hook sites, the never-issued authorization code and the script
# patch run on each prepared build's bytecode (no game).
add_executable(aln-online-status-tests EXCLUDE_FROM_ALL tests/online_status_tests.c src/online_status.c src/guest_hook.c
    src/runtime_np.c launcher/exit_game_script.c launcher/online_status_script.c)
target_include_directories(aln-online-status-tests PRIVATE src launcher)
target_compile_definitions(aln-online-status-tests PRIVATE ALN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_options(aln-online-status-tests PRIVATE -Wall -Wextra -Werror)
if (WIN32)
    target_link_libraries(aln-savedata-tests PRIVATE bcrypt -pthread)
    target_link_libraries(aln-np-tests PRIVATE -pthread)
    target_link_libraries(aln-online-status-tests PRIVATE ZLIB::ZLIB -pthread)
else()
    target_link_libraries(aln-savedata-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_link_libraries(aln-np-tests PRIVATE Threads::Threads)
    target_link_libraries(aln-online-status-tests PRIVATE ZLIB::ZLIB Threads::Threads)
endif()
# The classic Matching2, NpWebApi sessions, the Invitation dialog, DGRAM_P2P entry points and the launcher's online client
# against a loopback development lobby (tools/test.ps1 starts one).
add_executable(aln-online-hle-tests EXCLUDE_FROM_ALL tests/online_hle_tests.cpp src/runtime_matching2.cpp src/runtime_p2p.cpp
    src/runtime_webapi.cpp src/online_status.c src/guest_hook.c launcher/online_client.cpp launcher/online_view.cpp)
target_include_directories(aln-online-hle-tests PRIVATE src launcher)
set_target_properties(aln-online-hle-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln-online-hle-tests PRIVATE -Wall -Wextra)
target_link_libraries(aln-online-hle-tests PRIVATE aln_net)
# Local co-op input: SDL3 virtual gamepads through libScePad and libSceUserService; no window, no physical controller.
add_executable(aln-local-pad-tests EXCLUDE_FROM_ALL tests/local_pad_tests.c src/runtime_pad.c src/runtime_users.c)
target_include_directories(aln-local-pad-tests PRIVATE src)
target_compile_options(aln-local-pad-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(aln-local-pad-tests PRIVATE aln_keys SDL3::SDL3 $<IF:$<BOOL:${WIN32}>,-pthread,Threads::Threads>)
# The launcher's Online page model (sections, connection status, friends, invitations, public rooms); no window.
add_executable(aln-online-view-tests EXCLUDE_FROM_ALL tests/online_view_tests.cpp launcher/online_view.cpp)
target_include_directories(aln-online-view-tests PRIVATE launcher src)
set_target_properties(aln-online-view-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln-online-view-tests PRIVATE -Wall -Wextra)
target_link_libraries(aln-online-view-tests PRIVATE aln_net)

if (NOT WIN32)
    # Focused tests of the memory and fiber layers (tools/build.sh --test); not part of the default build.
    add_executable(aln-runtime-tests EXCLUDE_FROM_ALL tests/runtime_tests.c src/runtime_memory.c src/runtime_fiber.c src/runtime_thread.c)
    target_include_directories(aln-runtime-tests PRIVATE src)
    target_compile_options(aln-runtime-tests PRIVATE -Wall -Wextra -Werror -fno-omit-frame-pointer)
    target_link_libraries(aln-runtime-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
