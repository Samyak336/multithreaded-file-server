cmake_minimum_required(VERSION 3.16)
project(mtfs
  VERSION 1.0.0
  DESCRIPTION "Multithreaded file server with role-based access control"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Every executable lands in build/bin.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(MTFS_BUILD_TESTS "Build the test suite" ON)
set(MTFS_SANITIZER "" CACHE STRING
    "Sanitizers to build with, e.g. thread or address,undefined (empty = none)")

find_package(Threads REQUIRED)
find_package(OpenSSL 1.1 REQUIRED)

# Compiler settings shared by every target in the project.
add_library(mtfs_options INTERFACE)
target_compile_options(mtfs_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
if(MTFS_SANITIZER)
  target_compile_options(mtfs_options INTERFACE
    -fsanitize=${MTFS_SANITIZER} -fno-omit-frame-pointer)
  target_link_options(mtfs_options INTERFACE -fsanitize=${MTFS_SANITIZER})
endif()

# One directory per module; each builds a static library that the
# executables and tests link against.
add_subdirectory(src/common)
add_subdirectory(src/auth)
add_subdirectory(src/server)
add_subdirectory(src/client)
add_subdirectory(tools)

if(MTFS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS mtfs-server mtfs-client mtfs-user RUNTIME DESTINATION bin)
