add_executable(InputGrammarTest PowerClickWindowTest.cpp WakeReleaseSwallowTest.cpp)

target_include_directories(InputGrammarTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(InputGrammarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InputGrammarTest)
