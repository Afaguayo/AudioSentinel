cmake_minimum_required(VERSION 3.16)
project(AudioSentinel VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Shared logic (exposure model, alerts, settings, Spotify parsing).
add_library(as_core STATIC
  core/exposure.cpp
  core/settings.cpp
  core/spotify.cpp
  core/dashboard.cpp)
target_include_directories(as_core PUBLIC core)
if(MSVC)
  target_compile_options(as_core PRIVATE /W4 /WX)
else()
  target_compile_options(as_core PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE as_core)
add_test(NAME core_tests COMMAND core_tests)

# The Windows app is built with AudioSentinel.sln; CMake builds the Linux app.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  find_package(Threads REQUIRED)
  pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
  pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse libpulse-simple)
  pkg_check_modules(NOTIFY REQUIRED IMPORTED_TARGET libnotify)
  pkg_check_modules(INDICATOR IMPORTED_TARGET ayatana-appindicator3-0.1)

  add_executable(audiosentinel
    linux/main.cpp
    linux/pulse_audio.cpp
    linux/mpris.cpp
    linux/draw.cpp)
  target_link_libraries(audiosentinel PRIVATE
    as_core PkgConfig::GTK PkgConfig::PULSE PkgConfig::NOTIFY Threads::Threads)
  target_compile_definitions(audiosentinel PRIVATE AS_VERSION="${PROJECT_VERSION}")
  target_compile_options(audiosentinel PRIVATE -Wall -Wextra)

  if(INDICATOR_FOUND)
    target_link_libraries(audiosentinel PRIVATE PkgConfig::INDICATOR)
    target_compile_definitions(audiosentinel PRIVATE HAVE_APPINDICATOR)
  else()
    message(WARNING "libayatana-appindicator3 not found: building without a tray icon "
                    "(closing the window will quit the app)")
  endif()

  include(GNUInstallDirs)
  install(TARGETS audiosentinel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES linux/io.github.afaguayo.AudioSentinel.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES linux/audiosentinel.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
endif()
