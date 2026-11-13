# FIBP layout benchmark (opt-in: -DCROSSPOINT_BUILD_BENCH=ON). Builds the real
# FreeInkBook layout engine against the FreeType backend (FtFont) so the
# chapter-index build path can be timed without flashing the device. The
# target is a plain executable, not a ctest case, so the default gate stays
# fast and non-flaky.
add_executable(FibpLayoutBench
  FibpLayoutBench.cpp
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
  ${FONT_FT_TEST_SOURCES}
)

target_include_directories(FibpLayoutBench PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/expat
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/libunibreak
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/miniz/include
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/pngle
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/stb
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/jpegdec/src
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/stb
  ${FONT_FT_TEST_INCLUDES}
)

target_compile_definitions(FibpLayoutBench PRIVATE
  TESTDATA_DIR="${REPO_ROOT}/test"
  CROSSPOINT_FONT_BACKEND_FT=1
)

target_link_libraries(FibpLayoutBench PRIVATE
  crosspoint_test_common
)
