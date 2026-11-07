find_package(Python3 REQUIRED COMPONENTS Interpreter)

# JPEGDEC host fetch: same upstream pin the firmware libdeps use, patched with
# the repo's progressive-JPEG fixes (scripts/jpegdec_patches, same files the
# PlatformIO pre-script applies). The source tree is compiled directly —
# JPEGDEC ships no CMake project.
set(JPEGDEC_SOURCE_DIR "" CACHE PATH "Existing pinned JPEGDEC source for host tests")
if(NOT JPEGDEC_SOURCE_DIR)
  FetchContent_Declare(jpegdec
    GIT_REPOSITORY https://github.com/bitbank2/JPEGDEC.git
    GIT_TAG 86282979224c8a32fd51e091ed5a35b0c699a52b)
  # Populate only: JPEGDEC ships an ESP-IDF CMakeLists (idf_component_register),
  # which MakeAvailable cannot consume on host. We compile its sources directly.
  FetchContent_Populate(jpegdec)
  set(JPEGDEC_SOURCE_DIR "${jpegdec_SOURCE_DIR}")
endif()

# Patch a build-owned copy, never the original tree (review round 3): an
# explicitly provided JPEGDEC_SOURCE_DIR may sit on a read-only filesystem,
# and incremental rebuilds must always see the current patch set applied to
# pristine sources. file(COPY) + apply runs on every re-configure; the patch
# list below is registered as a configure dependency so editing a patch
# re-runs configure before the next build picks its sources.
set(JPEGDEC_PATCHED_DIR "${CMAKE_BINARY_DIR}/jpegdec-patched")
file(REMOVE_RECURSE "${JPEGDEC_PATCHED_DIR}")
file(COPY "${JPEGDEC_SOURCE_DIR}/" DESTINATION "${JPEGDEC_PATCHED_DIR}")
set(JPEGDEC_SOURCE_DIR "${JPEGDEC_PATCHED_DIR}")

file(GLOB JPEGDEC_PATCHES "${REPO_ROOT}/scripts/jpegdec_patches/*.patch")
foreach(PATCH ${JPEGDEC_PATCHES})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PATCH}")
  execute_process(
    COMMAND git -C ${JPEGDEC_SOURCE_DIR} apply --check --reverse ${PATCH}
    RESULT_VARIABLE already_applied ERROR_QUIET OUTPUT_QUIET)
  if(NOT already_applied EQUAL 0)
    execute_process(
      COMMAND git -C ${JPEGDEC_SOURCE_DIR} apply --check ${PATCH}
      RESULT_VARIABLE applies_clean ERROR_QUIET OUTPUT_QUIET)
    if(NOT applies_clean EQUAL 0)
      message(FATAL_ERROR "JPEGDEC patch does not apply to the fetched source: ${PATCH}")
    endif()
    execute_process(COMMAND git -C ${JPEGDEC_SOURCE_DIR} apply ${PATCH} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "JPEGDEC patch failed: ${PATCH}")
    endif()
  endif()
endforeach()

# ── ImageDecodeGate protocol tests ──────────────────────────────────────────
# Single-TU white-box: the gate's .cpp is included so the host clock seam
# (IMAGE_DECODE_GATE_HOST) feeds the bounded-wait path.
add_executable(ImageDecodeGateTest ImageDecodeGateTest.cpp)
target_include_directories(ImageDecodeGateTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub/blocks
  ${REPO_ROOT}/lib/hal
)
target_compile_definitions(ImageDecodeGateTest PRIVATE IMAGE_DECODE_GATE_HOST=1)
target_link_libraries(ImageDecodeGateTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ImageDecodeGateTest)

# ── Decode parity + abort tests (real JPEG converter + JPEGDEC) ─────────────
add_executable(ImagePredecodeParityTest
  ImagePredecodeParityTest.cpp
  ${JPEGDEC_SOURCE_DIR}/src/JPEGDEC.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(ImagePredecodeParityTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Utf8
  # After lib/FsHelpers: the converters need the REAL FsHelpers extension
  # predicates; the parser-stub FsHelpers.h (below) is the fallback for the
  # headers that only need the parser-facing subset. This suite's WString
  # stub (above) keeps String's ctors explicit so std::string arguments
  # resolve to the string_view overloads.
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${JPEGDEC_SOURCE_DIR}/src
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
)
target_compile_definitions(ImagePredecodeParityTest PRIVATE __LINUX__)
# The converters read ESP.getFreeHeap() on the firmware path; the stub header
# provides the surface (see section_incremental's harness for the same trick).
target_compile_options(ImagePredecodeParityTest PRIVATE -include Arduino.h)
target_link_libraries(ImagePredecodeParityTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ImagePredecodeParityTest)
