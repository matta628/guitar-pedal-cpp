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
  src/Distortion.cpp
  src/Delay.cpp
  src/Chorus.cpp
  src/Reverb.cpp
  src/Looper.cpp
  src/ClickDetector.cpp
  src/LedPattern.cpp
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

add_executable(guitar_pedal src/main.cpp)
target_link_libraries(guitar_pedal PRIVATE pedal_dsp PkgConfig::RTAUDIO)

enable_testing()
add_executable(offline_tests tests/offline_tests.cpp)
target_link_libraries(offline_tests PRIVATE pedal_dsp)
add_test(NAME offline_tests COMMAND offline_tests)
