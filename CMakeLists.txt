cmake_minimum_required(VERSION 3.16)

project(sip_ptt_demo C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(ptt_common STATIC
    src/ptt_common.c
)
target_include_directories(ptt_common PUBLIC include)
target_compile_definitions(ptt_common PRIVATE _POSIX_C_SOURCE=200809L)

add_executable(ptt_server
    src/ptt_server.c
)
target_link_libraries(ptt_server PRIVATE ptt_common)
target_compile_definitions(ptt_server PRIVATE _POSIX_C_SOURCE=200809L)

add_executable(ptt_client
    src/ptt_client.c
)
target_link_libraries(ptt_client PRIVATE ptt_common)
target_compile_definitions(ptt_client PRIVATE _POSIX_C_SOURCE=200809L)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ptt_common PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ptt_server PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ptt_client PRIVATE -Wall -Wextra -Wpedantic)
endif()

