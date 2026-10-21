cmake_minimum_required(VERSION 3.16)
project(wifi-camera C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
    gstreamer-rtsp-1.0
)

add_executable(wifi-camera wifi_camera.c)
target_compile_options(wifi-camera PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(wifi-camera PRIVATE PkgConfig::GST)
install(TARGETS wifi-camera RUNTIME DESTINATION sbin)
