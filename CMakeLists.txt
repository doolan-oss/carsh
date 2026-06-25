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

# Vendored single-header libs compiled in their own TU with warnings off — we do
# not police third-party code style, only our own.
file(GLOB THIRDPARTY_SRC CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/third_party/*.cpp
)
add_library(carsh_thirdparty STATIC ${THIRDPARTY_SRC})
target_include_directories(carsh_thirdparty PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_compile_options(carsh_thirdparty PRIVATE -w)

# Deterministic test fixtures (sine WAV/FLAC/MP3 + a small library tree),
# regenerated at build time. Encoders are best-effort (see gen_fixtures.py).
set(FIXTURES_DIR ${CMAKE_BINARY_DIR}/fixtures)
add_custom_target(carsh_fixtures ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${FIXTURES_DIR}
  COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_fixtures.py ${FIXTURES_DIR}
  BYPRODUCTS ${FIXTURES_DIR}/sine.wav
  COMMENT "Generating audio/library fixtures"
  VERBATIM
)

add_executable(carsh_tests
  ${CMAKE_SOURCE_DIR}/test/Test.cpp
  ${CORE_SRC}
  ${TEST_SRC}
)
add_dependencies(carsh_tests carsh_fixtures)
target_link_libraries(carsh_tests PRIVATE carsh_thirdparty)
target_compile_definitions(carsh_tests PRIVATE
  CARSH_FIXTURES_DIR="${FIXTURES_DIR}")

enable_testing()
add_test(NAME unit COMMAND carsh_tests)
