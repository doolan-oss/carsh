# Host build for carsh — compiles the platform-independent core and host
# backends, and builds the unit-test runner. This is NOT the Pi image build
# (that uses Circle's Makefiles + the AArch64 toolchain). This tree exists so
# the interface-backed logic is provable on the build server.
cmake_minimum_required(VERSION 3.16)
project(carsh_host CXX C)

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
  ${CMAKE_SOURCE_DIR}/device
)

# The device OTA crypto backend (TweetNaCl + SHA-256) is platform-independent
# C++; compile it into the host build so it can be cross-validated vs OpenSSL.
set(DEVICE_HOSTABLE_SRC)
if(EXISTS ${CMAKE_SOURCE_DIR}/device/DeviceCrypto.cpp)
  list(APPEND DEVICE_HOSTABLE_SRC ${CMAKE_SOURCE_DIR}/device/DeviceCrypto.cpp)
endif()

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
# Vendored C crypto used by the device OTA backend — compiled into the host build
# too so it can be cross-validated against OpenSSL (the device path is otherwise
# unobservable without hardware).
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c)
  list(APPEND THIRDPARTY_SRC
    ${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c
    ${CMAKE_SOURCE_DIR}/third_party/sha256/sha256.c)
endif()
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
  ${DEVICE_HOSTABLE_SRC}
  ${TEST_SRC}
)
add_dependencies(carsh_tests carsh_fixtures)
target_link_libraries(carsh_tests PRIVATE carsh_thirdparty)
target_compile_definitions(carsh_tests PRIVATE
  CARSH_FIXTURES_DIR="${FIXTURES_DIR}"
  CARSH_TEST_DIR="${CMAKE_SOURCE_DIR}/tests")

# Optional: real Opus decode via system libopusfile if present on the build
# server. The bare-metal port is separate; this proves the wrapper + factory.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(OPUSFILE QUIET opusfile)
endif()
if(OPUSFILE_FOUND)
  message(STATUS "carsh: libopusfile found — Opus decoder enabled")
  target_compile_definitions(carsh_tests PRIVATE CARSH_HAVE_OPUS=1)
  target_include_directories(carsh_tests PRIVATE ${OPUSFILE_INCLUDE_DIRS})
  target_link_libraries(carsh_tests PRIVATE ${OPUSFILE_LIBRARIES})
else()
  message(STATUS "carsh: libopusfile not found — Opus decoder skipped (stub)")
endif()

# Optional: real AAC (ADTS) decode via system faad2.
find_library(FAAD_LIB NAMES faad)
find_path(FAAD_INC NAMES neaacdec.h)
if(FAAD_LIB AND FAAD_INC)
  message(STATUS "carsh: faad2 found — AAC (ADTS) decoder enabled")
  target_compile_definitions(carsh_tests PRIVATE CARSH_HAVE_AAC=1)
  target_include_directories(carsh_tests PRIVATE ${FAAD_INC})
  target_link_libraries(carsh_tests PRIVATE ${FAAD_LIB})
else()
  message(STATUS "carsh: faad2 not found — AAC decoder skipped (stub)")
endif()

# OpenSSL (libcrypto) for the host OTA crypto backend (real Ed25519 + SHA-256).
# The device build uses an mbed TLS ICrypto implementation instead.
if(PkgConfig_FOUND)
  pkg_check_modules(LIBCRYPTO QUIET libcrypto)
endif()
if(LIBCRYPTO_FOUND)
  message(STATUS "carsh: libcrypto found — host OTA crypto enabled")
  target_compile_definitions(carsh_tests PRIVATE CARSH_HAVE_OPENSSL=1)
  target_include_directories(carsh_tests PRIVATE ${LIBCRYPTO_INCLUDE_DIRS})
  target_link_libraries(carsh_tests PRIVATE ${LIBCRYPTO_LIBRARIES})
else()
  message(STATUS "carsh: libcrypto not found — OTA crypto tests skipped")
endif()

# Optional: host-test the device's mbedTLS AA crypto (device/AaCryptoMbedTls.cpp)
# against the system mbedTLS. circle-stdlib bundles mbedTLS 2.28 and Ubuntu's
# libmbedtls-dev is the same 2.28 line, so the in-memory TLS 1.2 handshake +
# record round-trip the device path performs is genuinely exercised on the build
# server. The LIVE handshake against a real phone (and the Google-registered cert)
# stays [hw]. Without mbedTLS the test compiles to nothing (see the #ifdef guard).
find_library(MBEDTLS_LIB NAMES mbedtls)
find_library(MBEDX509_LIB NAMES mbedx509)
find_library(MBEDCRYPTO_LIB NAMES mbedcrypto)
find_path(MBEDTLS_INC NAMES mbedtls/ssl.h)
if(MBEDTLS_LIB AND MBEDX509_LIB AND MBEDCRYPTO_LIB AND MBEDTLS_INC)
  message(STATUS "carsh: mbedTLS found — AA TLS crypto host test enabled")
  target_sources(carsh_tests PRIVATE ${CMAKE_SOURCE_DIR}/device/AaCryptoMbedTls.cpp)
  target_compile_definitions(carsh_tests PRIVATE CARSH_HAVE_MBEDTLS=1)
  target_include_directories(carsh_tests PRIVATE ${MBEDTLS_INC})
  # Link order matters: mbedtls -> mbedx509 -> mbedcrypto.
  target_link_libraries(carsh_tests PRIVATE ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})
else()
  message(STATUS "carsh: mbedTLS not found — AA TLS crypto host test skipped (stub)")
endif()

enable_testing()
add_test(NAME unit COMMAND carsh_tests)
