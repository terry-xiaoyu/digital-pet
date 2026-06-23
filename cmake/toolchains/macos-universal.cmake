# cmake/toolchains/macos-universal.cmake
# Build a macOS Universal Binary (x86_64 + arm64) using Apple's lipo.
# Requires Xcode / clang on macOS.
#
# Usage:
#   cmake -B build-universal -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-universal.cmake

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")
