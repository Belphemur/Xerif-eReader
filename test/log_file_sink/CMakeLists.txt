add_executable(LogFileSinkTest
  LogFileSinkTest.cpp
  ${REPO_ROOT}/lib/Logging/LogFileSink.cpp
)

target_include_directories(LogFileSinkTest PRIVATE
  ${REPO_ROOT}/lib/Logging
)

# The sink's state machine is pure C++ (the HalStorage device wiring is guarded
# by ESP_PLATFORM and compiles out on host).
target_link_libraries(LogFileSinkTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LogFileSinkTest)
