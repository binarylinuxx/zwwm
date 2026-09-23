cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/.version" ZWWM_VERSION LIMIT_COUNT 1)
if(NOT ZWWM_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)([-+].*)?$")
  message(FATAL_ERROR "invalid version in .version: ${ZWWM_VERSION}")
endif()
set(ZWWM_PROJECT_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

project(zwwm VERSION "${ZWWM_PROJECT_VERSION}" LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/.version")

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "zwwm requires Clang for both C and C++")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(zwayland-scanner)
add_subdirectory(zwayland)
add_subdirectory(zwwm-protocols)
add_subdirectory(zw-lang)
add_subdirectory(zwwm-renderer)
add_subdirectory(zwwm-egl)
add_subdirectory(xdg-desktop-portal-zwwm)
add_subdirectory(zwwm)
add_subdirectory(zwwmctl)
