add_library(zwayland-client src/display.cpp)

target_include_directories(zwayland-client PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../wire/include>
  $<INSTALL_INTERFACE:include>)

target_link_libraries(zwayland-client PUBLIC zwayland-wire)
target_compile_features(zwayland-client PUBLIC cxx_std_23)
target_compile_options(zwayland-client PRIVATE -Wall -Wextra -Wpedantic)
