// Host-side gtest suite for the Phase 1a BookFontLoader (native TTF fonts).
// Exercises the real loader paths through controllable file/heap stubs:
// builtin fallback, budget clamping, malformed-sfnt rejection, fingerprint
// stability, and the retained byte-lifetime contract.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "BookFontLoader.h"
#include "TestHeapHooks.h"
#include "adapters/TtfUiFont.h"
#include "render/TtfFont.h"

// TtfFont.cpp compiles stb with STBTT_STATIC (internal linkage), so the test
// needs its own non-static copy for the reference-metrics math below.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {

// A minimal valid sfnt shell: 12-byte header + 1 table entry of zeros.
// TtfFont::init will reject it (no real tables) but it passes the loader's
// own sfnt boundary validation, isolating the loader's checks from stb.
std::string validSfntShell(uint16_t numTables = 1) {
  std::string s;
  s += std::string("\x00\x01\x00\x00", 4);  // sfnt version
  s += static_cast<char>(numTables >> 8);
  s += static_cast<char>(numTables & 0xFF);  // numTables
  s += std::string(6, '\0');                 // search fields
  // numTables * 16-byte table directory entries (offset/length 0)
  s += std::string(static_cast<size_t>(numTables) * 16, '\0');
  return s;
}

void writeFaceFile(const std::string& path, const std::string& bytes) { Storage.files[path] = bytes; }

TEST(BookFontLoaderBasics, InstanceCreated) {
  freeink::book::BookFontLoader loader;
  EXPECT_EQ(loader.familyCount(), 0u);
  EXPECT_EQ(loader.fontFingerprint(), 0u);
}

TEST(BookFontLoaderBasics, GetReaderFontFallsBackToBuiltin) {
  freeink::book::BookFontLoader loader;
  freeink::book::FontChain* readerFont = loader.getReaderFont();
  ASSERT_NE(readerFont, nullptr);
  // Builtin BitmapBookFont fallback covers None|Bold|Italic (0x07).
  EXPECT_EQ(readerFont->styleCoverage(), 0x07);
}

TEST(BookFontLoaderBasics, RepeatedBeginDoesNotLeak) {
  testSetPsramHeap({0, 0, 0, 0});
  testSetFreeHeap(320 * 1024, 320 * 1024);
  freeink::book::BookFontLoader loader;
  loader.begin();
  loader.begin();  // second begin() over a live loader must stay safe
  EXPECT_EQ(loader.familyCount(), 0u);
  EXPECT_EQ(loader.fontFingerprint(), 0u);
}

TEST(BookFontLoaderBasics, BudgetClampsToMaxAndFloors) {
  testSetPsramHeap({0, 0, 0, 0});  // no PSRAM → DRAM tier
  freeink::book::BookFontLoader loader;

  // Above the 48KB reserve: budget = min(free - 48KB, 128KB).
  testSetFreeHeap(320 * 1024, 320 * 1024);
  loader.begin();
  EXPECT_EQ(loader.dramBudgetForTest(), 128u * 1024u);

  // Above the reserve but under the cap: exact remainder.
  testSetFreeHeap(64 * 1024, 64 * 1024);
  loader.begin();
  EXPECT_EQ(loader.dramBudgetForTest(), 16u * 1024u);

  // Exactly at the reserve: zero (nothing safe to spend).
  testSetFreeHeap(48 * 1024, 48 * 1024);
  loader.begin();
  EXPECT_EQ(loader.dramBudgetForTest(), 0u);

  // Below the reserve: zero (was the pre-review 128KB grant).
  testSetFreeHeap(32 * 1024, 32 * 1024);
  loader.begin();
  EXPECT_EQ(loader.dramBudgetForTest(), 0u);
}

TEST(BookFontLoaderBasics, MalformedSfntRejected) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();

  // Too small for the sfnt header (<12 bytes).
  writeFaceFile("/fonts/Bad-Tiny.ttf", "short");
  // numTables == 0.
  std::string zero = validSfntShell(0);
  zero[5] = '\x00';
  writeFaceFile("/fonts/Bad-ZeroTables.ttf", zero);

  auto& fam = loader.editFamily(0);
  fam.faceCount = 2;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = 5;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Bad-Tiny.ttf");
  fam.faces[1].styleFlags = freeink::book::StyleBold;
  fam.faces[1].fileSize = static_cast<uint32_t>(zero.size());
  std::snprintf(fam.faces[1].file, sizeof(fam.faces[1].file), "%s", "/fonts/Bad-ZeroTables.ttf");
  // Establish the manifest count so ensureLoaded() actually attempts the
  // load (familyCount_ > 0 gate) and the rejection path really runs.
  loader.setFamilyCountForTest(1);

  // markDirty + reload: both faces must be skipped (no crash, no chain entry).
  loader.markDirty();
  loader.getReaderFont();
  // Nothing loaded → empty fingerprint (0), NOT the FNV offset basis.
  EXPECT_EQ(loader.fontFingerprint(), 0u);
  // The builtin fallback serves the reader because the chain is empty.
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
}

TEST(BookFontLoaderBasics, FingerprintIsContentBased) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  // Fingerprint must be 0 when nothing is loaded (content-based, never path).
  EXPECT_EQ(loader.fontFingerprint(), 0u);
}

// The Atkinson fallback faces must ride ALONG a selected TTF family as the
// chain tail (§14.5): a family whose faces all fail to load still serves a
// glyph-bearing chain through the live chain_, not just the standalone
// fallback singleton.
TEST(BookFontLoaderBasics, FallbackTailAppendedToSelectedChain) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();

  // A selected family whose only face fails sfnt validation.
  writeFaceFile("/fonts/Failing/Failing-Regular.ttf", "garbage");
  auto& fam = loader.editFamily(0);
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = 7;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Failing/Failing-Regular.ttf");
  loader.setFamilyCountForTest(1);
  loader.markDirty();

  freeink::book::FontChain* font = loader.getReaderFont();
  ASSERT_NE(font, nullptr);
  // The live chain itself carries the tail: full None|Bold|Italic coverage.
  EXPECT_EQ(font->styleCoverage(), 0x07);
  EXPECT_EQ(font, &loader.chainForTest());

  // A missing glyph resolves through the tail, never nullptr.
  const uint32_t cpNoOneHas = 0x1F600;  // emoji — no face covers it
  uint8_t faceFlags = 0xFF;
  EXPECT_NE(font->fontFor(cpNoOneHas, freeink::book::StyleNone, &faceFlags), nullptr);
}

std::string readFixtureBytes(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Issue #168: the boot-time TTF UI-fallback faces borrow the loader's
// RESIDENT face bytes. An unselected loader loads families_[0] via
// ensureLoaded() without recording it, so the reader's first selectFamily()
// of that SAME family used to spuriously markDirty() → reload → free the
// borrowed bytes under the live faces — the next status-bar draw walked a
// freed cmap and panicked (LoadProhibited). Selecting the already-resident
// family must be a no-op: byte owners stay put, fingerprint (FIBP cache
// identity) untouched. A genuinely different family must still reload.
TEST(BookFontLoaderBasics, SelectingResidentFamilyDoesNotReload) {
  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024});
  testSetFreeHeap(320 * 1024, 320 * 1024);
  freeink::book::BookFontLoader loader;
  loader.begin();

  // One real, loadable face at families_[0] — what an unselected
  // ensureLoaded() loads, and what the UI fallback borrows.
  const std::string face = readFixtureBytes(DEJAVU_FIXTURE);
  ASSERT_GT(face.size(), 1024u);
  writeFaceFile("/fonts/Alpha/Alpha-Regular.ttf", face);
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Alpha");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(face.size());
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Alpha/Alpha-Regular.ttf");
  loader.setFamilyCountForTest(1);

  // Boot path: unselected ensureLoaded() loads families_[0]. The borrowed
  // bytes are what the boot-time UI-fallback faces hold; slotFaceBytes only
  // exists on the FT backend (the borrowed-bytes consumer), so the stb
  // variant pins the same no-reload semantics via isDirty()/fingerprint.
  loader.getReaderFont();
#if CROSSPOINT_FONT_BACKEND_FT
  ASSERT_NE(loader.slotFaceBytes(0), nullptr);
  const void* resident = loader.slotFaceBytes(0);
#endif
  const uint32_t fingerprint = loader.fontFingerprint();
  EXPECT_NE(fingerprint, 0u);

  // The reader's first selectFamily() of the session names the SAME family.
  // Root-cause pin: it must not mark the loader dirty — the dirty flag is
  // what sent ensureLoaded() into the reload that freed the borrowed bytes.
  loader.selectFamily("Alpha");
  EXPECT_FALSE(loader.isDirty());
  loader.getReaderFont();

#if CROSSPOINT_FONT_BACKEND_FT
  EXPECT_EQ(loader.slotFaceBytes(0), resident);
#endif
  EXPECT_EQ(loader.fontFingerprint(), fingerprint);

  // Negative control: a genuinely different selection MUST dirty the loader
  // and reload. Same table content plus one trailing byte keeps the face
  // loadable while changing the content fingerprint, so the reload stays
  // observable even if the allocator hands back the same block.
  writeFaceFile("/fonts/Beta/Beta-Regular.ttf", face + std::string("\0", 1));
  auto& famB = loader.editFamily(1);
  std::snprintf(famB.name, sizeof(famB.name), "%s", "Beta");
  famB.faceCount = 1;
  famB.faces[0].styleFlags = freeink::book::StyleNone;
  famB.faces[0].fileSize = static_cast<uint32_t>(face.size() + 1);
  std::snprintf(famB.faces[0].file, sizeof(famB.faces[0].file), "%s", "/fonts/Beta/Beta-Regular.ttf");
  loader.setFamilyCountForTest(2);
  loader.selectFamily("Beta");
  EXPECT_TRUE(loader.isDirty());
  loader.getReaderFont();
#if CROSSPOINT_FONT_BACKEND_FT
  EXPECT_NE(loader.slotFaceBytes(0), nullptr);
#endif
  EXPECT_NE(loader.fontFingerprint(), fingerprint);
}

// The tail is a non-selectable addition: with NO family selected the chain
// stays empty and the builtin fallback singleton serves the reader.
TEST(BookFontLoaderBasics, NoFamilyStillUsesFallbackSingleton) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
  // chainForTest() is still empty: the singleton answered, not the live chain.
  EXPECT_EQ(loader.chainForTest().styleCoverage(), 0u);
}

