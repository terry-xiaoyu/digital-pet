add_executable(basic_device basic_device.c)
target_link_libraries(basic_device PRIVATE device_agent_sdk)

# voice_chat: push-to-talk over WebSocket. When DA_BUILD_VOICE_CHAT_AUDIO is on
# it links the on-board SpacemitAudio driver (mic/speaker) instead of sox and
# produces bin/voice_chat. The sox-based C version has been removed.
if(DA_BUILD_VOICE)
  if(DA_BUILD_VOICE_CHAT_AUDIO)
    add_executable(voice_chat voice_chat.cpp)
    target_link_libraries(voice_chat PRIVATE
      device_agent_sdk spacemit_audio audio_resampler)
    target_compile_features(voice_chat PRIVATE cxx_std_17)
    target_compile_definitions(voice_chat PRIVATE _POSIX_C_SOURCE=200809L)
    if(APPLE)
      target_compile_definitions(voice_chat PRIVATE _DARWIN_C_SOURCE)
    endif()
    set_target_properties(voice_chat PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  endif()
endif()
