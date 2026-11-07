add_executable(TtfImageEngineTest
  TtfImageEngineTest.cpp
  ${REPO_ROOT}/src/activities/reader/TtfImageCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageDecodeGate.cpp
  stubs/Stubs.cpp
)

target_include_directories(TtfImageEngineTest PRIVATE
  # stubs/ first so HalStorage.h/HalFile.h/Logging.h/Arduino.h resolve to the
  # host-safe versions
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
)

target_compile_definitions(TtfImageEngineTest PRIVATE
  HOST_TEST=1
  CROSSPOINT_TTF_READER=1
)

target_link_libraries(TtfImageEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TtfImageEngineTest)
