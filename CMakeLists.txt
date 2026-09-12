cmake_minimum_required(VERSION 3.14)

project(lrutrack C)

option(USE_32BIT_KEYS "Use 32-bit key interface" OFF)
option(ENABLE_HC_TESTS "Enable HC tests" OFF)

add_compile_options(
    -Wall
    -Wextra
    -Wsign-compare
    -Wundef
    -pedantic
    -Werror
    -Wno-nonportable-include-path
    -Wno-unused-function
)

add_compile_options($<$<COMPILE_LANGUAGE:C>:-std=c23>)

set(SOURCE_FILES
    lrutrack.c
    lrutrack.h
)

add_library(${PROJECT_NAME} ${SOURCE_FILES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(${PROJECT_NAME} PUBLIC
    LRUTRACK_32BIT_KEY=$<BOOL:${USE_32BIT_KEYS}>
    LRUTRACK_HC_TESTS=$<BOOL:${ENABLE_HC_TESTS}>)
