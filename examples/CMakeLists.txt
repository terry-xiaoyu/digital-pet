add_executable(basic_device basic_device.c)
target_link_libraries(basic_device PRIVATE device_agent_sdk)

if(DA_BUILD_VOICE)
  add_executable(voice_chat voice_chat.c)
  target_link_libraries(voice_chat PRIVATE device_agent_sdk)
endif()