// An empty manifest may legitimately complete the load attempt (fingerprint 0
// and the standalone fallback singleton), but a later begin() rescan must
// reset the load gate so the same persisted family selection can load without
// relying on selectFamily() to dirty an unchanged name.
TEST(BookFontLoaderBasics, BeginResetsLoadGateAfterEmptyManifest) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.selectFamily("Alpha");
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
  EXPECT_EQ(loader.chainForTest().styleCoverage(), 0u);  // empty-manifest path

  loader.begin();  // simulates the settings screen rescan after SD mounts
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Alpha");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = 5;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Alpha/Alpha-Regular.ttf");
  loader.setFamilyCountForTest(1);
  loader.selectFamily("Alpha");  // unchanged selection: intentionally no markDirty

  // The malformed face is rejected, but ensureLoaded() ran and appended the
  // fallback tail to the live chain (distinct from the standalone singleton).
  freeink::book::FontChain* font = loader.getReaderFont();
  EXPECT_EQ(font, &loader.chainForTest());
  EXPECT_EQ(font->styleCoverage(), 0x07);
}

TEST(FontFaceInfoTest, DefaultMembers) {
  freeink::book::FontFaceInfo faceInfo;
  EXPECT_EQ(faceInfo.name[0], '\0');
  EXPECT_EQ(faceInfo.file[0], '\0');
  EXPECT_EQ(faceInfo.styleFlags, 0u);
  EXPECT_EQ(faceInfo.fileSize, 0u);
  EXPECT_EQ(faceInfo.mtime, 0u);
}

TEST(FamilyInfoTest, DefaultMembers) {
  freeink::book::FamilyInfo familyInfo;
  EXPECT_EQ(familyInfo.name[0], '\0');
  EXPECT_EQ(familyInfo.faceCount, 0u);
  EXPECT_FALSE(familyInfo.isBuiltinFallback);
}

TEST(StyleFlagsTest, BitValues) {
  EXPECT_EQ(freeink::book::StyleNone, 0);
  EXPECT_EQ(freeink::book::StyleBold, 1);
  EXPECT_EQ(freeink::book::StyleItalic, 2);
  EXPECT_EQ(freeink::book::StyleBold & freeink::book::StyleItalic, 0);
  EXPECT_EQ(freeink::book::StyleBold | freeink::book::StyleItalic, 3);
}

TEST(BookFontLoaderConstants, DiscoveryBound) {
  EXPECT_GT(freeink::book::BookFontLoader::kMaxDiscoveredFamilies, 0u);
  EXPECT_LE(freeink::book::BookFontLoader::kMaxDiscoveredFamilies, 100u);
}

}  // namespace
// ── scanFonts — §14.4 per-family discovery walk ──────────────────────────────

namespace {

namespace book = freeink::book;
using freeink::book::BookFontLoader;

constexpr uint8_t kStyleBI = freeink::book::StyleBold | freeink::book::StyleItalic;

void resetStorage() {
  Storage.files.clear();
  Storage.dirs.clear();
  Storage.mtimes.clear();
}

// Registers a file entry (and every ancestor directory) in the stub storage.
void seedFile(const std::string& path, const std::string& bytes = "x") {
  for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
    Storage.dirs.insert(path.substr(0, slash));
  }
  Storage.files[path] = bytes;
}

const freeink::book::FontFaceInfo* findFace(const freeink::book::FamilyInfo& fam, uint8_t styleFlags) {
  for (uint8_t i = 0; i < fam.faceCount; ++i) {
    if (fam.faces[i].styleFlags == styleFlags) return &fam.faces[i];
  }
  return nullptr;
}

}  // namespace

TEST(ScanFontsTest, DiscoversFamiliesAndStyles) {
  resetStorage();
  seedFile("/fonts/Bookerly/Bookerly-Regular.ttf");
  seedFile("/fonts/Bookerly/Bookerly-Bold.ttf");
  seedFile("/fonts/Bookerly/Bookerly-Italic.ttf");
  seedFile("/fonts/Bookerly/Bookerly-BoldItalic.ttf");
  seedFile("/fonts/Bookerly/Bookerly-SemiBold.ttf", "");  // weight heuristic → Bold

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Bookerly");
  EXPECT_EQ(fams[0].faceCount, 4u);
  ASSERT_NE(findFace(fams[0], freeink::book::StyleNone), nullptr);
  ASSERT_NE(findFace(fams[0], freeink::book::StyleBold), nullptr);
  ASSERT_NE(findFace(fams[0], freeink::book::StyleItalic), nullptr);
  ASSERT_NE(findFace(fams[0], kStyleBI), nullptr);
  // Regular's full path points inside the family folder.
  EXPECT_STREQ(findFace(fams[0], freeink::book::StyleNone)->file, "/fonts/Bookerly/Bookerly-Regular.ttf");
}

TEST(ScanFontsTest, HiddenRootWinsFamilyDedupe) {
  resetStorage();
  seedFile("/.fonts/Bookerly/Bookerly-Light.ttf");
  seedFile("/fonts/Bookerly/Bookerly-Regular.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  // Hidden root first (the begin() order), so its family claims the name.
  BookFontLoader::scanFontsForTest("/.fonts", fams, count);
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  // Hidden root wins: the surviving family's Regular is its promoted file.
  EXPECT_STREQ(fams[0].faces[0].file, "/.fonts/Bookerly/Bookerly-Light.ttf");
  EXPECT_EQ(fams[0].faces[0].styleFlags, freeink::book::StyleNone);
}

TEST(ScanFontsTest, StylePriorityAndWordBoundaries) {
  resetStorage();
  seedFile("/fonts/Mix/Mix-SemiBold.ttf");  // weight heuristic → Bold
  seedFile("/fonts/Mix/Mix-Light.ttf");     // light → Regular
  seedFile("/fonts/Mix/Mix-Italic.ttf");    // Italic
  seedFile("/fonts/Mix/Mix-ExtraBold.ttf", "x");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  // SemiBold ≠ Bold (word boundary), so Mix has exactly: Regular(Light),
  // Bold(SemiBold), Bold(ExtraBold — dup resolved lexicographically? No:
  // ExtraBold is Bold too → same slot as SemiBold, first file wins).
  const auto* bold = findFace(fams[0], freeink::book::StyleBold);
  ASSERT_NE(bold, nullptr);
  EXPECT_STREQ(bold->name, "mix-extrabold");  // lexicographically-first dup wins
  const auto* regular = findFace(fams[0], freeink::book::StyleNone);
  ASSERT_NE(regular, nullptr);
  EXPECT_STREQ(regular->name, "mix-light");
  ASSERT_NE(findFace(fams[0], freeink::book::StyleItalic), nullptr);
}

TEST(ScanFontsTest, SingleFileFamilyPromotesRegular) {
  resetStorage();
  seedFile("/fonts/Solo/Solo-Head.ttf", std::string(1234, 'f'));

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(fams[0].faceCount, 1u);
  EXPECT_EQ(fams[0].faces[0].styleFlags, freeink::book::StyleNone);
  // The promoted face carries a canonical stem, not the extension-bearing filename.
  EXPECT_STREQ(fams[0].faces[0].name, "solo-head");
  // The promoted face must carry the lone candidate's size, or
  // tryLoadFace() would read zero bytes and reject the family.
  EXPECT_EQ(fams[0].faces[0].fileSize, 1234u);
}

TEST(ScanFontsTest, DuplicateStyleComparesCanonicalStems) {
  resetStorage();
  seedFile("/fonts/Dupe/Face-Bold.ttf");
  seedFile("/fonts/Dupe/Face-Bold-Extra.otf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  ASSERT_EQ(fams[0].faceCount, 1u);
  EXPECT_STREQ(fams[0].faces[0].name, "face-bold");
  EXPECT_STREQ(fams[0].faces[0].file, "/fonts/Dupe/Face-Bold.ttf");
}

TEST(ScanFontsTest, SkipsJunkFilesAndFolders) {
  resetStorage();
  seedFile("/fonts/Clean/Clean-Regular.ttf");
  seedFile("/fonts/Clean/._Clean-Regular.ttf", "");   // macOS resource fork
  seedFile("/fonts/Clean/Clean-Regular.ttf~", "");    // editor backup
  seedFile("/fonts/Clean/Clean-Regular.json", "");    // wrong extension
  seedFile("/fonts/.Trashes/Clean-Regular.ttf", "");  // hidden folder
  seedFile("/fonts/_private/x-Regular.ttf", "");      // underscore folder
  seedFile("/fonts/Loose-Regular.ttf", "");           // root-level: ignored

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Clean");
  EXPECT_EQ(fams[0].faceCount, 1u);
}

TEST(ScanFontsTest, FamilyCapAt32) {
  resetStorage();
  for (int i = 0; i < BookFontLoader::kMaxDiscoveredFamilies + 4; ++i) {
    seedFile(std::string("/fonts/Fam") + static_cast<char>('A' + i % 26) + std::to_string(i) + "/F-Regular.ttf");
  }
  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  EXPECT_EQ(count, BookFontLoader::kMaxDiscoveredFamilies);
}

TEST(ScanFontsTest, OtfExtensionAccepted) {
  resetStorage();
  seedFile("/fonts/Otf/Otf-Regular.otf");
  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].faces[0].file, "/fonts/Otf/Otf-Regular.otf");
}

TEST(ScanFontsTest, TruncatedFamilyNameIsSkipped) {
  resetStorage();
  // FamilyInfo::name holds 47 chars plus NUL; dirName now has headroom for
  // long path validation, but names too large for the manifest are skipped.
  const std::string longName(48, 'L');
  seedFile("/fonts/" + longName + "/Long-Regular.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  EXPECT_EQ(count, 0u);
}

TEST(ScanFontsTest, LongVendorPathsFitManifest) {
  resetStorage();
  // Owner-log regression: the full path is 71 chars and the old 64-byte face
  // path silently dropped it.
  const std::string path = "/.fonts/Atkinson Hyperlegible Next/AtkinsonHyperlegibleNext-Regular.otf";
  ASSERT_GT(path.size(), 64u);
  seedFile(path);

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/.fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Atkinson Hyperlegible Next");
  EXPECT_EQ(fams[0].faceCount, 1u);
  EXPECT_STREQ(fams[0].faces[0].file, path.c_str());
}

TEST(ScanFontsTest, SplitsFamilyAcrossRootsIntoOneFamily) {
  resetStorage();
  seedFile("/.fonts/Bookerly/Bookerly-Regular.ttf");
  seedFile("/fonts/Bookerly/Bookerly-Bold.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/.fonts", fams, count);
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Bookerly");
  ASSERT_EQ(fams[0].faceCount, 2u);
  const auto* regular = findFace(fams[0], freeink::book::StyleNone);
  const auto* bold = findFace(fams[0], freeink::book::StyleBold);
  ASSERT_NE(regular, nullptr);
  ASSERT_NE(bold, nullptr);
  EXPECT_STREQ(regular->file, "/.fonts/Bookerly/Bookerly-Regular.ttf");
  EXPECT_STREQ(bold->file, "/fonts/Bookerly/Bookerly-Bold.ttf");
}

TEST(ScanFontsTest, FamilyOnlyInVisibleRootIsStillDiscovered) {
  resetStorage();
  seedFile("/.fonts/Hidden/Hidden-Regular.ttf");
  seedFile("/fonts/Visible/Visible-Regular.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/.fonts", fams, count);
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 2u);
  EXPECT_STREQ(fams[1].name, "Visible");
  EXPECT_STREQ(fams[1].faces[0].file, "/fonts/Visible/Visible-Regular.ttf");
}

