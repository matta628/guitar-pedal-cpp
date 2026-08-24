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
)
target_include_directories(pedal_dsp PUBLIC include)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  pkg_check_modules(GPIOD IMPORTED_TARGET libgpiod)
  if(GPIOD_FOUND)
    target_sources(pedal_dsp PRIVATE src/GpioButton.cpp)
    target_link_libraries(pedal_dsp PUBLIC PkgConfig::GPIOD)
    target_compile_definitions(pedal_dsp PUBLIC PEDAL_HAVE_GPIO)
  endif()
endif()

add_executable(guitar_pedal src/main.cpp)
target_link_libraries(guitar_pedal PRIVATE pedal_dsp PkgConfig::RTAUDIO)

enable_testing()
add_executable(offline_tests tests/offline_tests.cpp)
target_link_libraries(offline_tests PRIVATE pedal_dsp)
add_test(NAME offline_tests COMMAND offline_tests)
