cmake_minimum_required(VERSION 3.10)
project(flashjni C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O2 -fPIC)

# flashctl sources are alongside jni_flash.c
set(FLASHCTL_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(flashjni SHARED
    jni_flash.c
    ${FLASHCTL_DIR}/flashctl.c
)

target_include_directories(flashjni PRIVATE
    ${FLASHCTL_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_library(log-lib log)

target_link_libraries(flashjni PRIVATE
    ${log-lib}
)

if (NOT ANDROID)
    target_link_libraries(flashjni PRIVATE pthread)
endif()
