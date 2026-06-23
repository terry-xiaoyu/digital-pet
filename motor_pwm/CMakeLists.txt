cmake_minimum_required(VERSION 3.10)
project(motor LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# The PWM drivers use POSIX symbols (usleep, struct timespec via <gpiod.h>), so
# they require GNU extensions (-std=gnu99). This must be set explicitly because
# when built as a subdirectory the parent project may force CMAKE_C_EXTENSIONS
# OFF, which would select strict -std=c99 and hide those declarations.
set(CMAKE_C_EXTENSIONS ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(PWM_DRIVER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/drivers")

# Find libgpiod (required by both PWM drivers)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGPIOD REQUIRED libgpiod)

# Detect libgpiod API version via compile check (v1 vs v2)
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${LIBGPIOD_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${LIBGPIOD_LIBRARIES})
check_c_source_compiles("
    #include <gpiod.h>
    int main(void) {
        enum gpiod_line_value val = GPIOD_LINE_VALUE_ACTIVE;
        (void)val; return 0;
    }
" LIBGPIOD_HAS_V2_API)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

if(LIBGPIOD_HAS_V2_API)
    add_definitions(-DLIBGPIOD_V2)
    message(STATUS "libgpiod: using v2 API")
else()
    add_definitions(-DLIBGPIOD_V1)
    message(STATUS "libgpiod: using v1 API")
endif()

include_directories(${LIBGPIOD_INCLUDE_DIRS})

# Source files: core + two PWM drivers
set(MOTOR_SOURCES
    src/motor_core.c
    ${PWM_DRIVER_PATH}/drv_pwm_generic.c
    ${PWM_DRIVER_PATH}/drv_pwm_RoHS.c
)

add_library(motor STATIC ${MOTOR_SOURCES})
set_target_properties(motor PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(motor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(motor PUBLIC
    ${LIBGPIOD_LIBRARIES}
    pthread
    m
)

target_compile_options(motor PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Test executable
option(BUILD_TESTS "Build test programs" ON)
if(BUILD_TESTS)
    add_executable(test_motor_pwm tests/test_motor_pwm.c)
    set_target_properties(test_motor_pwm PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(test_motor_pwm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(test_motor_pwm
        -Wl,--whole-archive motor -Wl,--no-whole-archive
        ${LIBGPIOD_LIBRARIES}
        pthread
    )
endif()
