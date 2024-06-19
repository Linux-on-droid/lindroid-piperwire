cmake_minimum_required(VERSION 3.10)
project(audio_socket_pipewire)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

add_library(pipewire-module-lindroid SHARED module-lindroid.c)
target_include_directories(pipewire-module-lindroid  PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
target_link_libraries(pipewire-module-lindroid  PRIVATE ${PIPEWIRE_LIBRARIES})
