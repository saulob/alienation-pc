# The launcher: TRG Launcher (third_party/trg-launcher, fetched by tools\build.ps1) in a window of its own that is
# closed before the loader creates the game window.
set(TRG_DIR ${PROJECT_SOURCE_DIR}/third_party/trg-launcher)
if (NOT EXISTS ${TRG_DIR}/include/trg/launcher.h)
    message(FATAL_ERROR "TRG Launcher is missing: run tools\\build.ps1, which fetches it (gpu\\deps.txt).")
endif()
set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/gpu/third_party/imgui)

# TRG's settings file, platform helpers and HTTPS downloads do not need Dear ImGui.
add_library(trg_core STATIC ${TRG_DIR}/src/settings.cpp ${TRG_DIR}/src/platform.cpp ${TRG_DIR}/src/download.cpp)
target_include_directories(trg_core PUBLIC ${TRG_DIR}/include)
target_compile_definitions(trg_core PRIVATE _CRT_SECURE_NO_WARNINGS)
if (WIN32)
    target_link_libraries(trg_core PRIVATE shell32 ole32 comdlg32 winhttp)
else()
    target_link_libraries(trg_core PRIVATE Threads::Threads)
endif()

# The launcher draws with the Dear ImGui compiled into the video core library.
add_library(trg_launcher STATIC ${TRG_DIR}/src/launcher.cpp ${TRG_DIR}/src/headers.cpp)
target_include_directories(trg_launcher PUBLIC ${IMGUI_DIR})
target_compile_definitions(trg_launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(trg_launcher PUBLIC trg_core alngpu)

# Written for C++17: in C++20 and later u8string() is a different type.
set_target_properties(trg_core trg_launcher PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_library(aln_imgui_sdl STATIC ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(aln_imgui_sdl PUBLIC ${IMGUI_DIR})
target_link_libraries(aln_imgui_sdl PUBLIC SDL3::SDL3)

# Game files (check, PKG unpacking, install, boot image), the trophies (src/trophies) and the settings file: no window
# needed (tests link only this).
find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
add_library(aln_launcher_core STATIC boot_image.cpp file_browser.cpp game_files.cpp game_install.cpp game_process.cpp
    game_profile.cpp incident_report.cpp log_scrubber.cpp report_history.cpp online_client.cpp online_settings.cpp
    online_view.cpp options.cpp page_icons.cpp paths.cpp frame_rate.c pkg_extract.cpp resolution.c settings.cpp
    controller_prompts.cpp controller_prompts_style.c controller_prompts_data.c
    controller_prompts_glyphs_data.c trophy_content.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_data.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_key.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_pack.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_runtime.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_service.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_store.cpp ${PROJECT_SOURCE_DIR}/src/trophies/trophy_view.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trp_archive.cpp)
target_include_directories(aln_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aln_launcher_core PUBLIC trg_core SDL3::SDL3 aln_ui_text aln_keys aln_net aln_startup_phase PRIVATE ZLIB::ZLIB)
# Hashes and ciphers: CNG on Windows, OpenSSL's libcrypto elsewhere.
if (WIN32)
    target_sources(aln_launcher_core PRIVATE crypto_win.cpp)
    target_link_libraries(aln_launcher_core PRIVATE bcrypt)
    set(ALN_LOG_SOURCE ${PROJECT_SOURCE_DIR}/src/runtime_log.c)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_sources(aln_launcher_core PRIVATE crypto_openssl.cpp)
    target_link_libraries(aln_launcher_core PRIVATE OpenSSL::Crypto)
    set(ALN_LOG_SOURCE ${PROJECT_SOURCE_DIR}/src/runtime_log_posix.c)
endif()
set(ALN_LOG_SOURCES ${ALN_LOG_SOURCE} ${PROJECT_SOURCE_DIR}/src/runtime_session_log.c)

# The sidebar's page icons (assets/launcher/icons/*.svg) as a table of byte arrays, page_icons.cpp.
file(GLOB ALN_PAGE_ICONS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/assets/launcher/icons/*.svg)
list(SORT ALN_PAGE_ICONS)
set(ALN_PAGE_ICON_LIST ${CMAKE_BINARY_DIR}/generated/page_icons.txt)
file(CONFIGURE OUTPUT ${ALN_PAGE_ICON_LIST} CONTENT "${ALN_PAGE_ICONS}")
set(ALN_PAGE_ICON_SOURCE ${CMAKE_BINARY_DIR}/generated/page_icon_data.cpp)
add_custom_command(OUTPUT ${ALN_PAGE_ICON_SOURCE}
    COMMAND ${CMAKE_COMMAND} -DINPUT_LIST=${ALN_PAGE_ICON_LIST} -DOUTPUT=${ALN_PAGE_ICON_SOURCE} -DSYMBOL=kPageIconFiles
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_files.cmake
    DEPENDS ${ALN_PAGE_ICON_LIST} ${ALN_PAGE_ICONS} ${CMAKE_CURRENT_SOURCE_DIR}/embed_files.cmake
    COMMENT "Embedding the page icons")
target_sources(aln_launcher_core PRIVATE ${ALN_PAGE_ICON_SOURCE})

add_library(aln_launcher STATIC host.cpp incident_page.cpp launcher.cpp my_reports.cpp online_page.cpp package_browser.cpp problem_dialog.cpp shift_key.cpp trophy_page.cpp update_config.cpp
    updater/github_release.cpp updater/json.cpp updater/sha256.cpp updater/third_party/miniz/miniz.c
    updater/update_controller.cpp updater/update_job.cpp updater/update_paths.cpp updater/update_platform.cpp
    updater/update_ui.cpp updater/update_zip.cpp updater/version.cpp)
set_source_files_properties(updater/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
target_compile_definitions(aln_launcher PRIVATE ALN_PORT_VERSION="${PROJECT_VERSION}")
target_include_directories(aln_launcher PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
target_link_libraries(aln_launcher PUBLIC aln_launcher_core trg_launcher aln_imgui_sdl ${CMAKE_DL_LIBS})

# Windows helper that replaces the launcher after it exits; not part of the Linux package.
if (WIN32)
    add_executable(trg-updater WIN32 updater/helper_main.cpp updater/install_transaction.cpp updater/sha256.cpp
        updater/update_job.cpp updater/update_paths.cpp updater/update_platform.cpp)
    target_include_directories(trg-updater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(trg-updater PRIVATE shell32)
    target_link_options(trg-updater PRIVATE -static)
    set_target_properties(trg-updater PROPERTIES OUTPUT_NAME "trg-updater" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(trg-updater PRIVATE -Wall -Wextra)

    add_executable(aln-update-restart-marker EXCLUDE_FROM_ALL WIN32 ../tests/update_restart_marker.cpp)
    target_link_libraries(aln-update-restart-marker PRIVATE shell32)
    target_link_options(aln-update-restart-marker PRIVATE -static)
    set_target_properties(aln-update-restart-marker PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    add_executable(aln-update-helper-tests EXCLUDE_FROM_ALL ../tests/update_helper_tests.cpp)
    target_compile_definitions(aln-update-helper-tests PRIVATE
        ALN_UPDATER_EXE="$<TARGET_FILE:trg-updater>"
        ALN_MARKER_EXE="$<TARGET_FILE:aln-update-restart-marker>")
    add_dependencies(aln-update-helper-tests trg-updater aln-update-restart-marker)
    set_target_properties(aln-update-helper-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(aln-update-helper-tests PRIVATE -Wall -Wextra)
endif()

set_target_properties(aln_launcher_core aln_launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln_launcher_core PRIVATE -Wall -Wextra)
target_compile_options(aln_launcher PRIVATE -Wall -Wextra)

# Focused updater tests: versions, release JSON, SHA-256, ZIP safety, staging, jobs and rollback; no network.
add_executable(aln-update-tests EXCLUDE_FROM_ALL ../tests/update_tests.cpp
    updater/github_release.cpp updater/install_transaction.cpp updater/json.cpp updater/sha256.cpp
    updater/update_job.cpp updater/update_paths.cpp updater/update_platform.cpp updater/update_zip.cpp
    updater/version.cpp updater/third_party/miniz/miniz.c)
target_include_directories(aln-update-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(aln-update-tests PRIVATE ALN_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    ALN_UPDATER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if (WIN32)
    target_link_libraries(aln-update-tests PRIVATE shell32)
endif()
set_target_properties(aln-update-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln-update-tests PRIVATE -Wall -Wextra)

# Focused tests of the Bug Report page's logic, on synthetic logs in temporary folders: no window, no network.
add_executable(aln-incident-tests EXCLUDE_FROM_ALL ../tests/incident_tests.cpp)
target_compile_definitions(aln-incident-tests PRIVATE ALN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(aln-incident-tests PRIVATE aln_launcher_core)
set_target_properties(aln-incident-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln-incident-tests PRIVATE -Wall -Wextra)

# Focused tests of the local trophies (tools/test.ps1); not part of the default build.
add_executable(aln-trophy-tests EXCLUDE_FROM_ALL ../tests/trophy_tests.cpp ${ALN_LOG_SOURCES} ../gpu/shim/aln_png.cpp)
target_link_libraries(aln-trophy-tests PRIVATE aln_launcher_core PNG::PNG)
target_include_directories(aln-trophy-tests PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
target_compile_definitions(aln-trophy-tests PRIVATE ALN_TROPHY_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src/trophies")
set_target_properties(aln-trophy-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(aln-trophy-tests PRIVATE -Wall -Wextra)
