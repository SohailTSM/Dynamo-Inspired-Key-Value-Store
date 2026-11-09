cmake_minimum_required(VERSION 3.20)
project(dynamo_kv LANGUAGES CXX)

# C++17: RCE's gRPC module is built with GCC 8.5 in C++17 mode (LLD §1, decision D8).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DKV_BUILD_TESTS "Build unit tests (dkv_tests)" ON)
set(DKV_SANITIZE "" CACHE STRING "Sanitizer to enable: thread | address | (empty)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(DKV_SANITIZE)
  add_compile_options(-fsanitize=${DKV_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${DKV_SANITIZE})
endif()

include(cmake/Dependencies.cmake)

# Warnings for our own code only (generated protobuf/gRPC code is excluded).
function(dkv_target_defaults target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/src)
endfunction()

add_subdirectory(proto)
add_subdirectory(src)
if(DKV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
