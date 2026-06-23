# cmake/toolchains/macos-arm64.cmake
# Build for macOS/Apple Silicon (arm64).
#
# Usage:
#   cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-arm64.cmake

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_OSX_ARCHITECTURES arm64)
