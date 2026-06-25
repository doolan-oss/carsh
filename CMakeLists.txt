# Host build for carsh — compiles the platform-independent core and host
# backends, and builds the unit-test runner. This is NOT the Pi image build
# (that uses Circle's Makefiles + the AArch64 toolchain). This tree exists so
# the interface-backed logic is provable on the build server.
cmake_minimum_required(VERSION 3.16)
project(carsh_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)

include_directories(
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/host
  ${CMAKE_SOURCE_DIR}/third_party
  ${CMAKE_SOURCE_DIR}/test
)

# Core + host-backend translation units (header-only modules contribute none).
file(GLOB_RECURSE CORE_SRC CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/host/*.cpp
)

# Test translation units.
file(GLOB_RECURSE TEST_SRC CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/tests/*.cpp
)

add_executable(carsh_tests
  ${CMAKE_SOURCE_DIR}/test/Test.cpp
  ${CORE_SRC}
  ${TEST_SRC}
)

enable_testing()
add_test(NAME unit COMMAND carsh_tests)
