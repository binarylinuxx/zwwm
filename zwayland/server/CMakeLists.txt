add_library(zwayland-server src/display.cpp src/event_loop.cpp)

target_include_directories(zwayland-server PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../wire/include>
  $<INSTALL_INTERFACE:include>)

target_link_libraries(zwayland-server PUBLIC zwayland-wire)
target_compile_features(zwayland-server PUBLIC cxx_std_23)
target_compile_options(zwayland-server PRIVATE -Wall -Wextra -Wpedantic)
