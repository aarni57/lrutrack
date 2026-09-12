cmake_minimum_required(VERSION 3.14)

set(USE_32BIT_KEYS ON CACHE BOOL "Use 32-bit key interface" FORCE)

add_subdirectory(".." "lrutrack")

project(lrutest32 C)
add_executable(${PROJECT_NAME} ../lrutest.c)
target_link_libraries(${PROJECT_NAME} PRIVATE lrutrack)
