cmake_minimum_required(VERSION 3.18)
project(guitar_pedal_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

option(ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(RTAUDIO REQUIRED IMPORTED_TARGET rtaudio)

add_library(pedal_dsp STATIC
  src/Amp.cpp
  src/Biquad.cpp
  src/BitCrusher.cpp
  src/Chorus.cpp
  src/ClickDetector.cpp
  src/Compressor.cpp
  src/Delay.cpp
  src/Distortion.cpp
  src/EnvFilter.cpp
  src/Flanger.cpp
  src/Freeze.cpp
  src/Fuzz.cpp
  src/LedPattern.cpp
  src/Looper.cpp
  src/Pedalboard.cpp
  src/Phaser.cpp
  src/PitchShifter.cpp
  src/PresetStats.cpp
  src/Presets.cpp
  src/Reverb.cpp
  src/Setlist.cpp
  src/RingMod.cpp
  src/Telemetry.cpp
  src/Tone.cpp
  src/Tremolo.cpp
  src/WaveFolder.cpp
)
target_include_directories(pedal_dsp PUBLIC include)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(GPIOD IMPORTED_TARGET libgpiod)
  if(GPIOD_FOUND)
    target_sources(pedal_dsp PRIVATE src/GpioButton.cpp src/GpioLed.cpp src/Lcd1602.cpp)
    target_link_libraries(pedal_dsp PUBLIC PkgConfig::GPIOD)
    target_compile_definitions(pedal_dsp PUBLIC PEDAL_HAVE_GPIO)

    # Bench diagnostic for the GPIO wiring. Only exists where libgpiod does,
    # and needs no audio interface, so it can run on a bare board.
    add_executable(gpio_check tools/gpio_check.cpp)
    target_link_libraries(gpio_check PRIVATE pedal_dsp)
  endif()
endif()

# Device enumeration and rate negotiation, shared by the pedal and the bench
# tool. Kept out of pedal_dsp so the offline test binary still links without
# RtAudio anywhere near it.
add_library(pedal_audio STATIC src/AudioDevice.cpp)
target_link_libraries(pedal_audio PUBLIC pedal_dsp PkgConfig::RTAUDIO)

# The dev UI is compiled into the binary rather than read from disk at runtime:
# one file to copy to the Pi, and no --ui-dir to get wrong. Editing
# web/index.html re-runs this and rebuilds.
set(UI_PAGE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/UiPage.h)
add_custom_command(
  OUTPUT ${UI_PAGE_HEADER}
  COMMAND ${CMAKE_COMMAND}
          -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
          -DOUTPUT=${UI_PAGE_HEADER}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_html.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_html.cmake
  COMMENT "Embedding web/index.html into UiPage.h"
  VERBATIM)

add_executable(guitar_pedal src/main.cpp src/WebServer.cpp ${UI_PAGE_HEADER})
target_include_directories(guitar_pedal PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(guitar_pedal PRIVATE pedal_audio)

# Bench diagnostic for the audio path. Needs RtAudio but no GPIO, so it builds
# on the dev machine too — unlike gpio_check, which only exists where libgpiod
# does.
add_executable(audio_check tools/audio_check.cpp)
target_link_libraries(audio_check PRIVATE pedal_audio)

enable_testing()
add_executable(offline_tests tests/offline_tests.cpp)
target_link_libraries(offline_tests PRIVATE pedal_dsp)
add_test(NAME offline_tests COMMAND offline_tests)
