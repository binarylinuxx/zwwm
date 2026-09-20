find_package(LibXml2 REQUIRED)

add_executable(zwayland-scanner main.cpp)
target_link_libraries(zwayland-scanner PRIVATE LibXml2::LibXml2)
target_compile_options(zwayland-scanner PRIVATE -Wall -Wextra -Wpedantic -Werror)
