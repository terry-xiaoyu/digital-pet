# cmake/toolchains/macos-x86_64.cmake
# Build for macOS/Intel (x86_64).
#
# On an Apple Silicon Mac this performs a cross-build (Rosetta is NOT required
# for the build tools -- CMake + clang handle it natively).
#
# Usage:
#   cmake -B build-x86_64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-x86_64.cmake

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_OSX_ARCHITECTURES x86_64)
