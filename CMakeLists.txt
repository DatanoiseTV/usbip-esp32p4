# USB/IP Server for ESP32-P4-Nano
# ESP-IDF v5.5 project
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usbip-esp32p4)