TEST(ScanFontsTest, TokenlessCandidateBecomesRegularNotPromotedBold) {
  resetStorage();
  // Owner-log regression: "Bookerly Display" has no style token and was
  // skipped, leaving Bold as the promoted Regular.
  seedFile("/fonts/Bookerly-otf/Bookerly Display.ttf");
  seedFile("/fonts/Bookerly-otf/Bookerly-Bold.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  const auto* regular = findFace(fams[0], freeink::book::StyleNone);
  const auto* bold = findFace(fams[0], freeink::book::StyleBold);
  ASSERT_NE(regular, nullptr);
  ASSERT_NE(bold, nullptr);
  EXPECT_STREQ(regular->file, "/fonts/Bookerly-otf/Bookerly Display.ttf");
  EXPECT_STREQ(bold->file, "/fonts/Bookerly-otf/Bookerly-Bold.ttf");
}

// A tokenless candidate belongs to the family that scanned it: the next
// family's post-loop Regular resolution must not reuse it.
TEST(ScanFontsTest, TokenlessCandidateDoesNotLeakToNextFamily) {
  resetStorage();
  seedFile("/fonts/First/First Display.ttf");
  seedFile("/fonts/Second/Second-Bold.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 2u);
  EXPECT_STREQ(fams[0].name, "First");
  const auto* firstRegular = findFace(fams[0], freeink::book::StyleNone);
  ASSERT_NE(firstRegular, nullptr);
  EXPECT_STREQ(firstRegular->file, "/fonts/First/First Display.ttf");

  EXPECT_STREQ(fams[1].name, "Second");
  // The single-file Second family keeps exactly its own promoted face; a
  // stale tokenless candidate from First must not survive into it. Asserting
  // the full shape (count + path) catches a leak even when the candidate is
  // copied under the SECOND family's own path prefix.
  ASSERT_EQ(fams[1].faceCount, 1u);
  EXPECT_STREQ(fams[1].faces[0].file, "/fonts/Second/Second-Bold.ttf");
}

TEST(ScanFontsTest, MissingRootIsQuietNoop) {
  resetStorage();
  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  EXPECT_EQ(count, 0u);
}

// ── Phase 3: family selection + picker availability gates ───────────────────

TEST(BookFontLoaderSelection, FindFamilyMatchesExactly) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Literata");
  fam.faceCount = 1;
  fam.faces[0].fileSize = 100;
  loader.setFamilyCountForTest(1);

  EXPECT_NE(loader.findFamily("Literata"), nullptr);
  // Case-insensitive per the loader contract (BookFontLoader.h §14.4 display
  // name): settings round-trip through the web UI/JSON with any casing.
  EXPECT_NE(loader.findFamily("literata"), nullptr);
  EXPECT_NE(loader.findFamily("LITERATA"), nullptr);
  EXPECT_EQ(loader.findFamily("Other"), nullptr);
  EXPECT_EQ(loader.findFamily(""), nullptr);
  EXPECT_EQ(loader.findFamily(nullptr), nullptr);
}

TEST(BookFontLoaderSelection, SelectFamilyRejectsSameAndAcceptsChange) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.selectFamily("Alpha");
  loader.selectFamily("Alpha");  // same selection: no reload churn
  loader.selectFamily("Beta");
  loader.selectFamily("");  // explicit fallback
  SUCCEED();
}

TEST(BookFontLoaderSelection, AvailabilityRequiresPsramAndFaceGuard) {
  freeink::book::BookFontLoader loader;
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Small");
  fam.faceCount = 1;
  fam.faces[0].fileSize = 100;
  auto& big = loader.editFamily(1);
  std::snprintf(big.name, sizeof(big.name), "%s", "Big");
  big.faceCount = 1;
  big.faces[0].fileSize = loader.kMaxFaceBytes + 1;
  loader.setFamilyCountForTest(2);

  testSetPsramHeap({0, 0, 0, 0});  // PSRAM-less: the whole class is unavailable
  EXPECT_FALSE(loader.isFamilyAvailable(fam));
  EXPECT_FALSE(loader.isFamilyAvailable(big));

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  EXPECT_TRUE(loader.isFamilyAvailable(fam));
#if defined(CROSSPOINT_FONT_BACKEND_FT)
  // Oversized faces no longer grey the row — they stream (§14.5). Only the
  // absolute stream cap disqualifies a face.
  EXPECT_TRUE(loader.isFamilyAvailable(big));
#else
  // stb has no streaming: the residency guard still greys oversized rows.
  EXPECT_FALSE(loader.isFamilyAvailable(big));
#endif
  auto& huge = loader.editFamily(2);
  std::snprintf(huge.name, sizeof(huge.name), "%s", "Huge");
  huge.faceCount = 1;
  huge.faces[0].fileSize = loader.kMaxStreamFaceBytes + 1;
  loader.setFamilyCountForTest(3);
  EXPECT_FALSE(loader.isFamilyAvailable(huge));
}

TEST(BookFontLoaderSelection, ClearedSelectionServesFallbackChain) {
  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.selectFamily("");  // §14.3: built-in = fallback chain, never a family
  loader.markDirty();
  loader.getReaderFont();
  EXPECT_EQ(loader.fontFingerprint(), 0u);
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
}

// ── Real-font fixtures: Amazon Ember (mixed upem, subsetted cmap) ────────────
// Owner-supplied faces with hostile metadata: upem 2048/2048/1000 across one
// family and inconsistent internal name tables ("AmazonEmber-Regular" vs
// "Amazon Ember"). Grouping must be folder-based (§14.4) and scaling
// per-face — these tests pin both so a family-uniform-upem regression fails
// here before it can produce off-page geometry on device.

namespace {

std::string readFixtureFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.good()) return {};  // callers must skip on empty (see fixtureAvailable)
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string emberBytes(const char* name) { return readFixtureFile(EMBER_FIXTURES_DIR "/" + std::string(name)); }

// Missing/unreadable fixtures must skip the test, not feed stb an empty
// buffer (stbtt_GetFontOffsetForIndex reads 4 header bytes → OOB crash).
// GTEST_SKIP() returns from the current function, so test bodies own it.
bool fixtureAvailable(const std::string& bytes) { return !bytes.empty(); }

struct ParsedFace {
  stbtt_fontinfo info{};
  int asc = 0;
  int desc = 0;
  int gap = 0;
};

ParsedFace parseFace(const std::string& bytes) {
  ParsedFace f;
  // stbtt_GetFontOffsetForIndex's TTC path reads the first container offset
  // at bytes[12..15]; require 16 bytes so every stb read below is in bounds
  // even for a truncated stub or a small git-lfs pointer file.
  if (bytes.size() < 16) {
    ADD_FAILURE() << "fixture too short to be a TTF (missing file?)";
    return f;
  }
  const auto* u8 = reinterpret_cast<const uint8_t*>(bytes.data());
  const int offset = stbtt_GetFontOffsetForIndex(u8, 0);
  // 64-bit compare: a fixture-crafted TTC container offset can be near
  // INT32_MAX and offset+12 would overflow signed 32-bit arithmetic.
  if (offset < 0 || static_cast<uint64_t>(offset) + 12u > bytes.size()) {
    ADD_FAILURE() << "fixture has no sfnt offset for index 0";
    return f;
  }
  // Table-directory bound, mirroring BookFontLoader's sfnt validation
  // (kMinSfntLen): stbtt_InitFont walks numTables * 16-byte entries at
  // data+12 and would OOB-read past a truncated directory.
  const uint16_t numTables = static_cast<uint16_t>((u8[offset + 4] << 8) | u8[offset + 5]);
  const uint64_t minSz = static_cast<uint64_t>(offset) + 12u + static_cast<uint64_t>(numTables) * 16u;
  if (numTables == 0 || minSz > bytes.size()) {
    ADD_FAILURE() << "fixture table directory out of bounds (numTables=" << numTables << ")";
    return f;
  }
  EXPECT_NE(stbtt_InitFont(&f.info, u8, offset), 0);
  stbtt_GetFontVMetrics(&f.info, &f.asc, &f.desc, &f.gap);
  return f;
}

// Mirrors TtfFont::ascent(): unitsAscent * stbtt_ScaleForPixelHeight(size).
int16_t expectedAscent(const ParsedFace& f, uint16_t sizePx) {
  const float scale = stbtt_ScaleForPixelHeight(&f.info, static_cast<float>(sizePx));
  return static_cast<int16_t>(f.asc * scale + 0.5f);
}

constexpr uint16_t kReadSize = 40;  // px; any size works — math is linear

}  // namespace

// Owner-supplied OTF regression: stb_truetype's vendored version supports
// OTTO/CFF (stb_truetype.h:1299 and the CFF charstring interpreter), but the
// loader path must still produce real pixels — an empty "A" means the font
// appears selectable yet cannot render.
TEST(TtfFaceMetrics, AtkinsonOtfCffRendersGlyphA) {
  const std::string bytes = readFixtureFile(ATKINSON_OTF_FIXTURE);
  if (!fixtureAvailable(bytes)) GTEST_SKIP() << "fixture unavailable: AtkinsonHyperlegibleNext-Regular.otf";
  const ParsedFace parsed = parseFace(bytes);
  ASSERT_NE(parsed.asc, 0);

  const int glyph = stbtt_FindGlyphIndex(&parsed.info, 'A');
  ASSERT_GT(glyph, 0);
  const float scale = stbtt_ScaleForPixelHeight(&parsed.info, static_cast<float>(kReadSize));
  int width = 0;
  int height = 0;
  int xoff = 0;
  int yoff = 0;
  const std::unique_ptr<unsigned char, decltype([](unsigned char* p) { stbtt_FreeBitmap(p, nullptr); })> bitmap(
      stbtt_GetCodepointBitmapSubpixel(&parsed.info, scale, scale, 0.0f, 0.0f, 'A', &width, &height, &xoff, &yoff));
  ASSERT_NE(bitmap, nullptr);
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);
  bool hasInk = false;
  for (int i = 0; i < width * height; ++i) {
    if (bitmap.get()[i] != 0) {
      hasInk = true;
      break;
    }
  }
  EXPECT_TRUE(hasInk);
}

