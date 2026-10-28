add_executable(QueuedEdgeDrainTest QueuedEdgeDrainTest.cpp)

target_include_directories(QueuedEdgeDrainTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(QueuedEdgeDrainTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(QueuedEdgeDrainTest)
