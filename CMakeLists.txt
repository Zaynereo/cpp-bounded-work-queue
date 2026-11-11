cmake_minimum_required(VERSION 3.20)
project(cpp_bounded_work_queue VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BOUNDED_QUEUE_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(BOUNDED_QUEUE_ENABLE_SANITIZERS "Enable ASan/UBSan when supported" OFF)
option(BOUNDED_QUEUE_ENABLE_TSAN "Enable ThreadSanitizer when supported" OFF)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  message(FATAL_ERROR "Supported toolchains: GCC 11+ or Clang 14+ (C++20).")
endif()

function(bounded_queue_apply_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
    -Wnull-dereference -Wdouble-promotion -Wformat=2)
  if(BOUNDED_QUEUE_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

if(BOUNDED_QUEUE_ENABLE_TSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    message(FATAL_ERROR "TSan requires Clang or GCC.")
  endif()
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
elseif(BOUNDED_QUEUE_ENABLE_SANITIZERS)
  add_compile_options(
    -fsanitize=address,undefined
    -fno-sanitize-recover=undefined
    -g -O1 -fno-omit-frame-pointer)
  add_link_options(
    -fsanitize=address,undefined
    -fno-sanitize-recover=undefined)
endif()

add_library(bounded_queue INTERFACE)
target_include_directories(bounded_queue INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(bounded_queue INTERFACE cxx_std_20)

add_executable(demo src/demo/main.cpp)
target_link_libraries(demo PRIVATE bounded_queue)
bounded_queue_apply_warnings(demo)

enable_testing()
add_subdirectory(tests)
