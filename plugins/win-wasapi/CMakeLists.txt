cmake_minimum_required(VERSION 3.28...3.30)

add_library(win-wasapi MODULE)
add_library(OBS::wasapi ALIAS win-wasapi)

target_sources(
  win-wasapi
  PRIVATE win-wasapi.cpp wasapi-notify.cpp wasapi-notify.hpp enum-wasapi.cpp enum-wasapi.hpp plugin-main.cpp
)

configure_file(cmake/windows/obs-module.rc.in win-wasapi.rc)
target_sources(win-wasapi PRIVATE win-wasapi.rc)

target_link_libraries(win-wasapi PRIVATE OBS::libobs Avrt)

if(ENABLE_VOIDGLASS_AUDIO_TRACE)
  target_sources(win-wasapi PRIVATE ../voidglass-audio-trace/audio-trace.c ../voidglass-audio-trace/audio-trace.h)
  target_compile_definitions(win-wasapi PRIVATE VOIDGLASS_AUDIO_TRACE)
endif()

set_target_properties_obs(win-wasapi PROPERTIES FOLDER plugins PREFIX "")