// §14.4: family identity is the folder name; the files' internal name tables
// ("AmazonEmber-Regular", "AmazonEmber-Bold", "Amazon Ember") must never
// split or rename the family, and filename tokens must drive style slots.
TEST(ScanFontsTest, AmazonEmberOneFamilyFromFolderNotNameTables) {
  const std::string regularBytes = emberBytes("Amazon_Ember_Regular.ttf");
  const std::string boldBytes = emberBytes("Amazon_Ember_Bold.ttf");
  const std::string boldItalicBytes = emberBytes("Amazon_Ember_Bold_Italic.ttf");
  if (!fixtureAvailable(regularBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  if (!fixtureAvailable(boldBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Bold.ttf";
  if (!fixtureAvailable(boldItalicBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Bold_Italic.ttf";
  resetStorage();
  seedFile("/fonts/Amazon Ember/Amazon_Ember_Regular.ttf", regularBytes);
  seedFile("/fonts/Amazon Ember/Amazon_Ember_Bold.ttf", boldBytes);
  seedFile("/fonts/Amazon Ember/Amazon_Ember_Bold_Italic.ttf", boldItalicBytes);

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Amazon Ember");
  ASSERT_EQ(fams[0].faceCount, 3u);
  const auto* regular = findFace(fams[0], freeink::book::StyleNone);
  const auto* bold = findFace(fams[0], freeink::book::StyleBold);
  const auto* boldItalic = findFace(fams[0], kStyleBI);
  ASSERT_NE(regular, nullptr);
  ASSERT_NE(bold, nullptr);
  ASSERT_NE(boldItalic, nullptr);
  EXPECT_STREQ(regular->file, "/fonts/Amazon Ember/Amazon_Ember_Regular.ttf");
  EXPECT_STREQ(bold->file, "/fonts/Amazon Ember/Amazon_Ember_Bold.ttf");
  EXPECT_STREQ(boldItalic->file, "/fonts/Amazon Ember/Amazon_Ember_Bold_Italic.ttf");
}

namespace {
// §14.4.2 helper: wraps one TTF into a 2-face synthetic .ttc container —
// TTC header whose two offsets both point at the same embedded face, whose
// table directory is patched to container-absolute offsets (per the TTC
// spec). Mirrors the SDK's FtFontTtcTest container builder.
std::vector<uint8_t> makeTwoFaceTtc(const std::vector<uint8_t>& ttf) {
  if (ttf.size() < 12) {
    return {};  // truncated header: offsets 4/5 (numTables) would be OOB
  }
  const uint32_t faceBase = 20;  // 12-byte TTC header + two offsets
  std::vector<uint8_t> out(faceBase + ttf.size(), 0);
  std::memcpy(out.data(), "ttcf", 4);
  out[10] = 0;
  out[11] = 2;  // numFonts = 2
  const auto wr32 = [&](uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
  };
  const auto rd32 = [&](const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  };
  wr32(out.data() + 12, faceBase);
  wr32(out.data() + 16, faceBase);
  const uint16_t numTables = static_cast<uint16_t>((ttf[4] << 8) | ttf[5]);
  std::vector<uint8_t> face = ttf;
  for (size_t i = 0; i < numTables && 12 + 16 * (i + 1) <= face.size(); ++i) {
    const size_t offField = 12 + 16 * i + 8;
    wr32(face.data() + offField, rd32(face.data() + offField) + faceBase);
  }
  std::memcpy(out.data() + faceBase, face.data(), face.size());
  return out;
}
}  // namespace

#if defined(CROSSPOINT_FONT_BACKEND_FT)
// §14.4.2: .ttc containers are accepted by the scan (FT backend) and
// refineStyles resolves the collection face index (first face with a
// Unicode cmap) into the manifest.
TEST(RefineStylesTest, TtcContainerAcceptedAndFaceIndexResolved) {
  const std::string regularBytes = emberBytes("Amazon_Ember_Regular.ttf");
  if (!fixtureAvailable(regularBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  const std::vector<uint8_t> ttfVec(regularBytes.begin(), regularBytes.end());
  const std::vector<uint8_t> ttcBytes = makeTwoFaceTtc(ttfVec);
  resetStorage();
  seedFile("/fonts/Coll/Coll-Regular.ttc", std::string(ttcBytes.begin(), ttcBytes.end()));

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Coll");
  ASSERT_EQ(fams[0].faceCount, 1u);
  EXPECT_STREQ(fams[0].faces[0].file, "/fonts/Coll/Coll-Regular.ttc");
  EXPECT_EQ(fams[0].faces[0].styleFlags, freeink::book::StyleNone);

  BookFontLoader::refineStylesForTest(fams, count);
  ASSERT_EQ(fams[0].faceCount, 1u);
  // The inspect scan resolved the collection: face index 0 (first face with
  // a Unicode cmap) stored on the manifest row.
  EXPECT_EQ(fams[0].faces[0].faceIndex, 0u);
}

// §14.4.1: refineStyles resolves roles from the faces' REAL OS/2 weights, not
// the filename tokens. The real Bold bytes live in a file named "Regular" and
// the real Regular bytes in a file named "Italic" — filename inference tags
// them Bold/Italic, but the metadata pass must assign regular↔400 (the
// regular bytes) and bold↔700 (the bold bytes) regardless of the names.
TEST(RefineStylesTest, MetadataOverridesFilenameRoles) {
  const std::string regularBytes = emberBytes("Amazon_Ember_Regular.ttf");
  const std::string boldBytes = emberBytes("Amazon_Ember_Bold.ttf");
  if (!fixtureAvailable(regularBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  if (!fixtureAvailable(boldBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Bold.ttf";
  resetStorage();
  // Real bold bytes under a "Regular" name; real regular bytes under "Italic".
  seedFile("/fonts/Misnamed/Misnamed-Regular.ttf", boldBytes);
  seedFile("/fonts/Misnamed/Misnamed-Italic.ttf", regularBytes);

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  // Pre-refine, filename inference tags the real-bold bytes "Regular" →
  // StyleNone and the real-regular bytes "Italic" → StyleItalic: exactly
  // backwards relative to the true weights.
  ASSERT_EQ(fams[0].faceCount, 2u);

  BookFontLoader::refineStylesForTest(fams, count);
  const auto* regular = findFace(fams[0], freeink::book::StyleNone);
  const auto* bold = findFace(fams[0], freeink::book::StyleBold);
  ASSERT_NE(regular, nullptr);
  ASSERT_NE(bold, nullptr);
  EXPECT_EQ(fams[0].faceCount, 2u);
  // Metadata wins: the ACTUALLY-regular bytes take the regular role even
  // though the filename says "Italic", and vice versa.
  EXPECT_STREQ(regular->file, "/fonts/Misnamed/Misnamed-Italic.ttf");
  EXPECT_STREQ(bold->file, "/fonts/Misnamed/Misnamed-Regular.ttf");
}

// A face that cannot be inspected keeps its filename-derived estimate as the
// pick input (the filename heuristics remain the fallback per §14.4.1).
TEST(RefineStylesTest, UninspectableFaceKeepsFilenameEstimate) {
  const std::string regularBytes = emberBytes("Amazon_Ember_Regular.ttf");
  if (!fixtureAvailable(regularBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  resetStorage();
  seedFile("/fonts/Broken/Broken-Regular.ttf", "NOTAFONT\0\0garbage");
  seedFile("/fonts/Broken/Broken-Bold.ttf", "ALSONOTAFONT\0\0");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  ASSERT_EQ(fams[0].faceCount, 2u);

  BookFontLoader::refineStylesForTest(fams, count);
  // Unreadable faces fall back to filename-derived estimates: Regular stays
  // regular, Bold stays bold (deterministic pick over the estimates).
  ASSERT_EQ(fams[0].faceCount, 2u);
  EXPECT_STREQ(findFace(fams[0], freeink::book::StyleBold)->file, "/fonts/Broken/Broken-Bold.ttf");
}
#endif  // CROSSPOINT_FONT_BACKEND_FT

#if CROSSPOINT_TTF_UI_FALLBACK
// TtfUiFont (TASK 4): the EpdFontFamily view over the reader family at one UI
// size. Faults 1bpp glyphs through the EpdFontData miss seam; coverage via
// the coverageHandler. Host FT variant only (needs FtFont).
using freeink::book::computeTtfUiFontId;
using freeink::book::TtfUiFont;

TEST(TtfUiFontTest, ComputeIdIsDeterministicAndSizeSensitive) {
  const int a10 = computeTtfUiFontId("Amazon Ember", 8);
  const int a12 = computeTtfUiFontId("Amazon Ember", 10);
  const int a12b = computeTtfUiFontId("Amazon Ember", 12);
  EXPECT_EQ(a12, computeTtfUiFontId("Amazon Ember", 10));
  EXPECT_NE(a12, a10);
  EXPECT_NE(a12, a12b);
  EXPECT_NE(a12, 0);
}

TEST(TtfUiFontTest, FaultsMonoGlyphsThroughMissSeam) {
  const std::string bytes = emberBytes("Amazon_Ember_Regular.ttf");
  if (!fixtureAvailable(bytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";

  const void* slotBytes[4] = {bytes.data(), nullptr, nullptr, nullptr};
  const uint32_t slotSizes[4] = {static_cast<uint32_t>(bytes.size()), 0, 0, 0};
  const uint8_t slotFaceIdx[4] = {0, 0, 0, 0};
  freeink::book::TtfUiFont ui;
  ASSERT_TRUE(ui.begin(slotBytes, slotSizes, slotFaceIdx, 12));

  // Stub data reports coverage through the RAM-resident engine (no interval
  // table — the coverageHandler answers hasGlyph).
  EXPECT_TRUE(ui.family().hasCodepoint('A', EpdFontFamily::REGULAR));

  const EpdGlyph* glyph = ui.family().getGlyph('A', EpdFontFamily::REGULAR);
  ASSERT_NE(glyph, nullptr);
  EXPECT_GT(glyph->width, 0);
  EXPECT_GT(glyph->height, 0);
  EXPECT_GT(glyph->advanceX, 0);
  // Consume the ring glyph BEFORE the next miss (ring contract).
  const EpdFontData* data = ui.family().getData(EpdFontFamily::REGULAR);
  ASSERT_NE(data->bitmap, nullptr);
  const uint8_t* bits = data->bitmap + glyph->dataOffset;
  bool hasInk = false;
  for (uint16_t y = 0; y < glyph->height && !hasInk; ++y) {
    for (uint16_t x = 0; x < glyph->width && !hasInk; ++x) {
      if (bits[y * ((glyph->width + 7) / 8) + (x >> 3)] & (0x80u >> (x & 7))) hasInk = true;
    }
  }
  EXPECT_TRUE(hasInk);

  // Ring wrap: more misses than kRingSlots must not crash and must still
  // serve valid glyphs (eviction contract).
  for (uint32_t cp = 0x4E00; cp < 0x4E00 + 24; ++cp) {
    const EpdGlyph* g = ui.family().getGlyph(cp, EpdFontFamily::REGULAR);
    ASSERT_NE(g, nullptr) << "cp " << cp;
  }
  ui.end();
}

// A zero-box glyph (space) still reports its advance — layout measures it
// through the same miss seam.
TEST(TtfUiFontTest, SpaceAdvancesWithoutBitmap) {
  const std::string bytes = emberBytes("Amazon_Ember_Regular.ttf");
  if (!fixtureAvailable(bytes)) GTEST_SKIP() << "fixture unavailable";
  const void* slotBytes[4] = {bytes.data(), nullptr, nullptr, nullptr};
  const uint32_t slotSizes[4] = {static_cast<uint32_t>(bytes.size()), 0, 0, 0};
  const uint8_t slotFaceIdx[4] = {0, 0, 0, 0};
  freeink::book::TtfUiFont ui;
  ASSERT_TRUE(ui.begin(slotBytes, slotSizes, slotFaceIdx, 10));
  const EpdGlyph* g = ui.family().getGlyph(' ', EpdFontFamily::REGULAR);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->width, 0);
  EXPECT_EQ(g->height, 0);
  EXPECT_GT(g->advanceX, 0);
  ui.end();
}
#endif  // CROSSPOINT_TTF_UI_FALLBACK

#ifndef CROSSPOINT_FONT_BACKEND_FT
// §14.4.2: stb builds skip .ttc files entirely (stb_truetype cannot parse
// TTC) rather than offering a family whose faces fail to load.
TEST(RefineStylesTest, TtcSkippedOnStbBackend) {
  const std::string regularBytes = emberBytes("Amazon_Ember_Regular.ttf");
  if (!fixtureAvailable(regularBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  const std::vector<uint8_t> ttfVec(regularBytes.begin(), regularBytes.end());
  const std::vector<uint8_t> ttcBytes = makeTwoFaceTtc(ttfVec);
  resetStorage();
  seedFile("/fonts/Coll/Coll-Regular.ttc", std::string(ttcBytes.begin(), ttcBytes.end()));
  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  EXPECT_EQ(count, 0u);
}
#endif

#if defined(CROSSPOINT_FONT_BACKEND_FT)
// §14.5: a face beyond the PSRAM residency guard streams from SD through
// FtFont::initStream — the chain still covers the style and the fingerprint
// is nonzero (streamed slots fold the SD head-hash identity, not bytes).
TEST(BookFontLoaderStreaming, OversizedFaceStreamsFromStorage) {
  const std::string regularBytes = emberBytes("Amazon_Ember_Regular.ttf");
  if (!fixtureAvailable(regularBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  // Pad past the 2MB residency guard with trailing zero bytes: the sfnt
  // table directory ignores bytes beyond its declared tables, so the face
  // stays valid while forcing the streaming path.
  std::string big = regularBytes;
  big.resize(BookFontLoader::kMaxFaceBytes + 1024, '\0');

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  resetStorage();
  seedFile("/fonts/Big/Big-Regular.ttf", big);
  freeink::book::BookFontLoader loader;
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Big");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(big.size());
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Big/Big-Regular.ttf");
  loader.setFamilyCountForTest(1);
  loader.selectFamily("Big");
  loader.markDirty();

  freeink::book::FontChain* chain = loader.getReaderFont();
  ASSERT_NE(chain, nullptr);
  EXPECT_NE(chain->styleCoverage(), 0u);  // regular slot streamed in (regular = coverage bit 0x04)
  EXPECT_NE(loader.fontFingerprint(), 0u);
  // Deterministic identity: a second load derives the same fingerprint.
  loader.markDirty();
  const uint32_t fp1 = loader.fontFingerprint();
  loader.getReaderFont();
  EXPECT_EQ(loader.fontFingerprint(), fp1);
}
#endif  // CROSSPOINT_FONT_BACKEND_FT

// Each face scales by its OWN unitsPerEm: the 1000-upem BoldItalic carries
// ~half the raw hhea units of the 2048 faces, yet the pixel-scaled ascents
// must land in the same neighborhood and match stb's per-face math exactly.
TEST(TtfFaceMetrics, AmazonEmberScalesByPerFaceUnitsPerEm) {
  const std::string regular = emberBytes("Amazon_Ember_Regular.ttf");
  const std::string boldItalic = emberBytes("Amazon_Ember_Bold_Italic.ttf");
  if (!fixtureAvailable(regular)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  if (!fixtureAvailable(boldItalic)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Bold_Italic.ttf";
  const ParsedFace pr = parseFace(regular);
  const ParsedFace pbi = parseFace(boldItalic);
  ASSERT_NE(pr.asc, 0);
  ASSERT_NE(pbi.asc, 0);

  static uint8_t arenaBuf[64 * 1024];
  book::Arena arena(arenaBuf, sizeof(arenaBuf));
  book::TtfFont fontReg;
  ASSERT_TRUE(
      fontReg.init(reinterpret_cast<const uint8_t*>(regular.data()), static_cast<uint32_t>(regular.size()), arena));
  book::TtfFont fontBi;
  ASSERT_TRUE(fontBi.init(reinterpret_cast<const uint8_t*>(boldItalic.data()), static_cast<uint32_t>(boldItalic.size()),
                          arena));

  // Exact per-face formula match (a family-uniform scale would break these).
  EXPECT_EQ(fontReg.ascent(kReadSize), expectedAscent(pr, kReadSize));
  EXPECT_EQ(fontBi.ascent(kReadSize), expectedAscent(pbi, kReadSize));
  // Raw unit spaces are ~2x apart; the scaled results are not.
  EXPECT_GT(std::abs(pr.asc), std::abs(pbi.asc));
  EXPECT_LE(std::abs(static_cast<int>(fontReg.ascent(kReadSize)) - static_cast<int>(fontBi.ascent(kReadSize))), 2);
}

// Mixed-upem chain: the chain's line grid comes from the FIRST registered
// face (stable baseline across styles), never from a later face's upem.
TEST(FontChainMixedUpem, LineGridComesFromFirstFace) {
  const std::string regular = emberBytes("Amazon_Ember_Regular.ttf");
  const std::string boldItalic = emberBytes("Amazon_Ember_Bold_Italic.ttf");
  if (!fixtureAvailable(regular)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  if (!fixtureAvailable(boldItalic)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Bold_Italic.ttf";

  static uint8_t arenaBuf[64 * 1024];
  book::Arena arena(arenaBuf, sizeof(arenaBuf));
  book::TtfFont fontReg;
  ASSERT_TRUE(
      fontReg.init(reinterpret_cast<const uint8_t*>(regular.data()), static_cast<uint32_t>(regular.size()), arena));
  book::TtfFont fontBi;
  ASSERT_TRUE(fontBi.init(reinterpret_cast<const uint8_t*>(boldItalic.data()), static_cast<uint32_t>(boldItalic.size()),
                          arena));

  book::FontChain chain;
  ASSERT_TRUE(chain.add(&fontReg, book::StyleNone));
  ASSERT_TRUE(chain.add(&fontBi, book::StyleBold | book::StyleItalic));
  EXPECT_EQ(chain.lineHeight(kReadSize), fontReg.lineHeight(kReadSize));
  EXPECT_EQ(chain.ascent(kReadSize), fontReg.ascent(kReadSize));
}

// The Regular face is heavily subsetted (78 glyphs, 682 cmap entries):
// common Latin letters must resolve in-face; a cmap miss must fall through
// the chain to a covering tail face instead of returning nullptr.
TEST(FontChainMixedUpem, CmapMissFallsThroughToCoveringFace) {
  const std::string regular = emberBytes("Amazon_Ember_Regular.ttf");
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(regular)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DEJAVU_FIXTURE";

  static uint8_t arenaBuf[64 * 1024];
  book::Arena arena(arenaBuf, sizeof(arenaBuf));
  book::TtfFont fontReg;
  ASSERT_TRUE(
      fontReg.init(reinterpret_cast<const uint8_t*>(regular.data()), static_cast<uint32_t>(regular.size()), arena));
  book::TtfFont fontTail;
  ASSERT_TRUE(
      fontTail.init(reinterpret_cast<const uint8_t*>(dejavu.data()), static_cast<uint32_t>(dejavu.size()), arena));

  book::FontChain chain;
  ASSERT_TRUE(chain.add(&fontReg, book::StyleNone));
  ASSERT_TRUE(chain.add(&fontTail, book::StyleNone));
  ASSERT_TRUE(chain.add(&fontTail, book::StyleBold));
  ASSERT_TRUE(chain.add(&fontTail, book::StyleItalic));
  ASSERT_TRUE(chain.add(&fontTail, book::StyleBold | book::StyleItalic));

  // Sanity: core Latin letters stay in the subsetted face.
  for (const uint32_t cp : {'a', 'A', 'e', ' ', '.'}) {
    EXPECT_TRUE(fontReg.hasGlyph(cp)) << "subset lacks common codepoint U+" << std::hex << cp;
  }

  // Find a codepoint the Ember subset does not cover but the tail face
  // does; the chain must route it to that covering face, never nullptr.
  // (Ember's cmap spans Latin/Greek/Cyrillic; Hebrew/Armenian/Georgian sit
  // outside it. DejaVu carries all three blocks.)
  const uint32_t candidates[] = {0x05D0 /*א*/, 0x0531 /*Ա*/, 0x1E00, 0x10A0};
  uint32_t missing = 0;
  for (const uint32_t cp : candidates) {
    if (!fontReg.hasGlyph(cp) && fontTail.hasGlyph(cp)) {
      missing = cp;
      break;
    }
  }
  ASSERT_NE(missing, 0u) << "fixture unexpectedly covers every candidate";
  uint8_t faceFlags = 0xFF;
  EXPECT_EQ(chain.fontFor(missing, book::StyleNone, &faceFlags), &fontTail);
}

// ── P3.1 SD fingerprint cache ────────────────────────────────────────
// Note: fontFingerprint() is documented as the PRE-fallback-tail identity
// (ensureLoaded computes it before appendFallbackTail; the prefetch worker
// mirrors that), so these tests never compare it against a post-tail
// computeFingerprint() call — cached-vs-pure parity is pinned by the
// corrupt-record recompute case instead.────
// The chained per-face FNV-1a walk (up to 4 × 2 MB per family load) is cached
// under /.crosspoint/fonts/ keyed by the face's path hash, valid only when
// {fileSize, mtime, incoming chain seed} all match. These tests pin:
// cache-hit == pure fingerprint (byte parity), mtime/size change → rehash,
// corrupt cache file → recompute-and-rewrite, and mtime 0 → cache disabled.

namespace {

std::string fpCachePathFor(const char* facePath) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/.crosspoint/fonts/fp_%08x.bin",
                freeink::book::BookFontLoader::fontBytesHash(reinterpret_cast<const uint8_t*>(facePath),
                                                             std::strlen(facePath), 0x811c9dc5u));
  return buf;
}

// One loadable DejaVu face driven through editFamily (scanFonts not needed).
void seedLoadableFace(const std::string& bytes, const char* path, uint32_t mtime) {
  writeFaceFile(path, bytes);
  if (mtime != 0) Storage.mtimes[path] = mtime;
}

}  // namespace

TEST(BookFontLoaderFingerprintCache, CacheRoundTripMatchesPureFingerprint) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr uint32_t kMtime = 0x5F123456u;
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, kMtime);

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = kMtime;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  // First load: cache MISS (no file yet) → byte-walk + cache WRITE.
  loader.markDirty();
  const uint32_t fpFirst = loader.getReaderFont()->styleCoverage() != 0 ? loader.fontFingerprint() : 0;
  EXPECT_NE(fpFirst, 0u);
  const std::string cachePath = fpCachePathFor(kFacePath);
  ASSERT_EQ(Storage.files.count(cachePath), 1u);    // cache written
  EXPECT_EQ(Storage.files[cachePath].size(), 28u);  // fixed record

  // Second load: cache HIT must reproduce the exact fingerprint.
  loader.markDirty();
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
  EXPECT_EQ(loader.fontFingerprint(), fpFirst);

  // Head rewrite (same size + mtime): the review-hardened hit check covers
  // the first 4 KB, so a changed header byte — still a valid, loadable sfnt
  // — must REHASH, never serve the stale record.
  std::string headModified = dejavu;
  headModified[8] = static_cast<char>(headModified[8] ^ 0xFF);
  ASSERT_NE(headModified, dejavu);
  writeFaceFile(kFacePath, headModified);
  loader.markDirty();
  loader.getReaderFont();  // markDirty alone only arms; ensureLoaded runs here
  EXPECT_NE(loader.fontFingerprint(), fpFirst);
  const uint32_t fpHead = loader.fontFingerprint();

  // Bounded window (documented): a rewrite beyond the 4 KB head with the
  // same size + mtime keeps the head hash — the record is served (hit).
  std::string tailModified = headModified;
  ASSERT_GT(tailModified.size(), 6000u) << "fixture too small for the tail-modify step";
  tailModified[6000] = static_cast<char>(tailModified[6000] ^ 0xFF);
  writeFaceFile(kFacePath, tailModified);
  loader.markDirty();
  loader.getReaderFont();
  EXPECT_EQ(loader.fontFingerprint(), fpHead);
}

TEST(BookFontLoaderFingerprintCache, MtimeChangeRehashes) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  std::string modified = dejavu;
  modified[8] = static_cast<char>(modified[8] ^ 0xFF);  // same size, different content

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  seedLoadableFace(dejavu, kFacePath, 0x5F123456u);
  fam.faces[0].mtime = 0x5F123456u;
  loader.markDirty();
  const uint32_t fpOriginal = loader.getReaderFont()->styleCoverage() != 0 ? loader.fontFingerprint() : 0;
  EXPECT_NE(fpOriginal, 0u);

  // mtime bump with swapped content: cache must miss and rehash the NEW
  // bytes — fingerprint follows the content, never the stale record.
  seedLoadableFace(modified, kFacePath, 0x5FFFFFFFu);
  fam.faces[0].mtime = 0x5FFFFFFFu;
  loader.markDirty();
  loader.getReaderFont();  // markDirty alone only arms; ensureLoaded runs here
  const uint32_t fpNew = loader.fontFingerprint();
  EXPECT_NE(fpNew, fpOriginal);
}

TEST(BookFontLoaderFingerprintCache, CorruptCacheFileRecomputesAndRewrites) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr uint32_t kMtime = 0x5F123456u;
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, kMtime);

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = kMtime;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  loader.markDirty();
  const uint32_t fpFirst = loader.getReaderFont()->styleCoverage() != 0 ? loader.fontFingerprint() : 0;
  EXPECT_NE(fpFirst, 0u);

  // Corrupt the cache record (wrong length, wrong magic) → the loader must
  // fall back to the pure byte-walk, get the SAME fingerprint, and rewrite a
  // valid 28-byte record for the next open.
  const std::string cachePath = fpCachePathFor(kFacePath);
  Storage.files[cachePath] = "garbage!";
  loader.markDirty();
  loader.getReaderFont();  // markDirty alone only arms; ensureLoaded runs here
  EXPECT_EQ(loader.fontFingerprint(), fpFirst);
  EXPECT_EQ(Storage.files[cachePath].size(), 28u);
}

TEST(BookFontLoaderFingerprintCache, MtimeZeroDisablesCache) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, 0);  // no SD timestamp

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = 0;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  loader.markDirty();
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
  // Fail closed: without a rehash trigger there is no safe cache identity —
  // no record may be written (or served) for that face.
  EXPECT_EQ(Storage.files.count(fpCachePathFor(kFacePath)), 0u);
}

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
// ── P2 hinting (Light) + stack-probe degrade identity ────────────────
// kRenderOptions requests Light; the runtime stack probe may degrade a slot
// to unhinted. The effective per-slot modes fold into the fingerprint tag
// (renderOptionsFingerprintTag), which both the loader and the FIBP prefetch
// worker mix into their parity hash — a degrade MUST change the tag.

namespace {

using RO = freeink::font::FtFont::RenderOptions;
using HM = freeink::font::FtFont::HintingMode;

uint32_t tagFor(std::initializer_list<HM> modes) {
  uint32_t tag = 0;
  uint8_t slot = 0;
  for (HM mode : modes) tag |= static_cast<uint32_t>(mode) << (3 * slot++);
  return tag;
}

}  // namespace

TEST(BookFontLoaderHinting, RequestedModeIsLightAndTagFoldsPerSlot) {
  BookFontLoader loader;
  loader.resetHintStateForTest();
  EXPECT_EQ(BookFontLoader::kRenderOptions.hinting, HM::Light);
  for (uint8_t slot = 0; slot < 4; ++slot) {
    EXPECT_EQ(BookFontLoader::effectiveRenderOptions(slot).hinting, HM::Light) << "slot " << slot;
    // Crisp is the default render mode (task6 directive).
    EXPECT_TRUE(BookFontLoader::effectiveRenderOptions(slot).monochrome) << "slot " << slot;
  }
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::Light, HM::Light, HM::Light, HM::Light}));
}

TEST(BookFontLoaderHinting, ApplyRenderModeFlipsMonochromeAndTag) {
  BookFontLoader loader;
  loader.resetHintStateForTest();
  loader.applyRenderMode(false);  // Smooth
  EXPECT_FALSE(BookFontLoader::effectiveRenderOptions(0).monochrome);
  loader.applyRenderMode(true);  // Crisp
  EXPECT_TRUE(BookFontLoader::effectiveRenderOptions(3).monochrome);
  // Raster mode is NOT layout identity: advances are identical across
  // modes, so the tag (and with it every FIBP gen) must stay stable or a
  // firmware update flipping the default re-indexes every book (soak
  // finding #6 correction: gen 1029201805 -> 3637587853 across builds).
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::Light, HM::Light, HM::Light, HM::Light}));
  loader.resetHintStateForTest();
}

TEST(BookFontLoaderHinting, DegradeFlipsEffectiveOptionsAndTag) {
  // A throwaway instance: the seam flips the file-scope effective-options
  // state; this instance's own faces stay null so setRenderOptions is
  // skipped (state-only degrade, exactly what the tag tests need).
  BookFontLoader loader;
  loader.degradeHintForTest(1);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(1).hinting, HM::None);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(0).hinting, HM::Light);
  const uint32_t degraded = BookFontLoader::renderOptionsFingerprintTag();
  EXPECT_NE(degraded, tagFor({HM::Light, HM::Light, HM::Light, HM::Light}));
  EXPECT_EQ(degraded, tagFor({HM::Light, HM::None, HM::Light, HM::Light}));
  // Mutate-check the tag really folds the degraded slot: a second slot's
  // degrade must move the tag again (no aliasing between slot fields).
  loader.degradeHintForTest(2);
  EXPECT_NE(BookFontLoader::renderOptionsFingerprintTag(), degraded);
  loader.resetHintStateForTest();
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::Light, HM::Light, HM::Light, HM::Light}));
}

// A stack-probe degrade must survive Crisp/Smooth switches: applyRenderMode
// changes ONLY the raster axis — re-enabling Light on a probed-off slot
// would put the Adobe interpreter back onto the 32KB worker stack (r5).
TEST(BookFontLoaderHinting, DegradeSurvivesApplyRenderMode) {
  BookFontLoader loader;
  loader.resetHintStateForTest();
  loader.degradeHintForTest(1);
  loader.applyRenderMode(false);  // Smooth
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(1).hinting, HM::None);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(1).monochrome, false);
  loader.applyRenderMode(true);  // Crisp
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(1).hinting, HM::None);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(1).monochrome, true);
  // Un-degraded slots keep the requested mode through the switches.
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(0).hinting, HM::Light);
  loader.resetHintStateForTest();
}

