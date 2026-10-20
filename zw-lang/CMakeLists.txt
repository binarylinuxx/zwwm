cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(zw-lang LANGUAGES CXX)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "zw-lang requires Clang")
  endif()
endif()

add_library(zw_lang
  src/config.cpp
)

target_include_directories(zw_lang PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(zw_lang PUBLIC cxx_std_23)
target_compile_options(zw_lang PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(zwwm::lang ALIAS zw_lang)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../.version" ZW_LANG_VERSION LIMIT_COUNT 1)
  set_target_properties(zw_lang PROPERTIES EXPORT_NAME lang)
  configure_package_config_file(zw-langConfig.cmake.in zw-langConfig.cmake
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zw-lang")
  configure_file(zw-lang.pc.in zw-lang.pc @ONLY)
  install(TARGETS zw_lang EXPORT zw-langTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(EXPORT zw-langTargets NAMESPACE zwwm:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zw-lang")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/zw-langConfig.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zw-lang")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/zw-lang.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
  install(FILES ../LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/zw-lang")
endif()
