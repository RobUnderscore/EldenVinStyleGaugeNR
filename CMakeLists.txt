cmake_minimum_required(VERSION 3.20)
project(VinStyleRankGauge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- Dependencies ----

# MinHook (flat structure from EROverlay)
add_library(minhook STATIC
    deps/minhook/buffer.c
    deps/minhook/hook.c
    deps/minhook/trampoline.c
    deps/minhook/hde64.c
)
target_include_directories(minhook PUBLIC deps/minhook)

# Pattern16 (header-only)
add_library(pattern16 INTERFACE)
target_include_directories(pattern16 INTERFACE deps/pattern16)

# ImGui (flat structure - backends in same dir)
add_library(imgui STATIC
    deps/imgui/imgui.cpp
    deps/imgui/imgui_draw.cpp
    deps/imgui/imgui_tables.cpp
    deps/imgui/imgui_widgets.cpp
    deps/imgui/imgui_impl_dx12.cpp
    deps/imgui/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC deps/imgui)

# ---- Main DLL ----

add_library(VinStyleRankGauge SHARED
    src/main.cpp
    src/d3dhook.cpp
    src/memory.cpp
    src/style_rank.cpp
    src/overlay.cpp
    src/config.cpp
)

target_link_libraries(VinStyleRankGauge PRIVATE
    minhook
    pattern16
    imgui
    d3d12
    dxgi
    dwmapi
    shell32
)

target_include_directories(VinStyleRankGauge PRIVATE src)

set_target_properties(VinStyleRankGauge PROPERTIES
    OUTPUT_NAME "VinStyleRankGauge"
    PREFIX ""
)
