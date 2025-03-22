cmake_minimum_required(VERSION 3.14)

project(lrutrack)

set(SOURCE_FILES
   lrutrack.c
   lrutrack.h
)

add_library(${PROJECT_NAME} ${SOURCE_FILES})
