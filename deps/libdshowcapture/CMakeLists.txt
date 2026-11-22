add_library(libdshowcapture INTERFACE)
add_library(OBS::libdshowcapture ALIAS libdshowcapture)

# Keep the pinned upstream checkout pristine. Its filter lookup otherwise
# falls back to a different camera with the same friendly name on hot-unplug.
find_package(Git REQUIRED)
set(strict_source_dir "${CMAKE_CURRENT_BINARY_DIR}/strict-video")
configure_file(src/source/dshow-base.cpp "${strict_source_dir}/dshow-base.cpp" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS strict-video-path.patch)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply --no-index --ignore-space-change "${CMAKE_CURRENT_SOURCE_DIR}/strict-video-path.patch"
  WORKING_DIRECTORY "${strict_source_dir}"
  COMMAND_ERROR_IS_FATAL ANY
)

target_sources(
  libdshowcapture
  INTERFACE src/dshowcapture.hpp
            src/source/capture-filter.cpp
            src/source/capture-filter.hpp
            src/source/device-vendor.cpp
            src/source/device.cpp
            src/source/device.hpp
            "${strict_source_dir}/dshow-base.cpp"
            src/source/dshow-base.hpp
            src/source/dshow-demux.cpp
            src/source/dshow-demux.hpp
            src/source/dshow-device-defs.hpp
            src/source/dshow-encoded-device.cpp
            src/source/dshow-enum.cpp
            src/source/dshow-enum.hpp
            src/source/dshow-formats.cpp
            src/source/dshow-formats.hpp
            src/source/dshow-media-type.cpp
            src/source/dshow-media-type.hpp
            src/source/dshowcapture.cpp
            src/source/dshowencode.cpp
            src/source/encoder.cpp
            src/source/encoder.hpp
            src/source/external/IVideoCaptureFilter.h
            src/source/log.cpp
            src/source/log.hpp
            src/source/output-filter.cpp
            src/source/output-filter.hpp
            src/external/capture-device-support/Library/EGAVResult.cpp
            src/external/capture-device-support/Library/ElgatoUVCDevice.cpp
            src/external/capture-device-support/Library/win/EGAVHIDImplementation.cpp
            src/external/capture-device-support/SampleCode/DriverInterface.cpp)

target_include_directories(
  libdshowcapture INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/source"
                            "${CMAKE_CURRENT_SOURCE_DIR}/src/external/capture-device-support/Library")

target_compile_definitions(libdshowcapture INTERFACE _UP_WINDOWS=1)
target_compile_options(libdshowcapture INTERFACE /wd4005 /wd4018)
