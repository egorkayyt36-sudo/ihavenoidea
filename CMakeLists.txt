cmake_minimum_required(VERSION 3.20)
project(tasdll LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "tasdll is a Windows-only DLL. Configure with MSVC or clang-cl on Windows.")
endif()

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /Zc:__cplusplus /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

include(FetchContent)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
)
FetchContent_MakeAvailable(minhook)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_core STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_core PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_core PUBLIC d3d11 dxgi d3dcompiler dwmapi)

add_library(tasdll SHARED
    src/dllmain.cpp
    src/log.cpp
    src/state.cpp
    src/speedhack.cpp
    src/recorder.cpp
    src/gui.cpp
    src/console_cli.cpp
)

target_include_directories(tasdll PRIVATE src)

target_link_libraries(tasdll PRIVATE
    imgui_core
    minhook
    winmm
    user32
    gdi32
    shell32
    advapi32
    d3d11
    dxgi
)

set_target_properties(tasdll PROPERTIES
    OUTPUT_NAME "tasdll"
    PREFIX ""
    SUFFIX ".dll"
)
