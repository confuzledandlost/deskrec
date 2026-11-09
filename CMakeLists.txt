cmake_minimum_required(VERSION 3.13)
project(deskrec VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DESKREC_WITH_FLAC "Enable FLAC output if libFLAC is found" ON)
option(DESKREC_WITH_MP3 "Enable MP3 output if LAME is found" ON)
option(DESKREC_WITH_GUI "Build the GTK 4 GUI (deskrec-gui) if GTK 4 is found" ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)

# ---------------------------------------------------------------- core

add_library(deskrec_core STATIC
  src/core/recorder.cpp
  src/core/source_watcher.cpp
  src/core/mixer.cpp
  src/core/audio_writer.cpp
  src/core/wav_writer.cpp
  src/core/pulse_common.cpp
)
target_include_directories(deskrec_core PUBLIC src/core)
target_link_libraries(deskrec_core PUBLIC PkgConfig::PULSE)
target_compile_options(deskrec_core PRIVATE -Wall -Wextra)

set(DESKREC_FORMATS "wav")

if(DESKREC_WITH_FLAC)
  pkg_check_modules(FLAC IMPORTED_TARGET flac)
  if(FLAC_FOUND)
    target_sources(deskrec_core PRIVATE src/core/flac_writer.cpp)
    target_link_libraries(deskrec_core PRIVATE PkgConfig::FLAC)
    target_compile_definitions(deskrec_core PRIVATE DESKREC_HAVE_FLAC)
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
    target_sources(deskrec_core PRIVATE src/core/mp3_writer.cpp)
    target_include_directories(deskrec_core PRIVATE ${LAME_INCLUDE_DIR})
    target_link_libraries(deskrec_core PRIVATE ${LAME_LIBRARY})
    target_compile_definitions(deskrec_core PRIVATE DESKREC_HAVE_MP3)
    string(APPEND DESKREC_FORMATS " mp3")
  else()
    message(WARNING "LAME not found; building without MP3 output "
                    "(Debian/Ubuntu: libmp3lame-dev, Fedora: lame-devel, Arch: lame)")
  endif()
endif()

message(STATUS "deskrec output formats: ${DESKREC_FORMATS}")

# ---------------------------------------------------------------- CLI

add_executable(deskrec src/cli/main.cpp)
target_link_libraries(deskrec PRIVATE deskrec_core)
target_compile_options(deskrec PRIVATE -Wall -Wextra)
install(TARGETS deskrec RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------- GUI

if(DESKREC_WITH_GUI)
  pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
  pkg_check_modules(PULSE_GLIB IMPORTED_TARGET libpulse-mainloop-glib)
  if(GTK4_FOUND AND PULSE_GLIB_FOUND)
    add_executable(deskrec-gui src/gui/main.cpp)
    target_link_libraries(deskrec-gui PRIVATE deskrec_core PkgConfig::GTK4 PkgConfig::PULSE_GLIB)
    target_compile_options(deskrec-gui PRIVATE -Wall -Wextra)
    install(TARGETS deskrec-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES data/io.github.confuzledandlost.deskrec.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    message(STATUS "deskrec GUI: enabled")
  else()
    message(WARNING "GTK 4 not found; building without deskrec-gui "
                    "(Debian/Ubuntu: libgtk-4-dev, Fedora: gtk4-devel, Arch: gtk4)")
  endif()
endif()
