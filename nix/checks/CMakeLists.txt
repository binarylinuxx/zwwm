cmake_minimum_required(VERSION 3.25)
project(zwayland-component-consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(zwayland CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZWAYLAND REQUIRED IMPORTED_TARGET zwayland-client zwayland-server)
pkg_check_modules(PROTOCOLS REQUIRED zwwm-protocols)
pkg_check_modules(SCANNER REQUIRED zwayland-scanner)
pkg_get_variable(protocol_dir zwwm-protocols pkgdatadir)
pkg_get_variable(scanner zwayland-scanner scanner)

foreach(side client server)
  set(header "${CMAKE_CURRENT_BINARY_DIR}/tags-zwayland-${side}.h")
  add_custom_command(OUTPUT "${header}"
    COMMAND "${scanner}" "zwayland-${side}-header"
      "${protocol_dir}/zwwm-tags-unstable-v1.xml" "${header}"
    DEPENDS "${protocol_dir}/zwwm-tags-unstable-v1.xml" VERBATIM)
  list(APPEND headers "${header}")
endforeach()

foreach(kind cmake pkgconfig)
  add_executable(${kind}-consumer component-consumer.cpp server-consumer.cpp ${headers})
  target_include_directories(${kind}-consumer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
target_link_libraries(cmake-consumer PRIVATE zwayland::client zwayland::server)
target_link_libraries(pkgconfig-consumer PRIVATE PkgConfig::ZWAYLAND)
