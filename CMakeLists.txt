cmake_minimum_required(VERSION 3.13)
project(deskrec VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)

add_executable(deskrec
  src/main.cpp
  src/list.cpp
  src/recorder.cpp
  src/mixer.cpp
  src/wav_writer.cpp
  src/pulse_common.cpp
)
target_link_libraries(deskrec PRIVATE PkgConfig::PULSE)
target_compile_options(deskrec PRIVATE -Wall -Wextra)

install(TARGETS deskrec RUNTIME DESTINATION bin)
