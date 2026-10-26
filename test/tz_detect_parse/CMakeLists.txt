add_executable(TzDetectParseTest
  TzDetectParseTest.cpp
)

target_include_directories(TzDetectParseTest PRIVATE
  ${REPO_ROOT}/lib
)

target_link_libraries(TzDetectParseTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(TzDetectParseTest)
