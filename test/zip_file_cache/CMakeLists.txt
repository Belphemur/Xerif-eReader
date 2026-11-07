add_executable(ZipFileCacheTest
  ZipFileCacheTest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../section_incremental/ParserLinkStubs.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../section_incremental/ContentProtectionHostStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContainerParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNcxParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CjkBreaks.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Txt/TxtToHtml.cpp
  ${REPO_ROOT}/lib/Txt/Txt.cpp
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/hal/HalMemory.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/inflate/src/InflateStream.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/miniz/src/miniz.c
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/miniz/src/miniz_cores.c
)

# Epub::ZipFileCache and its pool-allocated entry map only exist under
# BOARD_HAS_PSRAM — define it here so the host build exercises the same
# container types the firmware uses (poolMalloc = malloc on host).
target_compile_definitions(ZipFileCacheTest PRIVATE
  BOARD_HAS_PSRAM
  TEST_EPUB_SOURCE="${REPO_ROOT}/test/epubs/test_br_section_break.epub"
)

target_include_directories(ZipFileCacheTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../section_incremental/stubs
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/Epub/Epub/converters
  ${REPO_ROOT}/lib/AsciiText
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/inflate/src
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/miniz/include
  ${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/include
  ${REPO_ROOT}/lib/GifSupport
)

target_link_libraries(ZipFileCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(ZipFileCacheTest)
