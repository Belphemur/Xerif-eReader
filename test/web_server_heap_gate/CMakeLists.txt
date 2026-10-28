add_executable(WebServerHeapGateTest
  WebServerHeapGateTest.cpp
)

target_include_directories(WebServerHeapGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(WebServerHeapGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WebServerHeapGateTest)
