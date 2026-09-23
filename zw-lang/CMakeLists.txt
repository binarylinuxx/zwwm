add_library(zw_lang STATIC
  src/config.cpp
)

target_include_directories(zw_lang PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_options(zw_lang PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(zwwm::lang ALIAS zw_lang)
