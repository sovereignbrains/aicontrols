cmake_minimum_required(VERSION 3.28)
project(aicontrols LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# The CRT linked in: one exe, no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/W4 /WX /permissive- /EHsc /utf-8)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

option(AIC_ENABLE_ANALYZE "Build with MSVC /analyze static analysis" OFF)
if (AIC_ENABLE_ANALYZE)
  add_compile_options(/analyze /analyze:external-)
endif()

find_package(wil CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-webview2 CONFIG REQUIRED)  # the HTML preview: the static loader, Edge's runtime does the rest

# Everything that is not UI: claude.exe and its protocol.
add_library(aic_core STATIC
  src/core/app_update.cpp
  src/core/attachment.cpp
  src/core/auth.cpp
  src/core/bridge.cpp
  src/core/chat_export.cpp
  src/core/claude_install.cpp
  src/core/claude_process.cpp
  src/core/github.cpp
  src/core/grok_process.cpp
  src/core/highlight.cpp
  src/core/loopback_server.cpp
  src/core/markdown.cpp
  src/core/memory_writer.cpp
  src/core/model_router.cpp
  src/core/pty.cpp
  src/core/secrets.cpp
  src/core/store.cpp
  src/core/tools.cpp
  src/core/usage.cpp
  src/core/vt_screen.cpp
  src/core/voice.cpp)
target_include_directories(aic_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(aic_core PUBLIC WIL::WIL nlohmann_json::nlohmann_json ws2_32 shell32 ole32 crypt32 wintrust windowscodecs winhttp bcrypt winmm)

add_executable(aicontrols WIN32
  src/app/main.cpp
  src/app/chat_window.cpp
  src/app/fonts.cpp
  src/app/popup_menu.cpp
  src/app/aicontrols.manifest
  src/app/aicontrols.rc)
target_link_libraries(aicontrols PRIVATE aic_core d2d1 dwrite dwmapi comctl32 shlwapi unofficial::webview2::webview2)

# Tests of the core without the UI: `ctest --test-dir build/dev`.
enable_testing()
foreach(name tools markdown highlight vt_screen pty auth claude_install attachment app_update usage store model_router)
  add_executable(aic_${name}_test tests/${name}_test.cpp)
  target_link_libraries(aic_${name}_test PRIVATE aic_core)
  add_test(NAME ${name} COMMAND aic_${name}_test)
endforeach()

# Probes, run by hand against the real world (see each file's header):
#   aic_e2e_probe     claude.exe calling our tools (spends a request)
#   aic_grok_probe    grok.exe calling our tools over ACP (spends a request)
#   aic_github_probe  the repositories the GITHUB secret reaches, and a clone
#   aic_store_probe   the titles of the real chats; --delete <id>
#   aic_memory_probe  the memory folder committed and pushed (GITHUB in the environment)
#   aic_bridge_probe  the CLIs' way out listening, for curl -x; --no-direct: as with the VPN off
foreach(name e2e grok github store memory bridge)
  add_executable(aic_${name}_probe tests/probes/${name}_probe.cpp)
  target_link_libraries(aic_${name}_probe PRIVATE aic_core)
endforeach()
