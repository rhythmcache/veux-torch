cmake_minimum_required(VERSION 3.10)
project(flashctl C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Strict warnings and optimizations
add_compile_options(-Wall -Wextra -O2 -fPIC)

# 1. Shared Library: libflashctl.so
add_library(flashctl SHARED
    flashctl.c
)

target_include_directories(flashctl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# Android log library support
if(ANDROID)
    find_library(log-lib log)
    target_link_libraries(flashctl PRIVATE ${log-lib})
endif()

# Threading support
target_link_libraries(flashctl PRIVATE pthread)
