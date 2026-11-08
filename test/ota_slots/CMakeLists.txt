add_executable(OtaSlotsTest
  OtaSlotsTest.cpp
  ${REPO_ROOT}/src/network/OtaSlots.cpp
  stubs/Stubs.cpp
)

target_include_directories(OtaSlotsTest PRIVATE
  # stubs/ first so the ESP-IDF partition/OTA headers resolve to the
  # host-safe versions.
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)

target_link_libraries(OtaSlotsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaSlotsTest)

# §15/§16 install-time slot auto-selection: the pure routing decision points
# (classifier + §16 matrix). No device stubs needed — the routing header is
# dependency-free by design so the host suite can pin every matrix row.
add_executable(OtaInstallRoutingTest
  OtaInstallRoutingTest.cpp
  ${REPO_ROOT}/src/network/OtaInstallRouting.cpp
)

target_include_directories(OtaInstallRoutingTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(OtaInstallRoutingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaInstallRoutingTest)