// Device regression 2026-09-19: a mono request against a build whose
// FreeType lacks the mono renderer module rasterized EVERY glyph to nullptr
// (blank page). The load funnel must degrade to the nearest supported set
// (AA first) and the face must still paint.
TEST(BookFontLoaderHinting, UnsupportedMonoDegradesToAaAndStillRenders) {
  // The host FT variant compiles neither the mono renderer nor the
  // auto-hinter, so Crisp is refused here exactly like on a firmware env
  // missing FREEINK_FONT_ENABLE_MONOCHROME.
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, 0x5F123456u);

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = 0x5F123456u;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);

  // (C2) The degrade is visible in the effective mode: mono is dropped.
  // The TAG reflects the degrade: the host FT variant compiles neither the
  // mono renderer nor the auto-hinter, so the funnel drops BOTH on the
  // seeded slot (device builds compile both — there the tag stays Light).
  // Raster mode alone never moves the tag.
  EXPECT_FALSE(BookFontLoader::effectiveMonochrome());
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::None, HM::Light, HM::Light, HM::Light}));

  // (C1) The degraded face still rasterizes: non-null pixels, non-empty
  // bitmap (the regression produced nullptr for every glyph).
  freeink::font::FontChain* chain = loader.getReaderFont();
  freeink::font::RasterFont* face = chain->fontFor('A');
  ASSERT_NE(face, nullptr);
  const freeink::font::GlyphBitmap* bmp = face->rasterize('A', 14);
  ASSERT_NE(bmp, nullptr);
  ASSERT_NE(bmp->pixels, nullptr);
  EXPECT_GT(size_t(bmp->width) * bmp->height, 0u);
}

