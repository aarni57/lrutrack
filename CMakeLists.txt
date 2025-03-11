cmake_minimum_required(VERSION 3.14)

project(slru)

set(SOURCE_FILES
   slru.c
   slru.h
)

add_library(${PROJECT_NAME} ${SOURCE_FILES})
