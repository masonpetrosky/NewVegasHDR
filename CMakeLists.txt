cmake_minimum_required(VERSION 3.24)
project(NewVegasHDR VERSION 0.1.0 LANGUAGES CXX)

# Fallout New Vegas is a 32-bit game, so everything here builds for x86 (build.ps1 enters the x86 toolset).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure with the x86 toolset: New Vegas is a 32-bit game (build.ps1 does this).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W4 /permissive- /utf-8)

# Shaders are compiled to headers with the Windows SDK's fxc: shader(<symbol> <file> <profile> <entry> [defines])
# writes ${SHADER_DIR}/<symbol>.h with the bytecode array <symbol>.
find_program(FXC fxc REQUIRED)
set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SHADER_HEADERS)
function(shader symbol file profile entry)
  set(defines)
  foreach(define ${ARGN})
    list(APPEND defines /D ${define})
  endforeach()
  set(header ${SHADER_DIR}/${symbol}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
    COMMAND ${FXC} /nologo /T ${profile} /E ${entry} /O3 ${defines} /Vn ${symbol} /Fh ${header}
            ${CMAKE_SOURCE_DIR}/src/shaders/${file}
    DEPENDS src/shaders/${file}
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${header} PARENT_SCOPE)
endfunction()

# The HDR blend shaders, per variant of the game's ISHDRBLENDINSHADER and per shader model (ps_2_b pairs with the
# game's vs_1_1, ps_3_0 with Vanilla Plus Tonemapper's vs_3_0).
shader(g_hdrBlendPlainPs2 hdr_blend.hlsl ps_2_b main)
shader(g_hdrBlendCinPs2 hdr_blend.hlsl ps_2_b main CINEMATIC=1)
shader(g_hdrBlendCinAmPs2 hdr_blend.hlsl ps_2_b main CINEMATIC=1 ALPHA_MASK=1)
shader(g_hdrBlendPlainPs3 hdr_blend.hlsl ps_3_0 main)
shader(g_hdrBlendCinPs3 hdr_blend.hlsl ps_3_0 main CINEMATIC=1)
shader(g_hdrBlendCinAmPs3 hdr_blend.hlsl ps_3_0 main CINEMATIC=1 ALPHA_MASK=1)
shader(g_encodeVs encode.hlsl vs_3_0 VSMain)
shader(g_encodePs encode.hlsl ps_3_0 PSMain)

# The xNVSE plugin.
add_library(NewVegasHDR SHARED
  src/plugin.cpp
  src/config.cpp
  src/dds.cpp
  src/display.cpp
  src/log.cpp
  src/shader_info.cpp
  src/trace.cpp
  src/vtable.cpp
  ${SHADER_HEADERS})
target_include_directories(NewVegasHDR PRIVATE src ${SHADER_DIR})
set_target_properties(NewVegasHDR PROPERTIES PREFIX "" SUFFIX ".dll")

# Standalone check of a d3d9.dll's HDR support (tests/dxvk_hdr_probe.cpp).
add_executable(dxvk_hdr_probe tests/dxvk_hdr_probe.cpp)
target_include_directories(dxvk_hdr_probe PRIVATE src)

# End-to-end check of the plugin on DXVK without the game (tests/harness.cpp; test.ps1 runs it).
add_executable(harness tests/harness.cpp src/display.cpp)
target_include_directories(harness PRIVATE src)
