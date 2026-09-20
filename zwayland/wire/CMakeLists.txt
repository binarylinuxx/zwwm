add_library(zwayland-wire
  src/message.cpp
  src/connection.cpp)

target_include_directories(zwayland-wire PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

target_compile_features(zwayland-wire PUBLIC cxx_std_23)
target_compile_options(zwayland-wire PRIVATE -Wall -Wextra -Wpedantic)
