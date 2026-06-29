cmake_minimum_required(VERSION 3.14)

project(device_agent_sdk
  VERSION 1.0.0
  DESCRIPTION "Device Agent C SDK"
  LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# --- Options ---------------------------------------------------------------
option(DA_BUILD_VOICE    "Build WebSocket voice client (requires libwebsockets)" ON)
option(DA_BUILD_VOICE_CHAT_AUDIO "Build voice_chat against on-board SpacemitAudio driver (requires PortAudio/ALSA)" OFF)
option(DA_BUILD_EXAMPLES "Build example programs"                                ON)
option(DA_BUILD_VISION   "Build vision upload/chat client (requires libcurl)"    ON)
option(DA_PET_MOTORS     "Build digital-pet motor control (requires libgpiod)"   ON)

# --- Dependencies ----------------------------------------------------------

# libmosquitto and cJSON -- system packages when available
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
pkg_check_modules(CJSON QUIET IMPORTED_TARGET libcjson)

if(CJSON_FOUND)
  set(DA_CJSON_TARGET PkgConfig::CJSON)
else()
  # cJSON fallback -- fetched automatically when the system package is missing.
  include(FetchContent)
  FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
  )
  set(ENABLE_CJSON_TEST    OFF CACHE BOOL "" FORCE)
  set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(cjson)
  set(DA_CJSON_TARGET cjson)

  # Some environments include cJSON as <cjson/cJSON.h>. Create a compatibility
  # include directory for the FetchContent source tree.
  set(DA_COMPAT_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/compat-includes")
  file(MAKE_DIRECTORY "${DA_COMPAT_INCLUDE_DIR}/cjson")
  configure_file(
    "${cjson_SOURCE_DIR}/cJSON.h"
    "${DA_COMPAT_INCLUDE_DIR}/cjson/cJSON.h"
    COPYONLY
  )
endif()

# libwebsockets -- required only when DA_BUILD_VOICE is ON
if(DA_BUILD_VOICE)
  pkg_check_modules(WEBSOCKETS REQUIRED libwebsockets)
  find_package(OpenSSL REQUIRED)
endif()
if(DA_BUILD_VISION)
  pkg_check_modules(CURL REQUIRED libcurl)
endif()

# --- SDK static library -----------------------------------------------------

set(DA_SDK_SOURCES
  src/client.c
)
if(DA_BUILD_VOICE)
  list(APPEND DA_SDK_SOURCES src/voice_client.c)
endif()
if(DA_BUILD_VISION)
  list(APPEND DA_SDK_SOURCES src/vision_client.c)
endif()

add_library(device_agent_sdk STATIC ${DA_SDK_SOURCES})

target_compile_definitions(device_agent_sdk
  PRIVATE
    _POSIX_C_SOURCE=200809L
)

# On macOS, _POSIX_C_SOURCE can hide BSD typedefs (e.g. u_char/u_short)
# that are required by transitive libwebsockets system headers.
if(APPLE)
  target_compile_definitions(device_agent_sdk PRIVATE _DARWIN_C_SOURCE)
endif()

target_include_directories(device_agent_sdk
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${cjson_SOURCE_DIR}
)

target_link_libraries(device_agent_sdk
  PUBLIC
    ${DA_CJSON_TARGET}
    ${MOSQUITTO_LIBRARIES}
    pthread
)

target_include_directories(device_agent_sdk SYSTEM
  PUBLIC
    ${MOSQUITTO_INCLUDE_DIRS}
)

if(DA_COMPAT_INCLUDE_DIR)
  target_include_directories(device_agent_sdk SYSTEM PUBLIC ${DA_COMPAT_INCLUDE_DIR})
endif()

target_link_directories(device_agent_sdk
  PUBLIC
    ${MOSQUITTO_LIBRARY_DIRS}
)

if(DA_BUILD_VOICE)
  target_link_libraries(device_agent_sdk PUBLIC ${WEBSOCKETS_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)
  target_include_directories(device_agent_sdk SYSTEM PUBLIC ${WEBSOCKETS_INCLUDE_DIRS})
  target_link_directories(device_agent_sdk PUBLIC ${WEBSOCKETS_LIBRARY_DIRS})
  target_compile_definitions(device_agent_sdk PUBLIC DA_VOICE_ENABLED=1)
endif()

if(DA_BUILD_VISION)
  target_link_libraries(device_agent_sdk PUBLIC ${CURL_LIBRARIES})
  target_include_directories(device_agent_sdk SYSTEM PUBLIC ${CURL_INCLUDE_DIRS})
  target_link_directories(device_agent_sdk PUBLIC ${CURL_LIBRARY_DIRS})
  target_compile_definitions(device_agent_sdk PUBLIC DA_VISION_ENABLED=1)
endif()

# --- Generated device executable ------------------------------------------
# src/main.c is generated / customized per device.
# If it exists, build a "device" target automatically.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
  # Digital-pet motor control: build the motor_pwm library when requested and
  # libgpiod is available (Linux target). On hosts without libgpiod (e.g. a
  # macOS dev box) the device still builds, with motor calls stubbed out.
  set(DA_PET_MOTORS_AVAILABLE FALSE)
  if(DA_PET_MOTORS)
    pkg_check_modules(LIBGPIOD libgpiod)
    if(LIBGPIOD_FOUND)
      # The motor_pwm test executable needs GNU-ld --whole-archive; skip it here.
      set(BUILD_TESTS OFF CACHE BOOL "Build motor_pwm tests" FORCE)
      add_subdirectory(motor_pwm)
      set(DA_PET_MOTORS_AVAILABLE TRUE)
    else()
      message(WARNING
        "libgpiod not found -- building digital pet WITHOUT motor control. "
        "Install libgpiod-dev (and build on the target) for real motor support.")
    endif()
  endif()

  add_executable(device src/main.c src/pet_motors.c)
  target_link_libraries(device PRIVATE device_agent_sdk)
  target_include_directories(device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/motor_pwm/include)

  # Expose POSIX symbols (sleep/usleep/etc.) under the strict -std=c11 selected
  # by CMAKE_C_EXTENSIONS OFF; _DARWIN_C_SOURCE keeps BSD typedefs on macOS.
  target_compile_definitions(device PRIVATE _POSIX_C_SOURCE=200809L)
  if(APPLE)
    target_compile_definitions(device PRIVATE _DARWIN_C_SOURCE)
  endif()

  if(DA_PET_MOTORS_AVAILABLE)
    # Force the entire motor archive in so the drivers' self-registration
    # (REGISTER_MOTOR_DRIVER -> __attribute__((constructor))) is retained.
    # Without --whole-archive the linker drops the unreferenced driver objects
    # and motor_alloc_pwm() reports "Driver 'pwm_RoHS' not found". Mirrors the
    # link flags used by motor_pwm's own test target (GNU ld / Linux only,
    # which is the only platform where libgpiod is available).
    target_link_libraries(device PRIVATE
      -Wl,--whole-archive motor -Wl,--no-whole-archive)
    target_compile_definitions(device PRIVATE PET_HAVE_MOTORS=1)
  endif()
endif()

# --- On-board audio driver (for voice_chat) ---------------------------------
# Pulls in the audio/ SpacemitAudio library (PortAudio-based). Only enable on
# the target board where ALSA is available; the portaudio FetchContent build
# forces PA_USE_ALSA and will not succeed on hosts without ALSA.
if(DA_BUILD_VOICE_CHAT_AUDIO)
  enable_language(CXX)
  set(AUDIO_BUILD_PYTHON  OFF CACHE BOOL "" FORCE)
  set(AUDIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory(audio)
endif()

# --- Examples ---------------------------------------------------------------
if(DA_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# --- Install rules ----------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS device_agent_sdk ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
