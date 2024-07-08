cmake_minimum_required(VERSION 3.10)
project(lindroid LANGUAGES C)

# PipeWire and SPA paths
set(PIPEWIRE_INCLUDE_DIR /usr/include/pipewire-0.3)
set(SPA_INCLUDE_DIR /usr/include/spa-0.2)

# Include directories
include_directories(${PIPEWIRE_INCLUDE_DIR} ${SPA_INCLUDE_DIR} include)

# Source files
set(SOURCES module-lindroid.c)

# Create shared library
add_library(pipewire-module-lindroid SHARED ${SOURCES})
target_link_libraries(pipewire-module-lindroid pipewire-0.3)
set_target_properties(pipewire-module-lindroid PROPERTIES
    OUTPUT_NAME "pipewire-module-lindroid"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/pipewire-0.3")
