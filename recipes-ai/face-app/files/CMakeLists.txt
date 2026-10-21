cmake_minimum_required(VERSION 3.16)
project(face-ai LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
    gstreamer-rtsp-1.0
)

find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATH_SUFFIXES onnxruntime
    REQUIRED
)

find_library(ONNXRUNTIME_LIBRARY
    NAMES onnxruntime
    REQUIRED
)

add_executable(face-ai face_ai.cpp)

target_include_directories(face-ai PRIVATE
    ${ONNXRUNTIME_INCLUDE_DIR}
)

target_link_libraries(face-ai PRIVATE
    PkgConfig::GST
    ${ONNXRUNTIME_LIBRARY}
)

target_compile_options(face-ai PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

install(TARGETS face-ai
    RUNTIME DESTINATION bin
)
