add_library(zw_lang STATIC
  src/config.cpp
)

target_include_directories(zw_lang PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_options(zw_lang PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(zwwm::lang ALIAS zw_lang)

if(BUILD_TESTING)
  add_executable(zw_lang_tests tests/config_test.cpp)
  target_link_libraries(zw_lang_tests PRIVATE zwwm::lang)
  target_compile_definitions(zw_lang_tests PRIVATE
    ZW_LANG_EXAMPLE_PATH="${PROJECT_SOURCE_DIR}/zwwm/data/config.zw"
  )
  target_compile_options(zw_lang_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME zw_lang_tests COMMAND zw_lang_tests)
endif()
