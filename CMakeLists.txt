# CMakeLists.txt -- Linux / native (host) build for the portable C core.
#
# This builds the dependency-free C99 core (src/meshcore_companion.c), the host
# unit test, and the Linux tty example. It deliberately does NOT build the C++
# Arduino wrapper (src/MeshCoreCompanion.{h,cpp}) -- that requires <Arduino.h>
# and is compiled by the Arduino IDE / PlatformIO instead. The Arduino manifests
# (library.properties, library.json) at the repo root are simply ignored here.
#
#   cmake -B build && cmake --build build
#   ctest --test-dir build --output-on-failure
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.13)
project(meshcore_c LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

# Portable protocol core, reusable on any platform with a byte transport.
add_library(meshcore_companion STATIC src/meshcore_companion.c)
target_include_directories(meshcore_companion PUBLIC src)

# Linux example: drives a companion radio over any tty (USB-CDC or raw UART).
add_executable(meshcore_tty examples-linux/tty_bridge/meshcore_tty.c)
target_link_libraries(meshcore_tty PRIVATE meshcore_companion)

# Host unit test for the codec (no hardware required).
enable_testing()
add_executable(test_codec test/test_codec.c)
target_link_libraries(test_codec PRIVATE meshcore_companion)
add_test(NAME codec COMMAND test_codec)
