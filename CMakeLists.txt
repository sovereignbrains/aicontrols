cmake_minimum_required(VERSION 3.28)
project(aicontrols LANGUAGES CXX)

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

# Everything that is not UI: claude.exe and its protocol.
add_library(aic_core STATIC
  src/core/attachment.cpp
  src/core/claude_process.cpp
  src/core/store.cpp)
target_include_directories(aic_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(aic_core PUBLIC WIL::WIL nlohmann_json::nlohmann_json ws2_32 shell32 ole32 crypt32 windowscodecs)

add_executable(aicontrols WIN32
  src/app/main.cpp
  src/app/chat_window.cpp
  src/app/aicontrols.manifest)
target_link_libraries(aicontrols PRIVATE aic_core d2d1 dwrite dwmapi comctl32)
