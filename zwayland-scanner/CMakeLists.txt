cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(zwayland-scanner LANGUAGES CXX)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "zwayland-scanner requires Clang")
  endif()
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

find_package(LibXml2 REQUIRED)

add_executable(zwayland-scanner main.cpp)
target_link_libraries(zwayland-scanner PRIVATE LibXml2::LibXml2)
target_compile_options(zwayland-scanner PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(GNUInstallDirs)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../.version" ZWAYLAND_VERSION LIMIT_COUNT 1)
  configure_file(zwayland-scanner.pc.in zwayland-scanner.pc @ONLY)
  install(TARGETS zwayland-scanner RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/zwayland-scanner.pc"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pkgconfig")
  install(FILES ../LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/zwayland-scanner")
endif()
