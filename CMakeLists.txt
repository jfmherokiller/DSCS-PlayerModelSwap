cmake_minimum_required(VERSION 3.21)

project("DSCSPlayerModelSwap" LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)

include(cmake/CPM.cmake)

# --- Dependencies ---
# DSCSModLoader with signature-based address resolution (findSignature export).
# For local development point CPM at a checkout: -DCPM_DSCSModLoader_SOURCE=<path>
CPMAddPackage(
  NAME DSCSModLoader
  GITHUB_REPOSITORY "jfmherokiller/DSCSModLoader"
  GIT_TAG "7ae6be3d361af98097e446b739c837e82f7c2e08"
)

CPMAddPackage(
  NAME minhook
  GITHUB_REPOSITORY "TsudaKageyu/minhook"
  GIT_TAG "v1.3.3"
  DOWNLOAD_ONLY YES
)

add_library(minhook_static STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook_static PUBLIC ${minhook_SOURCE_DIR}/include)

CPMAddPackage(
  NAME imgui
  GITHUB_REPOSITORY "ocornut/imgui"
  GIT_TAG "v1.92.9b"
  DOWNLOAD_ONLY YES
)

add_library(imgui_static STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_static PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# The game reads pads itself; keep the overlay from loading XInput.
target_compile_definitions(imgui_static PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# --- Building ---
add_library(DSCSPlayerModelSwap SHARED "src/PlayerModelSwap.cpp" "src/Overlay.cpp")
target_link_libraries(DSCSPlayerModelSwap PRIVATE DSCSModLoader minhook_static imgui_static dinput8 dxguid opengl32)