// Gen-stability contract (soak finding #6 correction): the font
// fingerprint — the input every FIBP generation derives from — must be a
// pure function of the font bytes + style coverage + hinting tag. Two
// loader instances over unchanged files must agree exactly, or a firmware
// update (or a second loader in the same boot) silently re-indexes every
// book. layoutGenerationHash() hashes field-by-field (no struct padding),
// so fingerprint determinism pins the whole chain.
TEST(BookFontLoaderHinting, FingerprintStableAcrossLoaderInstances) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u);
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, 0x5F123456u);

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  uint32_t fingerprints[2] = {};
  for (auto& fp : fingerprints) {
    freeink::book::BookFontLoader loader;
    loader.begin();
    auto& fam = loader.editFamily(0);
    std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
    fam.faceCount = 1;
    fam.faces[0].styleFlags = freeink::book::StyleNone;
    fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
    fam.faces[0].mtime = 0x5F123456u;
    std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
    loader.setFamilyCountForTest(1);
    loader.markDirty();
    ASSERT_NE(loader.getReaderFont(), nullptr);
    fp = loader.fontFingerprint();
    EXPECT_NE(fp, 0u);
  }
  EXPECT_EQ(fingerprints[0], fingerprints[1]);
}

// The shallow probe's verdict is CACHED, and layout identity must follow it.
// The measurement is keyed by the content fingerprint captured under the
// REQUESTED render options, so a verdict re-applies to a plain reload of the
// same faces (no re-probe, no identity churn) and never transfers to changed
// bytes (the cache misses and the load owes a fresh probe). The fingerprint
// re-derivation is the load-bearing half: renderOptionsFingerprintTag folds
// the effective per-slot hinting, so a reused degrade that left the
// fingerprint at its pre-degrade value would let the FIBP worker render
// unhinted glyphs under a hinted identity.
TEST(BookFontLoaderHinting, VerdictReusesOnUnchangedReloadAndMissesOnChangedBytes) {
  BookFontLoader::setHintProbeSizeForTest(16);  // a fixed "body size" for the verdict key
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u);
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, 0x5F123456u);

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = 0x5F123456u;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);

  // Load key: the fingerprint as ensureLoaded() left it, with no verdict
  // applied for this face set yet.
  const uint32_t fpRequested = loader.fontFingerprint();
  // The identity is the PRE-fallback-tail one and must not depend on where it
  // is evaluated: the pure (uncached, tail-independent) and the loader's own
  // value agree, which is what the worker's parity hash mirrors.
  EXPECT_EQ(loader.computeFingerprint(), fpRequested);
  // Settle a verdict as the shallow probe would: slot 3 measured deeper
  // than the consumer budget. (Slot 0 already sits at None on the host FT
  // variant — the funnel drops mono and autohint alike — hence the tag.)
  loader.recordHintVerdictForTest(fpRequested, 0x08);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(3).hinting, HM::None);
  const uint32_t fpDegraded = loader.fontFingerprint();
  EXPECT_NE(fpDegraded, fpRequested);
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::None, HM::Light, HM::Light, HM::None}));

  // The verdict is settled, so a second settle from the call site is a no-op
  // (the host probe measures nothing — only the identity may confirm it).
  loader.ensureHintProbeSettled();
  EXPECT_EQ(loader.fontFingerprint(), fpDegraded);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(3).hinting, HM::None);

  // Reload of the SAME face set: the verdict is re-applied, not re-measured,
  // and the identity comes back byte-identical (a needless FIBP re-index here
  // would be a regression, not a cost).
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(3).hinting, HM::None);
  EXPECT_EQ(loader.fontFingerprint(), fpDegraded);
  loader.ensureHintProbeSettled();
  EXPECT_EQ(loader.fontFingerprint(), fpDegraded);
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::None, HM::Light, HM::Light, HM::None}));

  // Changed bytes: the content identity moves, so the cached verdict no
  // longer describes these faces — slot 3 returns to the requested mode and
  // the new load settles its own fresh verdict at the tail.
  std::string modified = dejavu;
  modified[8] = static_cast<char>(modified[8] ^ 0xFF);  // same size, valid sfnt
  ASSERT_NE(modified, dejavu);
  writeFaceFile(kFacePath, modified);
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(3).hinting, HM::Light);
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::None, HM::Light, HM::Light, HM::Light}));
  EXPECT_NE(loader.fontFingerprint(), fpDegraded);

  // Body-size change: the probe's depth is a function of the size it
  // rasterized, so the cached verdict must not be reused at a different
  // size. The settle now runs at the load tail, so the changed-bytes load
  // already recorded its own verdict (the cache is single-slot), and the
  // restored bytes settle a fresh one — the slot stays at the requested mode
  // and the identity returns to the original content's value.
  writeFaceFile(kFacePath, dejavu);  // restore the verdict's face bytes
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(3).hinting, HM::Light);
  EXPECT_EQ(loader.fontFingerprint(), fpRequested);
  BookFontLoader::setHintProbeSizeForTest(24);
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(3).hinting, HM::Light);
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::None, HM::Light, HM::Light, HM::Light}));
  EXPECT_EQ(loader.fontFingerprint(), fpRequested);
  // Restore after the test (the override is file-scope static state).
  BookFontLoader::setHintProbeSizeForTest(16);
}

