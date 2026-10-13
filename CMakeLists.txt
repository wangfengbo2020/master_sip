cmake_minimum_required(VERSION 3.16)

project(sip_ptt_demo C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus third_party/opus-build EXCLUDE_FROM_ALL)

add_library(ptt_common STATIC
    native/src/ptt_common.c
)
target_include_directories(ptt_common PUBLIC native/include)
target_compile_definitions(ptt_common PRIVATE _POSIX_C_SOURCE=200809L)

add_library(ptt_audio STATIC
    native/src/ptt_audio.c
)
target_include_directories(ptt_audio PUBLIC native/include)
target_link_libraries(ptt_audio PUBLIC Opus::opus)

add_library(ptt_client_core STATIC
    native/src/ptt_client.c
)
target_include_directories(ptt_client_core PUBLIC native/include)
target_link_libraries(ptt_client_core PUBLIC ptt_common)
target_compile_definitions(ptt_client_core PRIVATE _POSIX_C_SOURCE=200809L)

add_executable(ptt_server
    apps/server/ptt_server.c
)
target_link_libraries(ptt_server PRIVATE ptt_common)
target_compile_definitions(ptt_server PRIVATE _POSIX_C_SOURCE=200809L)

add_executable(ptt_client
    apps/pc/ptt_client_main.c
    apps/pc/ptt_pc_audio.c
)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE_SIMPLE REQUIRED IMPORTED_TARGET libpulse-simple)
find_package(Threads REQUIRED)
target_link_libraries(ptt_client PRIVATE
    ptt_client_core
    ptt_audio
    PkgConfig::PULSE_SIMPLE
    Threads::Threads
)
target_compile_definitions(ptt_client PRIVATE _POSIX_C_SOURCE=200809L)

option(PTT_BUILD_TESTS "Build PTT unit tests" ON)
if(PTT_BUILD_TESTS)
    enable_testing()
    add_executable(ptt_audio_test tests/ptt_audio_test.c)
    target_link_libraries(ptt_audio_test PRIVATE ptt_audio ptt_common)
    add_test(NAME ptt_audio_codec COMMAND ptt_audio_test)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ptt_common PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ptt_audio PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ptt_client_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ptt_server PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ptt_client PRIVATE -Wall -Wextra -Wpedantic)
    if(PTT_BUILD_TESTS)
        target_compile_options(ptt_audio_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

