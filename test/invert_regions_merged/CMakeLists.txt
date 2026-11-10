# Header-only XOR coalescing for selection/clip inversion (PR #306
# CodeRabbit finding): rects are partitioned into vertical bands, each band
# x-merged separately, and no emitted segment is ever overlapped again —
# overlaps would restore (cancel) the highlight pixels via double XOR.
add_executable(InvertRegionsMergedTest
  InvertRegionsMergedTest.cpp
)
target_include_directories(InvertRegionsMergedTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)
target_link_libraries(InvertRegionsMergedTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(InvertRegionsMergedTest)
