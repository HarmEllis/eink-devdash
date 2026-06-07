# Host unit-test project for the pure OTA trust-anchor helpers
# (firmware/main/ota_version.c). Built on the ESP-IDF `linux` target so it runs
# on the devcontainer host without flashing hardware. See README.md.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ota_version_host_test)
