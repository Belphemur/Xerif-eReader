add_executable(TlsHeapGuardTest
  TlsHeapGuardTest.cpp
)

target_include_directories(TlsHeapGuardTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(TlsHeapGuardTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TlsHeapGuardTest)
