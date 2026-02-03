# Tests configuration for AxolotlDB

include(GoogleTest)

# Helper function to add tests
function(add_AxolotlDB_test test_name test_label)
  add_executable(${test_name} ${ARGN})
  
  target_link_libraries(${test_name}
    PRIVATE
      AxolotlDB
      GTest::gtest
      GTest::gtest_main
  )
  
  # POST_BUILD, not PRE_TEST.
  #
  # With PRE_TEST the test list -- and therefore the LABELS -- does not exist
  # until ctest runs its discovery pass, so `ctest -L unit` and
  # `ctest -L integration` match nothing and exit 8. CI relies on those labels.
  # POST_BUILD registers each TEST() case at build time, labels included.
  gtest_discover_tests(${test_name}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES LABELS "${test_label}"
    DISCOVERY_MODE POST_BUILD
    DISCOVERY_TIMEOUT 30
  )
endfunction()

# Unit tests
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit)
  file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS unit/*.cpp)
  
  foreach(test_source ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_AxolotlDB_test(${test_name} "unit" ${test_source})
  endforeach()
else()
  message(STATUS "No unit tests directory found")
endif()

# Integration tests
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration)
  file(GLOB INTEGRATION_TEST_SOURCES CONFIGURE_DEPENDS integration/*.cpp)
  
  foreach(test_source ${INTEGRATION_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    if(TARGET ${test_name})
      set(test_name "${test_name}_integration")
    endif()
    add_AxolotlDB_test(${test_name} "integration" ${test_source})
  endforeach()
else()
  message(STATUS "No integration tests directory found")
endif()

# If no test files exist yet, create a dummy test to verify the setup
if(NOT UNIT_TEST_SOURCES AND NOT INTEGRATION_TEST_SOURCES)
  message(STATUS "No test files found, creating sample test")
  
  # Create a simple placeholder test
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sample_test.cpp
    "#include <gtest/gtest.h>\n\n"
    "TEST(SampleTest, AlwaysPasses) {\n"
    "  EXPECT_EQ(1, 1);\n"
    "}\n"
  )
  
  add_AxolotlDB_test(sample_test "unit" ${CMAKE_CURRENT_BINARY_DIR}/sample_test.cpp)
  
  message(STATUS "Created sample test - replace with actual tests in tests/unit/ or tests/integration/")
endif()