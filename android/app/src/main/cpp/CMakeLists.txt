cmake_minimum_required(VERSION 3.22.1)

project(ptt_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../..")

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPOSITORY_ROOT}/third_party/opus
                 ${CMAKE_CURRENT_BINARY_DIR}/opus-build
                 EXCLUDE_FROM_ALL)

add_library(ptt_native SHARED
    ptt_jni.c
    ${REPOSITORY_ROOT}/native/src/ptt_audio.c
    ${REPOSITORY_ROOT}/native/src/ptt_common.c
    ${REPOSITORY_ROOT}/native/src/ptt_client.c
)

target_include_directories(ptt_native PRIVATE
    ${REPOSITORY_ROOT}/native/include
)

target_compile_definitions(ptt_native PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(ptt_native PRIVATE -Wall -Wextra -Wpedantic)

find_library(log_library log)
target_link_libraries(ptt_native Opus::opus ${log_library})
