# Both font-backend variants (stb + FreeType) — see add_font_backend_tests().
add_font_backend_tests(TtfWordSelectTest
  SOURCES
    TtfWordSelectTest.cpp
    ${REPO_ROOT}/src/activities/reader/TtfWordSelect.cpp
    ${REPO_ROOT}/src/util/DictionarySelection.cpp
    ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
    ${REPO_ROOT}/lib/MiniBidi/minibidi.c
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/TtfFont.cpp
    ${REPO_ROOT}/lib/Utf8/Utf8.cpp
    ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${REPO_ROOT}/src
    ${REPO_ROOT}/src/activities/reader
    ${REPO_ROOT}/src/util
    ${REPO_ROOT}/lib
    ${REPO_ROOT}/lib/Memory
    ${REPO_ROOT}/lib/MiniBidi
    ${REPO_ROOT}/lib/hal
    ${REPO_ROOT}/lib/Utf8
    ${REPO_ROOT}/lib/Epub
    ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/stb
    ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/stb
  DEFINITIONS
    HOST_TEST=1
    CROSSPOINT_TTF_READER=1
    EMBER_FIXTURES_DIR="${REPO_ROOT}/test/fixtures/fonts/amazon-ember"
)

# Vendored upstream C source emits -Wsign-compare under the test build's
# -Wall/-Wextra; leave the vendored code untouched and silence it here
# (mirrors test/minibidi_arabic).
set_source_files_properties(${REPO_ROOT}/lib/MiniBidi/minibidi.c PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare")