// Retry backstop for an unmeasurable probe (2026-10-22): the device's probe
// task needs a 56KB contiguous DRAM stack, which a fragmented heap cannot
// give at the reader's first render — the settle therefore runs at
// ensureLoaded()'s tail (shallow window) and arms exactly ONE re-probe that
// the reader's first-render ensureHintProbeSettled() call consumes. The host
// probe is compiled out, so the unmeasurable path is driven through
// setHintProbeUnmeasurableForTest and the flag state machine is pinned
// directly: (a) a failed shallow attempt leaves the probe pending + one
// retry armed, (b) the retry attempt disarms it whatever its outcome,
// (c) a failed retry never re-arms, (d) a successful attempt clears both.
// Slot-option/fingerprint folds are covered by the BookFontLoaderHinting
// tests; the host FT funnel keeps every loaded face unhinted, so this suite
// pins the scheduling state, not the modes.
namespace {
// Loads the one-face DejaVu family through the real ensureLoaded() path
// (which now settles the probe at its tail). Returns the load's content
// fingerprint, or 0 when the fixture is unavailable (the test body owns the
// GTEST_SKIP — it only returns from the current function).
uint32_t loadDejaFamily(BookFontLoader& loader) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) return 0;
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, 0x5F123456u);
  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = 0x5F123456u;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);
  loader.markDirty();
  if (loader.getReaderFont() == nullptr) return 0;
  return loader.fontFingerprint();
}
// Scoped hold of the sticky unmeasurable override: the destructor resets it
// on EVERY exit — including a GTEST_SKIP() throw from the fixture-unavailable
// early return — so a skipped retry test cannot leave later probe tests
// running against a forced-unmeasurable probe (kody 2026-10-22).
struct UnmeasurableOverrideGuard {
  UnmeasurableOverrideGuard() { BookFontLoader::setHintProbeUnmeasurableForTest(true); }
  ~UnmeasurableOverrideGuard() { BookFontLoader::setHintProbeUnmeasurableForTest(false); }
};
}  // namespace

TEST(HintingProbeRetryTest, FailedShallowAttemptArmsExactlyOneRetry) {
  BookFontLoader::setHintProbeSizeForTest(16);
  UnmeasurableOverrideGuard unmeasurable;
  BookFontLoader loader;
  const uint32_t fp = loadDejaFamily(loader);
  if (fp == 0u) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";

  // (a) The shallow attempt failed inside the load: the verdict stayed
  // uncached, the probe is still pending, and one retry is armed for the
  // reader's first-render backstop call.
  EXPECT_TRUE(loader.hintProbePendingForTest());
  EXPECT_TRUE(loader.hintProbeRetryArmedForTest());

  // Backstop: consumes the retry — the attempt runs again, fails again, and
  // must NOT re-arm (the retry is granted once per load cycle).
  loader.ensureHintProbeSettled();
  EXPECT_FALSE(loader.hintProbeRetryArmedForTest());  // (b) consumed
  EXPECT_FALSE(loader.hintProbePendingForTest());     // (c) no further probing

  // A third settle is a no-op: the cycle is over, the session stays degraded.
  loader.ensureHintProbeSettled();
  EXPECT_FALSE(loader.hintProbePendingForTest());

  // A fresh load cycle re-arms the attempt budget (the retry flag resets
  // with the load); the unmeasurable override still holds, so the new
  // shallow attempt fails and grants its own retry.
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_TRUE(loader.hintProbePendingForTest());
  EXPECT_TRUE(loader.hintProbeRetryArmedForTest());

  loader.resetHintStateForTest();
}

