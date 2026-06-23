# cmake/toolchains/linux-arm64.cmake
# Cross-compile for Linux/arm64 (aarch64) from an x86_64 Linux host.
#
# Requirements:
#   sudo apt install gcc-aarch64-linux-gnu
#   # Cross-compiled dependencies (mosquitto, libwebsockets):
#   sudo apt install libmosquitto-dev:arm64 libwebsockets-dev:arm64
#   # Enable arm64 dpkg arch first:
#   sudo dpkg --add-architecture arm64
#   sudo apt update
#
# Usage:
#   cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm64.cmake

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config wrapper picks up cross-compiled .pc files automatically
set(ENV{PKG_CONFIG_PATH} /usr/lib/aarch64-linux-gnu/pkgconfig)
