add_executable(zwwmctl main.cpp)
target_link_libraries(zwwmctl PRIVATE zwayland-client zwwm::protocol_metadata)
target_compile_options(zwwmctl PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(zwwmctl PRIVATE ZWWM_VERSION="${ZWWM_VERSION}")

include(GNUInstallDirs)
install(TARGETS zwwmctl RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
