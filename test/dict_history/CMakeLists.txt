# Dictionary search history ring (design doc ttf/2026-11-09 §F4): shares the
# fake HalStorage used by the clipping and library tests (temp+rename writes,
# injected failures).
add_executable(DictHistoryStoreTest
  DictHistoryStoreTest.cpp
  ${REPO_ROOT}/src/DictHistoryStore.cpp
)
target_link_libraries(DictHistoryStoreTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(DictHistoryStoreTest PRIVATE stubs ../library_builder/stubs ${REPO_ROOT}/src)
gtest_discover_tests(DictHistoryStoreTest)
