# Convenience wrapper around CMake.
# Prefer using CMake directly for cross-compilation and full control.

.PHONY: all build run clean

all: build

build:
	cmake -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j$$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)

run: build
	./build/device

clean:
	rm -rf build
