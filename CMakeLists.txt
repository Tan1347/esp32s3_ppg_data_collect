cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)

# Firmware version: CI generates version.txt before build, local builds use default
if(EXISTS "${CMAKE_SOURCE_DIR}/version.txt")
    file(READ "${CMAKE_SOURCE_DIR}/version.txt" PROJECT_VER)
    string(STRIP "${PROJECT_VER}" PROJECT_VER)
else()
    set(PROJECT_VER "1.0.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(app)
