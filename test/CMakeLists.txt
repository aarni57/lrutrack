cmake_minimum_required(VERSION 3.14)

add_subdirectory(".." "slru")

project(slrutest)

include_directories(. ..)

set(SOURCE_FILES
    slrutest.c
)

add_executable(${PROJECT_NAME} ${SOURCE_FILES})

target_link_libraries(${PROJECT_NAME} slru)
