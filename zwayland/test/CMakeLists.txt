add_executable(zwayland-foundation-test roundtrip.cpp)
target_link_libraries(zwayland-foundation-test PRIVATE zwayland-server zwayland-client)
target_compile_features(zwayland-foundation-test PRIVATE cxx_std_23)
target_compile_options(zwayland-foundation-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME zwayland-foundation-roundtrip COMMAND zwayland-foundation-test)

add_executable(zwayland-loopback-test loopback.cpp)
target_link_libraries(zwayland-loopback-test PRIVATE zwayland-server zwayland-client)
target_include_directories(zwayland-loopback-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../loopback/include)
target_compile_features(zwayland-loopback-test PRIVATE cxx_std_23)
target_compile_options(zwayland-loopback-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME zwayland-loopback COMMAND zwayland-loopback-test)

add_executable(zwayland-server-core-test server_core.cpp)
target_link_libraries(zwayland-server-core-test PRIVATE zwayland-server zwayland-client)
target_include_directories(zwayland-server-core-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../loopback/include)
target_compile_features(zwayland-server-core-test PRIVATE cxx_std_23)
target_compile_options(zwayland-server-core-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME zwayland-server-core COMMAND zwayland-server-core-test)

add_executable(zwayland-bench-loopback bench_loopback.cpp)
target_link_libraries(zwayland-bench-loopback PRIVATE zwayland-server zwayland-client)
target_include_directories(zwayland-bench-loopback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../loopback/include)
target_compile_features(zwayland-bench-loopback PRIVATE cxx_std_23)
target_compile_options(zwayland-bench-loopback PRIVATE -Wall -Wextra -Wpedantic)


