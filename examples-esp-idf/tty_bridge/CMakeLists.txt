# Project-level CMake for the ESP-IDF build system.
# Build with: idf.py set-target esp32s3 && idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meshcore_tty_bridge)
