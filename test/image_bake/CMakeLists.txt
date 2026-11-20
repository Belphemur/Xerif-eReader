# Blue-noise cover bake suite (design docs/design/2026-11-20-cover-image-pipeline.md):
# pins ImageBakeQuantizer against the reader's ToneBake4 bake (per-pixel path
# and the tiled toneRow fast path) and against the BmpStreamWriter packing the
# BMP cover converters emit, plus the 1-bit quantizer regression contract.
add_executable(ImageBakeTest
  ImageBakeQuantizerTest.cpp
  ${REPO_ROOT}/lib/ImageBake/BlueNoise64.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BmpStreamWriter.cpp
)

target_include_directories(ImageBakeTest PRIVATE
  # Host-safe stubs first: Logging/HalStorage/HalFile/Arduino resolve to the
  # ttf_image_engine versions (TtfImageCache.h), Print/freertos to the
  # bmp_stream_writer versions (BmpStreamWriter.cpp).
  ${REPO_ROOT}/test/ttf_image_engine/stubs
  ${REPO_ROOT}/test/bmp_stream_writer/stubs
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/ImageBake
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
)

target_compile_definitions(ImageBakeTest PRIVATE
  HOST_TEST=1
  CROSSPOINT_TTF_READER=1
)

target_link_libraries(ImageBakeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ImageBakeTest)
