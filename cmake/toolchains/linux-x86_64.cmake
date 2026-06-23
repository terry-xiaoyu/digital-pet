# cmake/toolchains/linux-x86_64.cmake
# Native or explicit Linux/x86_64 build.
# Use this when cross-compiling or when you want to pin the target explicitly.
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x86_64.cmake

set(CMAKE_SYSTEM_NAME    Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Use the system compiler for native builds
# Override with CC / CXX env-vars to use a cross-compiler
