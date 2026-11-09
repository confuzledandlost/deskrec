cmake_minimum_required(VERSION 3.13)
project(deskrec VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DESKREC_WITH_FLAC "Enable FLAC output if libFLAC is found" ON)
option(DESKREC_WITH_MP3 "Enable MP3 output if LAME is found" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)

add_executable(deskrec
  src/main.cpp
  src/list.cpp
  src/recorder.cpp
  src/mixer.cpp
  src/audio_writer.cpp
  src/wav_writer.cpp
  src/pulse_common.cpp
)
target_link_libraries(deskrec PRIVATE PkgConfig::PULSE)
target_compile_options(deskrec PRIVATE -Wall -Wextra)

set(DESKREC_FORMATS "wav")

if(DESKREC_WITH_FLAC)
  pkg_check_modules(FLAC IMPORTED_TARGET flac)
  if(FLAC_FOUND)
    target_sources(deskrec PRIVATE src/flac_writer.cpp)
    target_link_libraries(deskrec PRIVATE PkgConfig::FLAC)
    target_compile_definitions(deskrec PRIVATE DESKREC_HAVE_FLAC)
    string(APPEND DESKREC_FORMATS " flac")
  else()
    message(WARNING "libFLAC not found; building without FLAC output "
                    "(Debian/Ubuntu: libflac-dev, Fedora: flac-devel, Arch: flac)")
  endif()
endif()

if(DESKREC_WITH_MP3)
  # LAME ships no pkg-config file on most distros.
  find_path(LAME_INCLUDE_DIR lame/lame.h)
  find_library(LAME_LIBRARY mp3lame)
  if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
    target_sources(deskrec PRIVATE src/mp3_writer.cpp)
    target_include_directories(deskrec PRIVATE ${LAME_INCLUDE_DIR})
    target_link_libraries(deskrec PRIVATE ${LAME_LIBRARY})
    target_compile_definitions(deskrec PRIVATE DESKREC_HAVE_MP3)
    string(APPEND DESKREC_FORMATS " mp3")
  else()
    message(WARNING "LAME not found; building without MP3 output "
                    "(Debian/Ubuntu: libmp3lame-dev, Fedora: lame-devel, Arch: lame)")
  endif()
endif()

message(STATUS "deskrec output formats: ${DESKREC_FORMATS}")

install(TARGETS deskrec RUNTIME DESTINATION bin)
