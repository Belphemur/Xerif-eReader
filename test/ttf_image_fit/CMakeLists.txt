# TTF image full-fit + tone-bake suite (design
# docs/design/2026-11-08-ttf-image-contrast-and-fit.md). Compiles the real
# TtfImageEngine + the SDK render stack over a panel-shaped fake GfxRenderer,
# so the placement-coverage invariant and the byte-exact tone bake are pinned
# on host without a device.
add_executable(TtfImageFitTest
  TtfImageFitTest.cpp
  TtfImageTextureTest.cpp
  DpidScalerTest.cpp
  stubs/Stubs.cpp
  stubs/ImageDecoderFactory.cpp
  ${REPO_ROOT}/src/activities/reader/TtfImageCache.cpp
  ${REPO_ROOT}/src/activities/reader/TtfImageEngine.cpp
  ${REPO_ROOT}/src/activities/reader/DpidScaler.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageDecodeGate.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/GifToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/render/ImageRenderer.cpp
  ${REPO_ROOT}/freeink-sdk/libs/assets/ProgressiveJpeg/src/ProgressiveJpeg.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/FreeInkBook.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/BookCatalog.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/epub/ZipCatalog.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/epub/XmlSax.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/epub/PackageParsers.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/epub/ImageProbe.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/text/EntityFilter.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/text/Hyphenator.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/css/Css.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/vendor/expat_xmlparse.c
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/vendor/expat_xmlrole.c
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/vendor/expat_xmltok.c
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/vendor/miniz_impl.c
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/vendor/miniz_cores_impl.c
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/vendor/unibreak_impl.c
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/pngle/pngle.c
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/vendor/tjpgd_impl.c
)

target_include_directories(TtfImageFitTest PRIVATE
  # stubs/ first: GfxRenderer/HalMemory/HalStorage resolve to the host fakes
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/GifSupport
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/freeink-sdk/libs/assets/ProgressiveJpeg/include
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/expat
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/libunibreak
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/miniz/include
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/pngle
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/stb
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/tjpgd
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/stb
)

target_compile_definitions(TtfImageFitTest PRIVATE
  HOST_TEST=1
  CROSSPOINT_TTF_READER=1
  TESTDATA_DIR="${REPO_ROOT}/test"
  FIT_FIXTURES="${CMAKE_CURRENT_BINARY_DIR}/fixtures"
)

# The progressive-JPEG fixture (DC-only first scan, stdlib encoder) — no
# committed fixture carries that format and no image tooling is assumed.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fixtures/fit_fixtures.epub"
         "${CMAKE_CURRENT_BINARY_DIR}/fixtures/dark.gif"
         "${CMAKE_CURRENT_BINARY_DIR}/fixtures/small.bmp"
         "${CMAKE_CURRENT_BINARY_DIR}/fixtures/prog.jpg"
         "${CMAKE_CURRENT_BINARY_DIR}/fixtures/pattern.png"
         "${CMAKE_CURRENT_BINARY_DIR}/fixtures/dark.png"
         "${CMAKE_CURRENT_BINARY_DIR}/fixtures/dark_prog.jpg"
  COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH= "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_fixtures.py"
          "${CMAKE_CURRENT_BINARY_DIR}/fixtures"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_fixtures.py"
          "${REPO_ROOT}/test/epubs/test_jpeg_images.epub"
  COMMENT "ttf_image_fit: generating progressive-JPEG/GIF/BMP fixtures + EPUB"
  VERBATIM
)
add_custom_target(ttf_image_fit_fixtures DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/fixtures/fit_fixtures.epub")
add_dependencies(TtfImageFitTest ttf_image_fit_fixtures)

# The AnimatedGIF host library is declared (and source-patched) by
# test/gif_support — declared above this add_subdirectory.
target_link_libraries(TtfImageFitTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  animatedgif_host
  GTest::gtest_main
)

gtest_discover_tests(TtfImageFitTest)
