cmake_minimum_required(VERSION 3.20)
project(native_display_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(SOURCE_ROOT "../.." ABSOLUTE)
add_subdirectory("${SOURCE_ROOT}/third-party/lizardbyte-common/third-party/googletest" googletest)
add_executable(test_native_display
    "${SOURCE_ROOT}/src/native_display.cpp"
    "${SOURCE_ROOT}/tests/unit/platform/windows/test_native_display.cpp")
target_include_directories(test_native_display PRIVATE "${SOURCE_ROOT}")
target_compile_options(test_native_display PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_native_display PRIVATE gtest_main user32 setupapi cfgmgr32 wtsapi32 uuid)
enable_testing()
add_test(NAME native_display COMMAND test_native_display)
