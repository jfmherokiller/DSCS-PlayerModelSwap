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

# --- Building ---
add_library(DSCSPlayerModelSwap SHARED "src/PlayerModelSwap.cpp")
target_link_libraries(DSCSPlayerModelSwap PRIVATE DSCSModLoader minhook_static)
