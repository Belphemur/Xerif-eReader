# Vocabulary Notebook store (addendum 2026-11-09): the tail reader and the
# rewrite-without-the-record delete paths over /Vocabulary.txt, sharing the
# fake HalStorage used by the clipping and library tests (temp+replaceFile
# publication, injected failures).
add_executable(VocabNotebookTest
  VocabNotebookTest.cpp
  ${REPO_ROOT}/src/vocabulary/VocabularyStore.cpp
  ${REPO_ROOT}/src/util/HtmlToPlainText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_link_libraries(VocabNotebookTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(VocabNotebookTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/../library_builder/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Logging
)
gtest_discover_tests(VocabNotebookTest)