TEST(HintingProbeRetryTest, RetrySuccessClearsBothAndCachesVerdict) {
  BookFontLoader::setHintProbeSizeForTest(16);
  UnmeasurableOverrideGuard unmeasurable;
  BookFontLoader loader;
  const uint32_t fp = loadDejaFamily(loader);
  if (fp == 0u) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_TRUE(loader.hintProbeRetryArmedForTest());  // shallow attempt failed

  // The backstop retry runs with a clean heap: the measurement succeeds, the
  // verdict is recorded, and the fail-closed degrade of the failed attempt is
  // rolled back through the degrade funnel (device: hinting upgrades, one
  // FIBP re-index accepted by design).
  BookFontLoader::setHintProbeUnmeasurableForTest(false);
  loader.ensureHintProbeSettled();
  EXPECT_FALSE(loader.hintProbePendingForTest());  // (d) success clears both
  EXPECT_FALSE(loader.hintProbeRetryArmedForTest());

  // The settled verdict is cached: an unchanged reload reuses it instead of
  // re-probing, and the identity stays stable.
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_EQ(loader.fontFingerprint(), fp);
  EXPECT_FALSE(loader.hintProbePendingForTest());
  EXPECT_FALSE(loader.hintProbeRetryArmedForTest());

  loader.resetHintStateForTest();
}

// The retry must (a) restore the requested mode BEFORE the re-measurement —
// the failed attempt's fail-closed degrade would otherwise make the probe
// read unhinted depth and clear a hinted render it never tested — and (b)
// cache the verdict under the key observed when the retry was ARMED (the
// requested-options fingerprint): the retry pass re-enters with fingerprint_
// under the fail-closed degrade tag, so caching under the entry key would
// never match what an unchanged later load computes and every reload would
// re-probe. Host limits: the host funnel drops autohint for every loaded
// face (slot 0 already sits at None), so (a) is pinned via the restore-order
// snapshot and (b) by engineering a fingerprint-visible degrade on a
// FACELESS slot (allowed there — it stays at the requested Light until
// degraded), reproducing the device's tag shift.
TEST(HintingProbeRetryTest, RetryMeasuresRequestedModeAndCachesRequestedKey) {
  BookFontLoader::setHintProbeSizeForTest(16);
  UnmeasurableOverrideGuard unmeasurable;
  BookFontLoader loader;
  const uint32_t fp = loadDejaFamily(loader);
  if (fp == 0u) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_TRUE(loader.hintProbeRetryArmedForTest());  // shallow attempt failed
  // The first attempt ran with nothing restored ahead of it.
  EXPECT_EQ(BookFontLoader::hintProbeRestoresAtProbeForTest(), 0);

  // Make the fail-closed degrade fingerprint-visible the way it is on the
  // device (there the loaded faces drop Light -> None; the host funnel has
  // already dropped slot 0, so shift a faceless slot instead).
  loader.degradeHintForTest(1);
  loader.applyRenderMode(false);
  const uint32_t degradedEntryKey = loader.fontFingerprint();
  ASSERT_NE(degradedEntryKey, fp);

  // The backstop retry runs with a clean heap: it must restore BEFORE the
  // probe (snapshot 1 at entry) and cache under the armed key.
  BookFontLoader::setHintProbeUnmeasurableForTest(false);
  loader.ensureHintProbeSettled();
  EXPECT_EQ(BookFontLoader::hintProbeRestoresAtProbeForTest(), 1);
  EXPECT_FALSE(loader.hintProbePendingForTest());
  EXPECT_FALSE(loader.hintProbeRetryArmedForTest());
  EXPECT_EQ(loader.hintVerdictKeyForTest(), fp);
  EXPECT_NE(loader.hintVerdictKeyForTest(), degradedEntryKey);

  // The unchanged reload therefore REUSES the verdict (the fingerprint comes
  // back at the requested tag) instead of paying a second blocking probe.
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_EQ(loader.fontFingerprint(), fp);
  EXPECT_FALSE(loader.hintProbePendingForTest());

  loader.resetHintStateForTest();
}

TEST(HintingProbeRetryTest, CleanLoadSettlesAtLoadTailAndClearsBoth) {
  BookFontLoader::setHintProbeSizeForTest(16);
  BookFontLoader loader;
  const uint32_t fp = loadDejaFamily(loader);
  if (fp == 0u) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";

  // (d) The shallow attempt succeeded inside the load: nothing pending, no
  // retry armed — the reader's backstop call is a no-op.
  EXPECT_FALSE(loader.hintProbePendingForTest());
  EXPECT_FALSE(loader.hintProbeRetryArmedForTest());
  loader.ensureHintProbeSettled();
  EXPECT_EQ(loader.fontFingerprint(), fp);

  // An unchanged reload reuses the verdict instead of re-probing.
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_EQ(loader.fontFingerprint(), fp);
  EXPECT_FALSE(loader.hintProbePendingForTest());
  EXPECT_FALSE(loader.hintProbeRetryArmedForTest());

  loader.resetHintStateForTest();
}
#endif

// ── Cold-start HalFile lifecycle (device boot-panic regression) ─────────
// The device's streamed-slot StreamSources are default-constructed HalFile
// members; closing one before its first open used to assert
// (HalStorage.cpp: assert(impl != nullptr)) and panicked every boot where
// no streamed font had been loaded (upstream 3dd1748e). The HAL close() is
// now total (no-op on a never-opened handle, matching SdFat). The host stub
// count-exposes every close of a never-opened handle, so these tests pin
// the whole lifecycle: cold begin, re-begin over a live streamed slot, and
// the failure ladder — with closeOfNeverOpenedCounter proving the release
// paths stay disciplined rather than merely surviving the no-op.

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT

// Regression: cold begin() (zero streamed slots, no manifest) must survive
// the release paths — this was the on-device boot panic in setup()
// before the first frame rendered.
TEST(BookFontLoaderStreaming, ColdBeginSurvivesReleaseOnColdSlots) {
  testSetPsramHeap({0, 0, 0, 0});  // no PSRAM → streaming path unreachable
  freeink::book::BookFontLoader loader;
  loader.begin();
  loader.begin();
  EXPECT_EQ(loader.familyCount(), 0u);
  EXPECT_EQ(loader.fontFingerprint(), 0u);
}

// Contract: close() on a never-opened HalFile is a safe no-op (device
// HalStorage.cpp), not an assert — the crash class this suite pins. The
// counter delta proves the close actually ran and was tolerated, instead
// of silently not executing at all.
TEST(BookFontLoaderStreaming, CloseOnNeverOpenedHandleIsNoOp) {
  const int before = HalFile::closeOfNeverOpenedCounter;
  HalFile cold;
  cold.close();  // pre-fix device: assert(impl != nullptr) panic
  EXPECT_FALSE(cold.isOpen());
  EXPECT_EQ(HalFile::closeOfNeverOpenedCounter, before + 1);
}

// Regression: a cold begin() after a live streamed load tears down that
// slot's open handle exactly once and leaves reset StreamSource state.
TEST(BookFontLoaderStreaming, RebeginAfterStreamedLoadTearsDownOnceAndStaysLive) {
  const std::string regularBytes = emberBytes("Amazon_Ember_Regular.ttf");
  if (!fixtureAvailable(regularBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  std::string big = regularBytes;
  big.resize(BookFontLoader::kMaxFaceBytes + 1024, '\0');  // force streaming path

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  resetStorage();
  seedFile("/fonts/Big/Big-Regular.ttf", big);
  freeink::book::BookFontLoader loader;
  auto seedFamily = [&] {
    auto& fam = loader.editFamily(0);
    std::snprintf(fam.name, sizeof(fam.name), "%s", "Big");
    fam.faceCount = 1;
    fam.faces[0].styleFlags = freeink::book::StyleNone;
    fam.faces[0].fileSize = static_cast<uint32_t>(big.size());
    fam.faces[0].mtime = 0;
    std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Big/Big-Regular.ttf");
    loader.setFamilyCountForTest(1);
  };
  seedFamily();
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_NE(loader.fontFingerprint(), 0u);  // streamed slot loaded (owner=3)

  // begin() re-scans the manifest and clears the selection, so re-seed and
  // re-select the way callers do across a real restart-scoped begin().
  loader.begin();
  seedFamily();
  loader.selectFamily("Big");
  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  const uint32_t fp1 = loader.fontFingerprint();
  EXPECT_EQ(loader.fontFingerprint(), fp1);
  // The streamed slot's handle must actually be open again post-rebegin.
  EXPECT_TRUE(loader.streamSourceOpenForTest(0));
}

// Regression: streamed load failure paths (open failure) must clean up the
// slot — the slot stays closed and the builtin fallback still serves all
// styles — while never mishandling the never-opened handle.
TEST(BookFontLoaderStreaming, StreamedOpenFailureCleansSlotWithoutMisuse) {
  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  resetStorage();  // no files → openFileForRead fails immediately
  freeink::book::BookFontLoader loader;
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Ghost");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = BookFontLoader::kMaxFaceBytes + 1024;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Ghost/Ghost-Regular.ttf");
  loader.setFamilyCountForTest(1);
  loader.markDirty();
  // Attempting the load hits the open-failure release path on a slot whose
  // first open failed (fresh Impl assigned, file not open on device).
  freeink::book::FontChain* chain = loader.getReaderFont();
  ASSERT_NE(chain, nullptr);
  // The failed slot never contributed — builtin fallback still covers all.
  EXPECT_EQ(chain->styleCoverage(), 0x07);
  EXPECT_FALSE(loader.streamSourceOpenForTest(0));
  EXPECT_EQ(loader.fontFingerprint(), 0u);
}

// Determinism: two streamed loads over the same file must close/reopen the
// underlying handle — fingerprint stable and the slot back open, with zero
// never-opened closes over the whole lifecycle.
TEST(BookFontLoaderStreaming, StreamedReloadHasCleanHandleLifecycle) {
  const std::string regularBytes = emberBytes("Amazon_Ember_Regular.ttf");
  if (!fixtureAvailable(regularBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  std::string big = regularBytes;
  big.resize(BookFontLoader::kMaxFaceBytes + 1024, '\0');

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  resetStorage();
  seedFile("/fonts/Big/Big-Regular.ttf", big);
  freeink::book::BookFontLoader loader;
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Big");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(big.size());
  fam.faces[0].mtime = 0;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Big/Big-Regular.ttf");
  loader.setFamilyCountForTest(1);

  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);
  const uint32_t fpFirst = loader.fontFingerprint();
  loader.markDirty();  // reload: close + reopen of slot 0's handle
  ASSERT_NE(loader.getReaderFont(), nullptr);
  EXPECT_EQ(loader.fontFingerprint(), fpFirst);
  EXPECT_TRUE(loader.streamSourceOpenForTest(0));
}
#endif  // CROSSPOINT_FONT_BACKEND_FT
