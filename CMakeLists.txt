# CMake 4.x compatibility — allow older policy ranges from dependencies
if(POLICY CMP0200)
  cmake_policy(SET CMP0200 OLD)
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for CMake 4.x compat")
cmake_minimum_required(VERSION 3.15...3.27)
project(AxolotlDB VERSION 0.1.0 LANGUAGES CXX)

# Silence policy warnings
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# UTF-8 source and execution encoding (required for Unicode literals on MSVC)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# Keep vcpkg's machine-wide MSBuild integration out of this build.
#
# `vcpkg integrate install` injects <vcpkg>/installed/<triplet>/include and the
# matching lib directory into EVERY MSBuild project on the machine, ahead of the
# include directories CMake sets. If vcpkg has a package we also pull in via
# FetchContent, the compiler sees vcpkg's headers while the linker links our
# FetchContent build.
#
# With GoogleTest the result is silent and severe: TEST() macros register into
# the vcpkg gtest's singleton while gtest_main's RUN_ALL_TESTS() queries the
# FetchContent one, so every test binary reports "Running 0 tests" and exits 0.
# It links cleanly, so nothing warns you.
#
# This project vendors all of its dependencies through FetchContent, so vcpkg
# has nothing to contribute here. Turn its integration off for our projects.
if(MSVC)
  set(CMAKE_VS_GLOBALS "VcpkgEnabled=false")
endif()

message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "C++ standard: C++${CMAKE_CXX_STANDARD}")

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Options
option(ENABLE_TESTING "Enable testing" ON)
option(ENABLE_BENCHMARKS "Enable benchmarks" ON)
option(ENABLE_COVERAGE "Enable coverage reporting" OFF)
option(ENABLE_EIGEN "Enable Eigen dependency" ON)
option(USE_SYSTEM_LIBS "Use system-installed libraries instead of FetchContent" OFF)
option(BUILD_SERVER "Build the database server" ON)
option(BUILD_CLI "Build the CLI client" ON)

# Compiler flags
if(MSVC)
  add_compile_options(/W4 /permissive-)
  set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Od")
  set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

if(ENABLE_COVERAGE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif()

# Include FetchContent for dependency management
include(FetchContent)

# Set FetchContent base directory
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "FetchContent dependencies directory")

# Find or fetch dependencies
find_package(Threads REQUIRED)

# Boost
if(USE_SYSTEM_LIBS)
  find_package(Boost REQUIRED COMPONENTS system)
