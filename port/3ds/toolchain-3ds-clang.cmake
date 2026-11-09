# Nintendo 3DS: clang compiles (the game needs its MSVC extensions), devkitARM's GCC links against
# newlib, libstdc++ and libctru. Run inside the image of port/3ds/Dockerfile.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv6k)
set(NINTENDO_3DS TRUE)

set(DEVKITPRO /opt/devkitpro)
set(DEVKITARM ${DEVKITPRO}/devkitARM)
set(CTR_ROOT ${DEVKITPRO}/libctru)
set(CLANG_VERSION 22 CACHE STRING "clang-<version> in the build image")

set(CMAKE_C_COMPILER clang-${CLANG_VERSION})
set(CMAKE_CXX_COMPILER clang++-${CLANG_VERSION})
set(CMAKE_AR ${DEVKITARM}/bin/arm-none-eabi-ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${DEVKITARM}/bin/arm-none-eabi-ranlib CACHE FILEPATH "")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

file(GLOB gcc_dirs LIST_DIRECTORIES true ${DEVKITARM}/arm-none-eabi/include/c++/*)
list(GET gcc_dirs 0 cxx_inc)

# devkitARM's ABI (3DS.cmake): ARM11 MPCore, VFPv2, hard float, software thread pointer. Unaligned loads and
# stores as devkitARM's GCC makes them (clang's bare metal default is byte by byte).
set(arch "--target=armv6k-none-eabihf -mcpu=mpcore -mfpu=vfpv2 -mfloat-abi=hard -mtp=soft -munaligned-access")
set(inc "--sysroot=${DEVKITARM}/arm-none-eabi -nostdinc++ -isystem ${cxx_inc} -isystem ${cxx_inc}/arm-none-eabi/armv6k/fpu -isystem ${CTR_ROOT}/include")
set(CMAKE_C_FLAGS_INIT "${arch} -isystem ${CTR_ROOT}/include -D__3DS__ -ffunction-sections -flto")
set(CMAKE_CXX_FLAGS_INIT "${arch} ${inc} -D__3DS__ -ffunction-sections -flto")

# Link time optimisation: the objects are LLVM bitcode, which devkitARM's ld compiles through clang's linker
# plugin (llvm-<version>-linker-tools).
set(lto "-Wl,-plugin,/usr/lib/llvm-${CLANG_VERSION}/lib/LLVMgold.so,-plugin-opt=mcpu=mpcore,-plugin-opt=-function-sections")
set(link "${DEVKITARM}/bin/arm-none-eabi-g++ -specs=3dsx.specs -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -L${CTR_ROOT}/lib -Wl,--no-enum-size-warning,--no-wchar-size-warning ${lto}")
set(CMAKE_CXX_LINK_EXECUTABLE "${link} <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")

set(CMAKE_FIND_ROOT_PATH ${DEVKITPRO}/portlibs/3ds ${CTR_ROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
