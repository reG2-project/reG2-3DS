cmake_minimum_required(VERSION 3.20)
project(gta2_3ds C CXX)

# Nintendo 3DS build: the Linux port's sources (game, port/src, compat, the gbh_* layer) with the citro3d
# renderer of this folder in place of the OpenGL one. See port/docs/3ds.md.
#   cmake -S port/3ds -B build_3ds -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/port/3ds/toolchain-3ds-clang.cmake -DSDL3_DIR=<prefix>/lib/cmake/SDL3

set(PORT ${CMAKE_CURRENT_LIST_DIR}/..)
set(ROOT ${PORT}/..)

# The game sources are the VC6 build's gta2_lib_src list, as in port/CMakeLists.txt.
file(READ ${ROOT}/CMakeLists.txt top)
string(REGEX MATCH "set\\(gta2_lib_src\n([^)]*)\\)" _ "${top}")
string(REGEX MATCHALL "Source/[^\n ]+\\.cpp" lib_src "${CMAKE_MATCH_1}")
list(REMOVE_ITEM lib_src Source/ImGuiDebug.cpp)
list(TRANSFORM lib_src PREPEND ${ROOT}/)

# Everything the Linux build takes from port/, except the OpenGL renderer and the first-start setup.
file(GLOB port_src CONFIGURE_DEPENDS ${PORT}/src/*.cpp ${PORT}/compat/src/*.cpp ${PORT}/renderer/*.cpp)
list(FILTER port_src EXCLUDE REGEX "/renderer/(gl_[a-z_]+|sdl_gl_context)\\.cpp$")
file(GLOB ctr_src CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -fms-extensions -fshort-wchar -fsigned-char
    -fno-strict-aliasing -fwrapv -ftrivial-auto-var-init=zero -fno-delete-null-pointer-checks -w -Wno-address-of-temporary -Wno-c++11-narrowing)
# Static destructors would fill newlib's 32 atexit slots, so that the port's exit handlers fail, and would run
# while the sound threads still use those statics.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-c++-static-destructors>)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NON_CONFORMING_SWPRINTFS GTA2_PORT GTA2_PORT_SDL __NT__)

find_package(SDL3 REQUIRED CONFIG)

# The vertex shader, assembled by picasso and linked in as render_shbin[] .. render_shbin_end[].
set(shbin ${CMAKE_CURRENT_BINARY_DIR}/render.shbin)
add_custom_command(OUTPUT ${shbin}.o
    COMMAND ${DEVKITPRO}/tools/bin/picasso -o ${shbin} ${CMAKE_CURRENT_LIST_DIR}/render.v.pica
    COMMAND sh -c "${DEVKITPRO}/tools/bin/bin2s -a 4 ${shbin} | ${DEVKITARM}/bin/arm-none-eabi-as -o ${shbin}.o"
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/render.v.pica VERBATIM)
# The map cache's vertex shader (C3DRenderer::DrawMeshes), as map_cache_shbin[].
set(map_shbin ${CMAKE_CURRENT_BINARY_DIR}/map_cache.shbin)
add_custom_command(OUTPUT ${map_shbin}.o
    COMMAND ${DEVKITPRO}/tools/bin/picasso -o ${map_shbin} ${CMAKE_CURRENT_LIST_DIR}/map_cache.v.pica
    COMMAND sh -c "${DEVKITPRO}/tools/bin/bin2s -a 4 ${map_shbin} | ${DEVKITARM}/bin/arm-none-eabi-as -o ${map_shbin}.o"
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/map_cache.v.pica VERBATIM)

# The minimap's frame and markers (port/docs/minimap/assets, original art) as C arrays.
set(minimap_assets ${PORT}/docs/minimap/assets)
set(minimap_png Frame=${minimap_assets}/frames/J_police_scanner/frame_1x.png
    LampGang=${minimap_assets}/frames/J_police_scanner/lamp_gang_1x.png
    LampHostile=${minimap_assets}/frames/J_police_scanner/lamp_hostile_1x.png
    Arrow=${minimap_assets}/markers/player_arrow_1x.png)
string(REGEX REPLACE "[A-Za-z]+=" "" minimap_png_files "${minimap_png}")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/minimap_assets.h
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/embed_png.py ${CMAKE_CURRENT_BINARY_DIR}/minimap_assets.h ${minimap_png}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/embed_png.py ${minimap_png_files} VERBATIM)

add_executable(gta2 ${lib_src} ${ROOT}/test.cpp ${ROOT}/StdAfx.cpp ${ROOT}/Source/3rdParty/bink.cpp
    ${port_src} ${ctr_src} ${shbin}.o ${map_shbin}.o ${CMAKE_CURRENT_BINARY_DIR}/minimap_assets.h)
set_target_properties(gta2 PROPERTIES SUFFIX .elf)
set_source_files_properties(${PORT}/compat/src/miles.cpp ${PORT}/compat/src/miles3d.cpp PROPERTIES COMPILE_DEFINITIONS BUILD_MSS)
set_source_files_properties(${ROOT}/Source/3rdParty/bink.cpp PROPERTIES COMPILE_DEFINITIONS BUILD_BINK)
# Console test builds: extra flags for the renderer, and for it with the per-frame game code (cars' physics,
# sprites, map), e.g. -O2 in a MinSizeRel build. Full LTO keeps each function's optsize from its own file.
set(GTA2_3DS_FLAGS "" CACHE STRING "Extra compile flags for every file")
set(GTA2_3DS_RENDER_FLAGS "" CACHE STRING "Extra compile flags for the port's renderer files")
set(GTA2_3DS_HOT_FLAGS "" CACHE STRING "Extra compile flags for the renderer and the game's per-frame files")
set(render_src ${PORT}/renderer/gbh_api.cpp ${PORT}/renderer/vid_system.cpp
    ${CMAKE_CURRENT_LIST_DIR}/c3d_renderer.cpp ${CMAKE_CURRENT_LIST_DIR}/c3d_context.cpp
    ${CMAKE_CURRENT_LIST_DIR}/map_cache.cpp ${CMAKE_CURRENT_LIST_DIR}/map_cache_mesh.cpp)
set(hot_src ${PORT}/src/lights.cpp ${PORT}/src/shadows.cpp)
foreach(f CarPhysics collide sprite SpriteGrid MapRenderer SpriteRenderer map_0x370 gbh_graphics fix16 Fix16_Point Fix16_Rect ang16)
    list(APPEND hot_src ${ROOT}/Source/${f}.cpp)
endforeach()
if(GTA2_3DS_FLAGS)
    target_compile_options(gta2 PRIVATE "SHELL:${GTA2_3DS_FLAGS}")
endif()
# Profile guided optimisation with the Linux port's counts (pgo/train.sh), kept as text and indexed by the
# image's llvm-profdata. Functions whose code differs from the profiled build's keep the static heuristics.
option(GTA2_3DS_PGO "Optimise with the profile of GTA2_3DS_PGO_PROFILE" ON)
set(GTA2_3DS_PGO_PROFILE ${CMAKE_CURRENT_LIST_DIR}/pgo/gta2.proftext CACHE FILEPATH "Text profile for GTA2_3DS_PGO")
find_program(LLVM_PROFDATA llvm-profdata-${CLANG_VERSION})
if(GTA2_3DS_PGO AND NOT LLVM_PROFDATA)
    message(WARNING "No llvm-profdata-${CLANG_VERSION} (an older build image): building without PGO")
elseif(GTA2_3DS_PGO)
    set(profdata ${CMAKE_CURRENT_BINARY_DIR}/gta2.profdata)
    add_custom_command(OUTPUT ${profdata}
        COMMAND ${LLVM_PROFDATA} merge -o ${profdata} ${GTA2_3DS_PGO_PROFILE}
        DEPENDS ${GTA2_3DS_PGO_PROFILE} VERBATIM)
    target_compile_options(gta2 PRIVATE -fprofile-instr-use=${profdata})
    get_target_property(gta2_src gta2 SOURCES)
    set_property(SOURCE ${gta2_src} APPEND PROPERTY OBJECT_DEPENDS ${profdata})
endif()
# pgo/hot_order.ld puts the profiled calls first in .text, in call graph order, then the rest of the hot code, and the
# never run code last. gta2.map and lto.o are what pgo/hot_order.py rewrites it from.
option(GTA2_3DS_HOT_LAYOUT "Order .text by pgo/hot_order.ld" OFF)
if(GTA2_3DS_HOT_LAYOUT)
    target_link_options(gta2 PRIVATE -Wl,--section-ordering-file,${CMAKE_CURRENT_LIST_DIR}/pgo/hot_order.ld)
    set_property(TARGET gta2 APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/pgo/hot_order.ld)
endif()
target_link_options(gta2 PRIVATE -Wl,-Map,gta2.map -Wl,-plugin-opt=obj-path=${CMAKE_CURRENT_BINARY_DIR}/lto.o)
separate_arguments(render_flags UNIX_COMMAND "${GTA2_3DS_RENDER_FLAGS}")
separate_arguments(hot_flags UNIX_COMMAND "${GTA2_3DS_HOT_FLAGS}")
set_property(SOURCE ${render_src} APPEND PROPERTY COMPILE_OPTIONS ${render_flags} ${hot_flags})
set_property(SOURCE ${hot_src} APPEND PROPERTY COMPILE_OPTIONS ${hot_flags})
target_compile_options(gta2 PRIVATE -include ${PORT}/compat/prelude.hpp
    -fno-builtin-wcslen -fno-builtin-wcscpy -fno-builtin-wcscmp -fno-builtin-wcsncmp -fno-builtin-wcschr
    -fno-builtin-wmemcpy -fno-builtin-wmemset -fno-builtin-wmemcmp)
target_include_directories(gta2 PRIVATE ${PORT}/compat/include ${PORT}/include ${ROOT}/Source ${ROOT} ${CMAKE_CURRENT_BINARY_DIR})
# OpenDSP (build_opendsp.sh's prefix) links in a DSP firmware; without it, sound needs the console's dspfirm.cdc.
set(OPENDSP_DIR "" CACHE PATH "OpenDSP prefix from build_opendsp.sh, empty for none")
if(OPENDSP_DIR)
    set(opendsp ${OPENDSP_DIR}/lib/libopendsp.a)
    target_compile_definitions(gta2 PRIVATE GTA2_OPENDSP)
    target_include_directories(gta2 PRIVATE ${OPENDSP_DIR}/include)
    # Its BSD-2 terms: the notice goes with the binary.
    configure_file(${OPENDSP_DIR}/LICENSE LICENSE.OpenDSP.txt COPYONLY)
endif()
target_link_libraries(gta2 PRIVATE SDL3::SDL3-static ${opendsp} citro3d ctru m)
# test_pad.cpp adds scripted buttons, sticks and touches to what SDL and the game read; platform_3ds.cpp shows SDL's
# message boxes; stats_3ds.cpp times the main thread's sleeps; linear_lock.cpp puts the linear heap's calls under a lock.
target_link_options(gta2 PRIVATE -Wl,--wrap=hidScanInput,--wrap=hidKeysDown,--wrap=hidKeysUp,--wrap=hidCircleRead,--wrap=irrstCstickRead,--wrap=hidTouchRead,--wrap=SDL_ShowSimpleMessageBox,--wrap=svcSleepThread
    -Wl,--wrap=linearAlloc,--wrap=linearMemAlign,--wrap=linearRealloc,--wrap=linearGetSize,--wrap=linearFree,--wrap=linearSpaceFree)

# The Homebrew Launcher's title, description, author and 48x48 icon.
set(icon ${CMAKE_CURRENT_BINARY_DIR}/icon.png)
add_custom_command(OUTPUT ${icon}
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/make_icon.py ${icon}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/make_icon.py VERBATIM)
set(GTA2_3DS_TITLE "GTA2" CACHE STRING "The Homebrew Launcher's title, at most 64 characters")
set(smdh ${CMAKE_CURRENT_BINARY_DIR}/gta2.smdh)
add_custom_command(OUTPUT ${smdh}
    COMMAND ${DEVKITPRO}/tools/bin/smdhtool --create "${GTA2_3DS_TITLE}" "Grand Theft Auto 2, from the gta2_re decompilation. Needs the PC game's data."
            "gta2_re" ${icon} ${smdh}
    DEPENDS ${icon} VERBATIM)
add_custom_command(OUTPUT gta2.3dsx
    COMMAND ${DEVKITPRO}/tools/bin/3dsxtool $<TARGET_FILE:gta2> gta2.3dsx --smdh=${smdh}
    DEPENDS gta2 ${smdh} VERBATIM)
add_custom_target(gta2_3dsx ALL DEPENDS gta2.3dsx)