else()
  # For FetchContent, we'll use header-only Boost.Asio standalone
  FetchContent_Declare(
    asio
    SYSTEM
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-28-0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(asio)
  add_library(asio INTERFACE)
  target_include_directories(asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
  target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
endif()

# spdlog
if(USE_SYSTEM_LIBS)
  find_package(spdlog REQUIRED)
else()
  FetchContent_Declare(
    spdlog
    SYSTEM
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(spdlog)
endif()

# GoogleTest (only if testing is enabled)
if(ENABLE_TESTING)
  if(USE_SYSTEM_LIBS)
    find_package(GTest REQUIRED)
  else()
    FetchContent_Declare(
      googletest
      SYSTEM
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.14.0
      GIT_SHALLOW TRUE
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  enable_testing()
endif()

# nlohmann/json
if(USE_SYSTEM_LIBS)
  find_package(nlohmann_json REQUIRED)
else()
  FetchContent_Declare(
    json
    SYSTEM
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(json)
endif()

# Eigen (header-only)
#
# Eigen ships a full CMake project with ~900 of its own CTest entries. Pulled in
# unguarded, those register into THIS project's test suite and drown out (and
# outnumber) our own tests -- `ctest` then tries to run executables that were
# never built. Turn its testing/docs/pkgconfig off before making it available,
# and restore BUILD_TESTING afterwards so our tests still register.
if(ENABLE_EIGEN)
  FetchContent_Declare(
    eigen
    SYSTEM
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG 3.4.0
    GIT_SHALLOW TRUE
  )

  set(_AxolotlDB_saved_build_testing ${BUILD_TESTING})
  set(BUILD_TESTING        OFF CACHE BOOL "" FORCE)
  set(EIGEN_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
  set(EIGEN_BUILD_DOC      OFF CACHE BOOL "" FORCE)
  set(EIGEN_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)
  set(EIGEN_BUILD_BLAS     OFF CACHE BOOL "" FORCE)
  set(EIGEN_BUILD_LAPACK   OFF CACHE BOOL "" FORCE)
  set(EIGEN_LEAVE_TEST_IN_ALL_TARGET OFF CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(eigen)

  if(DEFINED _AxolotlDB_saved_build_testing AND NOT _AxolotlDB_saved_build_testing STREQUAL "")
    set(BUILD_TESTING ${_AxolotlDB_saved_build_testing} CACHE BOOL "" FORCE)
  else()
    set(BUILD_TESTING ON CACHE BOOL "" FORCE)
  endif()
endif()

# CLI11 (header-only)
FetchContent_Declare(
  cli11
  SYSTEM
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.3.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cli11)

# yaml-cpp
if(USE_SYSTEM_LIBS)
  find_package(yaml-cpp REQUIRED)
else()
  FetchContent_Declare(
    yaml-cpp
    SYSTEM
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(yaml-cpp)
endif()

if(TARGET yaml-cpp::yaml-cpp)
  set(YAML_CPP_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
  set(YAML_CPP_TARGET yaml-cpp)
else()
  message(FATAL_ERROR "yaml-cpp target not found after dependency setup")
endif()

# Project directories
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Main library
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS 
  "${SRC_DIR}/storage/*.cpp"
  "${SRC_DIR}/parser/*.cpp"
  "${SRC_DIR}/optimizer/*.cpp"
  "${SRC_DIR}/executor/*.cpp"
  "${SRC_DIR}/index/*.cpp"
  "${SRC_DIR}/vector/*.cpp"
  "${SRC_DIR}/catalog/*.cpp"
  "${SRC_DIR}/network/*.cpp"
  "${SRC_DIR}/utils/*.cpp"
)

if(LIB_SOURCES)
  # Sources come from the glob alone. Listing a header here made the glob
  # stop being the single source of truth for what the library contains.
  add_library(AxolotlDB ${LIB_SOURCES}
    include/parser/token.h
    include/utils/diagnosticsink.h
    src/utils/diagnosticsink.cpp
    include/parser/ast.h
    include/parser/ast/stmt.h
    include/parser/ast/expr.h
    include/parser/ast/printer.h
    include/parser/ast/boundstmt.h
    src/parser/binder.cpp
    include/utils/log/transaction.h
    include/optimizer/physical_plan.h)
  target_include_directories(AxolotlDB
    PUBLIC
      $<BUILD_INTERFACE:${INCLUDE_DIR}>
      $<INSTALL_INTERFACE:include>
  )
  
  target_link_libraries(AxolotlDB
    PUBLIC
      Threads::Threads
      spdlog::spdlog
      ${YAML_CPP_TARGET}
      nlohmann_json::nlohmann_json
  )
  if(ENABLE_EIGEN)
    target_link_libraries(AxolotlDB PUBLIC Eigen3::Eigen)
  endif()
  
  if(USE_SYSTEM_LIBS)
    target_link_libraries(AxolotlDB PUBLIC Boost::system)
  else()
    target_link_libraries(AxolotlDB PUBLIC asio)
  endif()
else()
  # If no source files exist yet, create a minimal library
  message(STATUS "No source files found, creating header-only interface library")
  add_library(AxolotlDB INTERFACE)
  target_include_directories(AxolotlDB
    INTERFACE
      $<BUILD_INTERFACE:${INCLUDE_DIR}>
      $<INSTALL_INTERFACE:include>
  )
  
  # Add dependencies to interface library
  target_link_libraries(AxolotlDB
    INTERFACE
      Threads::Threads
      spdlog::spdlog
      ${YAML_CPP_TARGET}
      nlohmann_json::nlohmann_json
  )
  if(ENABLE_EIGEN)
    target_link_libraries(AxolotlDB INTERFACE Eigen3::Eigen)
  endif()
  
  if(USE_SYSTEM_LIBS)
    target_link_libraries(AxolotlDB INTERFACE Boost::system)
  else()
    target_link_libraries(AxolotlDB INTERFACE asio)
  endif()
endif()

# Server executable
if(BUILD_SERVER)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/server/main.cpp)
    add_executable(AxolotlDB-server server/main.cpp)
    target_link_libraries(AxolotlDB-server PRIVATE AxolotlDB)
  else()
    message(STATUS "Server main.cpp not found, skipping server build")
  endif()
endif()

# CLI executable
if(BUILD_CLI)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cli/main.cpp)
    # vector_heap.cpp is already in the AxolotlDB library this target links.
    # Compiling it a second time here is an ODR violation waiting for that
    # file to gain its first non-inline global.
    add_executable(AxolotlDB-cli
      cli/main.cpp
      cli/sql_runner.cpp
      cli/mini_json.cpp
      src/executor/expression.cpp)
    target_link_libraries(AxolotlDB-cli PRIVATE AxolotlDB CLI11::CLI11)
  else()
    message(STATUS "CLI main.cpp not found, skipping CLI build")
  endif()
endif()

# Benchmarks
if(ENABLE_BENCHMARKS)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/vector_bench/allocator_bench.cpp)
    add_executable(allocator_bench benchmarks/vector_bench/allocator_bench.cpp)
    target_link_libraries(allocator_bench PRIVATE AxolotlDB)
  else()
    message(STATUS "allocator_bench source not found, skipping benchmark target")
  endif()

  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/vector_bench/allocate_page_bench.cpp)
    add_executable(allocate_page_bench benchmarks/vector_bench/allocate_page_bench.cpp)
    target_link_libraries(allocate_page_bench PRIVATE AxolotlDB)
  endif()
endif()

# Testing
if(ENABLE_TESTING)
  # tests/CMakeLists.txt owns test registration (labels, gtest_discover_tests).
  # Note the exact spelling: on Linux and in Docker this path is case-sensitive.
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
  else()
    message(FATAL_ERROR
      "ENABLE_TESTING is ON but tests/CMakeLists.txt was not found. "
      "Check the exact filename -- it is case-sensitive outside Windows.")
  endif()
endif()

# Installation
install(TARGETS AxolotlDB
  EXPORT AxolotlDBTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

install(DIRECTORY ${INCLUDE_DIR}/ 
  DESTINATION include
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# Print summary
message(STATUS "")
message(STATUS "=== Build Configuration ===")
message(STATUS "Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "Testing:          ${ENABLE_TESTING}")
message(STATUS "Benchmarks:       ${ENABLE_BENCHMARKS}")
message(STATUS "Coverage:         ${ENABLE_COVERAGE}")
message(STATUS "Eigen:            ${ENABLE_EIGEN}")
message(STATUS "Use system libs:  ${USE_SYSTEM_LIBS}")
message(STATUS "Build server:     ${BUILD_SERVER}")
message(STATUS "Build CLI:        ${BUILD_CLI}")
message(STATUS "============================")
message(STATUS "")
