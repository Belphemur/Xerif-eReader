add_executable(QuickPageCaptureTest
  QuickPageCaptureTest.cpp
  ${REPO_ROOT}/src/activities/reader/QuickPageCapture.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/layout/ChapterLayout.cpp
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/TtfFont.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/render/PageRenderer.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/render/ImageRenderer.cpp
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
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/vendor/jpegdec_impl.cpp
)

target_include_directories(QuickPageCaptureTest PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/stb
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/expat
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/libunibreak
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/miniz/include
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/pngle
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/stb
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/jpegdec/src
)

target_compile_definitions(QuickPageCaptureTest PRIVATE
  TESTDATA_DIR="${REPO_ROOT}/test"
)

target_link_libraries(QuickPageCaptureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(QuickPageCaptureTest)
