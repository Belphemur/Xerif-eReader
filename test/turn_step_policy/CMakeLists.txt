add_executable(TurnStepPolicyTest TurnStepPolicyTest.cpp)

target_include_directories(TurnStepPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(TurnStepPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TurnStepPolicyTest)
