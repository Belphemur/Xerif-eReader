# Hint-stack memory benchmark (opt-in: -DCROSSPOINT_BUILD_BENCH=ON). Links the
# REAL BookFontLoader against the FreeType backend and measures how deep the
# firmware's actual render/layout calls go through the loaded faces, so the
# hint-probe budget can be audited without flashing the device. A plain
# executable, not a ctest case: a depth assertion would be flaky across hosts.
add_executable(HintMemBench
  HintMemBench.cpp
  ${REPO_ROOT}/src/BookFontLoader.cpp
  ${REPO_ROOT}/src/adapters/TtfUiFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/TtfFont.cpp
  ${REPO_ROOT}/test/book_font_loader/stubs/Stubs.cpp
  ${FONT_FT_TEST_SOURCES}
)

target_include_directories(HintMemBench PRIVATE
  # stubs/ first so Arduino.h/HalStorage.h resolve to the host-safe versions
  ${REPO_ROOT}/test/book_font_loader/stubs
  ${REPO_ROOT}
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/adapters
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/stb
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/stb
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
  ${FONT_FT_TEST_INCLUDES}
)

target_compile_definitions(HintMemBench PRIVATE
  TESTDATA_DIR="${REPO_ROOT}/test"
  # Memory.h host mode (no heap_caps); matches the real host-test gating.
  HOST_TEST=1
  CROSSPOINT_FONT_BACKEND_FT=1
  # The firmware's autohint/mono modules (platformio.ini build_flags): without
  # AUTOHINT the requested Light mode renders silently unhinted and the whole
  # hinted-vs-unhinted comparison is void.
  FREEINK_FONT_ENABLE_AUTOHINT=1
  FREEINK_FONT_ENABLE_MONOCHROME=1
)
# Opt-in second bench variant with the TrueType bytecode interpreter unlocked
# (FREEINK_FONT_ENABLE_NATIVE_HINTING=1): what a Default/Native hinting
# increase would actually cost. The vendored FreeType already compiles the
# interpreter (TT_CONFIG_OPTION_BYTECODE_INTERPRETER); only the FreeInk gate
# refuses the mode. cmake -DCROSSPOINT_BENCH_NATIVE_HINTING=ON builds
# HintMemBenchNative alongside the stock bench.
option(CROSSPOINT_BENCH_NATIVE_HINTING "Also build the Native-hinting bench variant" OFF)
if(CROSSPOINT_BENCH_NATIVE_HINTING)
  add_executable(HintMemBenchNative
    HintMemBench.cpp
    ${REPO_ROOT}/src/BookFontLoader.cpp
    ${REPO_ROOT}/src/adapters/TtfUiFont.cpp
    ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
    ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
    ${REPO_ROOT}/lib/Utf8/Utf8.cpp
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/TtfFont.cpp
    ${REPO_ROOT}/test/book_font_loader/stubs/Stubs.cpp
    ${FONT_FT_TEST_SOURCES}
  )
  target_include_directories(HintMemBenchNative PRIVATE
    ${REPO_ROOT}/test/book_font_loader/stubs
    ${REPO_ROOT}
    ${REPO_ROOT}/src
    ${REPO_ROOT}/src/adapters
    ${REPO_ROOT}/lib
    ${REPO_ROOT}/lib/EpdFont
    ${REPO_ROOT}/lib/Memory
    ${REPO_ROOT}/lib/hal
    ${REPO_ROOT}/lib/Utf8
    ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/stb
    ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/stb
    ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
    ${FONT_FT_TEST_INCLUDES}
  )
  target_compile_definitions(HintMemBenchNative PRIVATE
    TESTDATA_DIR="${REPO_ROOT}/test"
    HOST_TEST=1
    CROSSPOINT_FONT_BACKEND_FT=1
    FREEINK_FONT_ENABLE_AUTOHINT=1
    FREEINK_FONT_ENABLE_MONOCHROME=1
    FREEINK_FONT_ENABLE_NATIVE_HINTING=1
  )
  target_link_libraries(HintMemBenchNative PRIVATE crosspoint_test_common)
endif()

target_link_libraries(HintMemBench PRIVATE
  crosspoint_test_common
)
