add_executable(MemoryClaimGateTest
  MemoryClaimGateTest.cpp
)

target_include_directories(MemoryClaimGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(MemoryClaimGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MemoryClaimGateTest)
