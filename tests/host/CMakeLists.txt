# Host (PC) unit tests for PicoDoom's hardware-independent logic:
#   cmake -S tests/host -B tests-build && cmake --build tests-build && ctest --test-dir tests-build
# (The firmware's Pico-Toolset also has host tests, third_party/pico-toolset/tests/host.)
cmake_minimum_required(VERSION 3.16)
project(picodoom_host_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
add_executable(picodoom_host_tests test_main.cpp)
target_include_directories(picodoom_host_tests PRIVATE
    ${ROOT}/src
    ${ROOT}/third_party/pico-toolset/components/hid_input/include
)
target_compile_options(picodoom_host_tests PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME picodoom_host_tests COMMAND picodoom_host_tests)
