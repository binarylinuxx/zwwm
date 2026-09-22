cmake_minimum_required(VERSION 3.25)

project(zwwm VERSION 0.1.1 LANGUAGES C CXX)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "zwwm requires Clang for both C and C++")
endif()

set(ZWWM_VERSION "0.1.1-alpha.1")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_subdirectory(zwayland-scanner)
add_subdirectory(zwayland)
add_subdirectory(zwwm-protocols)
add_subdirectory(zw-lang)
add_subdirectory(zwwm-renderer)
add_subdirectory(zwwm-egl)
add_subdirectory(xdg-desktop-portal-zwwm)
add_subdirectory(zwwm)
add_subdirectory(zwwmctl)
