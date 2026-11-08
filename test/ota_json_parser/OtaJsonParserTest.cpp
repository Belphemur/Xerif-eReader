#include <gtest/gtest.h>

#include <cstring>
#include <string_view>

#include "OtaJson.h"

namespace {

const char* kRealisticPretty = R"({
  "url": "https://api.github.com/repos/Belphemur/Xerif-eReader/releases/12345",
  "assets_url": "https://api.github.com/repos/Belphemur/Xerif-eReader/releases/12345/assets",
  "upload_url": "https://uploads.github.com/repos/Belphemur/Xerif-eReader/releases/12345/assets{?name,label}",
  "html_url": "https://github.com/Belphemur/Xerif-eReader/releases/tag/v2.4.1",
  "id": 12345,
  "author": {
    "login": "releasebot",
    "id": 99887766,
    "node_id": "MDQ6VXNlcjk5ODg3NzY2",
    "avatar_url": "https://avatars.githubusercontent.com/u/99887766?v=4",
    "url": "https://api.github.com/users/releasebot",
    "type": "User",
    "site_admin": false
  },
  "node_id": "RE_kwDOAbCdEf4AADBN",
  "tag_name": "v2.4.1",
  "target_commitish": "main",
  "name": "Xerif v2.4.1",
  "draft": false,
  "prerelease": false,
  "created_at": "2026-04-28T10:00:00Z",
  "published_at": "2026-04-28T10:30:00Z",
  "assets": [
    {
      "url": "https://api.github.com/repos/Belphemur/Xerif-eReader/releases/assets/100001",
      "id": 100001,
      "node_id": "RA_kwDOAbCdEf4AAGHR",
      "name": "Xerif-v2.4.1-source.zip",
      "label": null,
      "uploader": {
        "login": "releasebot",
        "id": 99887766,
        "node_id": "MDQ6VXNlcjk5ODg3NzY2",
        "type": "User"
      },
      "content_type": "application/zip",
      "state": "uploaded",
      "size": 2048576,
      "download_count": 42,
      "created_at": "2026-04-28T10:15:00Z",
      "updated_at": "2026-04-28T10:15:30Z",
      "browser_download_url": "https://github.com/Belphemur/Xerif-eReader/releases/download/v2.4.1/Xerif-v2.4.1-source.zip"
    },
    {
      "url": "https://api.github.com/repos/Belphemur/Xerif-eReader/releases/assets/100002",
      "id": 100002,
      "node_id": "RA_kwDOAbCdEf4AAGHS",
      "name": "firmware.bin",
      "label": "ESP32-C3 Firmware",
      "uploader": {
        "login": "releasebot",
        "id": 99887766,
        "node_id": "MDQ6VXNlcjk5ODg3NzY2",
        "type": "User"
      },
      "content_type": "application/octet-stream",
      "state": "uploaded",
      "size": 1572864,
      "download_count": 187,
      "created_at": "2026-04-28T10:16:00Z",
      "updated_at": "2026-04-28T10:16:45Z",
      "browser_download_url": "https://github.com/Belphemur/Xerif-eReader/releases/download/v2.4.1/firmware.bin"
    },
    {
      "url": "https://api.github.com/repos/Belphemur/Xerif-eReader/releases/assets/100003",
      "id": 100003,
      "node_id": "RA_kwDOAbCdEf4AAGHR",
      "name": "checksums.sha256",
      "label": null,
      "uploader": {
        "login": "releasebot",
        "id": 99887766,
        "node_id": "MDQ6VXNlcjk5ODg3NzY2",
        "type": "User"
      },
      "content_type": "text/plain",
      "state": "uploaded",
      "size": 192,
      "download_count": 15,
      "created_at": "2026-04-28T10:17:00Z",
      "updated_at": "2026-04-28T10:17:10Z",
      "browser_download_url": "https://github.com/Belphemur/Xerif-eReader/releases/download/v2.4.1/checksums.sha256"
    }
  ],
  "tarball_url": "https://api.github.com/repos/Belphemur/Xerif-eReader/tarball/v2.4.1",
  "zipball_url": "https://api.github.com/repos/Belphemur/Xerif-eReader/zipball/v2.4.1",
  "body": "## What's Changed\n\n* Fixed orientation crash (#123)\n* Improved EPUB rendering performance\n* Added Serbian translation\n\n**Full Changelog**: https://github.com/Belphemur/Xerif-eReader/compare/v2.4.0...v2.4.1",
  "reactions": {
    "url": "https://api.github.com/repos/Belphemur/Xerif-eReader/releases/12345/reactions",
    "total_count": 5,
    "+1": 3,
    "-1": 0,
    "laugh": 1,
    "hooray": 1,
    "confused": 0,
    "heart": 0,
    "rocket": 0,
    "eyes": 0
  }
})";

const char* kRealisticMinified =
    R"({"url":"https://api.github.com/repos/Belphemur/Xerif-eReader/releases/12345","assets_url":"https://api.github.com/repos/Belphemur/Xerif-eReader/releases/12345/assets","id":12345,"author":{"login":"releasebot","id":99887766,"node_id":"MDQ6VXNlcjk5ODg3NzY2","type":"User","site_admin":false},"tag_name":"v2.4.1","target_commitish":"main","name":"Xerif v2.4.1","draft":false,"prerelease":false,"assets":[{"url":"https://api.github.com/repos/Belphemur/Xerif-eReader/releases/assets/100001","id":100001,"name":"Xerif-v2.4.1-source.zip","uploader":{"login":"releasebot","id":99887766},"content_type":"application/zip","state":"uploaded","size":2048576,"download_count":42,"browser_download_url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v2.4.1/Xerif-v2.4.1-source.zip"},{"url":"https://api.github.com/repos/Belphemur/Xerif-eReader/releases/assets/100002","id":100002,"name":"firmware.bin","uploader":{"login":"releasebot","id":99887766},"content_type":"application/octet-stream","state":"uploaded","size":1572864,"download_count":187,"browser_download_url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v2.4.1/firmware.bin"},{"url":"https://api.github.com/repos/Belphemur/Xerif-eReader/releases/assets/100003","id":100003,"name":"checksums.sha256","uploader":{"login":"releasebot","id":99887766},"content_type":"text/plain","state":"uploaded","size":192,"download_count":15,"browser_download_url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v2.4.1/checksums.sha256"}],"body":"## What's Changed\n\n* Fixed orientation crash","reactions":{"url":"https://api.github.com/repos/Belphemur/Xerif-eReader/releases/12345/reactions","total_count":5,"+1":3}})";

// Release body carrying a signed manifest alongside the firmware asset.
const char* kReleaseWithManifest =
    R"({"tag_name":"v1.9.0","name":"Xerif v1.9.0","draft":false,"prerelease":false,)"
    R"("assets":[)"
    R"({"name":"firmware-x4pro.bin","content_type":"application/octet-stream","size":5435936,)"
    R"("browser_download_url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.9.0/firmware-x4pro.bin"},)"
    R"({"name":"manifest.json","content_type":"application/octet-stream","size":1101,)"
    R"("browser_download_url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.9.0/manifest.json"},)"
    R"({"name":"manifest.json.sig","content_type":"application/octet-stream","size":96,)"
    R"("browser_download_url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.9.0/manifest.json.sig"}]})";

// Signed manifest shape used by the release workflow (v1.9.0): a bare-semver
// version plus one boards[] entry per board with the exact asset URL, byte
// size, and 64-hex SHA-256.
const char* kManifestV190 = R"({
  "version": "1.9.0",
  "boards": [
    {
      "board": "papermono",
      "sha256": "1111111122222222333333334444444455555555666666667777777788888888",
      "size": 5401231,
      "url": "https://github.com/Belphemur/Xerif-eReader/releases/download/v1.9.0/firmware-papermono.bin"
    },
    {
      "board": "sticky",
      "sha256": "99990000111122223333444455556666777788889999aaaabbbbccccddddeeee",
      "size": 5420096,
      "url": "https://github.com/Belphemur/Xerif-eReader/releases/download/v1.9.0/firmware-sticky.bin"
    },
    {
      "board": "x4pro",
      "sha256": "a5f3c9d2e1b8074a6f2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f607182",
      "size": 5435936,
      "url": "https://github.com/Belphemur/Xerif-eReader/releases/download/v1.9.0/firmware-x4pro.bin"
    },
    {
      "board": "x4",
      "sha256": "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
      "size": 5390000,
      "url": "https://github.com/Belphemur/Xerif-eReader/releases/download/v1.9.0/firmware-x4.bin"
    }
  ]
})";

// Dual-family release manifest (xpoint->xerif transition): five xpoint entries
// first for pre-rename parsers, followed by the five xerif entries.
static constexpr char kManifestDualFamily[] = R"({
  "version": "1.16.0",
  "boards": [
    {"board":"x4","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xpoint-1.16.0-x3-x4.bin","size":5390001,"sha256":"1111111122222222333333334444444455555555666666667777777788888888"},
    {"board":"sticky","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xpoint-1.16.0-sticky.bin","size":5420097,"sha256":"99990000111122223333444455556666777788889999aaaabbbbccccddddeeee"},
    {"board":"x4pro","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xpoint-1.16.0-x4pro.bin","size":5435937,"sha256":"a5f3c9d2e1b8074a6f2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f607182"},
    {"board":"papermono","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xpoint-1.16.0-papermono.bin","size":5401232,"sha256":"2222222233333333444444445555555566666666777777778888888899999999"},
    {"board":"x4c","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xpoint-1.16.0-x4c.bin","size":5410000,"sha256":"33333333444444445555555566666666777777778888888899999999aaaaaaab"},
    {"board":"x4","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xerif-1.16.0-x3-x4.bin","size":5390001,"sha256":"1111111122222222333333334444444455555555666666667777777788888888"},
    {"board":"sticky","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xerif-1.16.0-sticky.bin","size":5420097,"sha256":"99990000111122223333444455556666777788889999aaaabbbbccccddddeeee"},
    {"board":"x4pro","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xerif-1.16.0-x4pro.bin","size":5435937,"sha256":"a5f3c9d2e1b8074a6f2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f607182"},
    {"board":"papermono","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xerif-1.16.0-papermono.bin","size":5401232,"sha256":"2222222233333333444444445555555566666666777777778888888899999999"},
    {"board":"x4c","url":"https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xerif-1.16.0-x4c.bin","size":5410000,"sha256":"33333333444444445555555566666666777777778888888899999999aaaaaaab"}
  ]
})";

// OtaReleaseInfo (~1KB) and ManifestBoardEntry (~580B each) blow the
// 256-byte stack-safety budget, so test instances live in static storage
// and are zeroed before each use.
OtaReleaseInfo g_info;
OtaReleaseInfo g_infoAlt;
ManifestBoardEntry g_entries[OTA_MANIFEST_MAX_BOARDS];

OtaReleaseInfo& makeReleaseInfo() {
  g_info = {};
  return g_info;
}

OtaReleaseInfo& makeReleaseInfoAlt() {
  g_infoAlt = {};
  return g_infoAlt;
}

ManifestBoardEntry* makeEntries() {
  memset(g_entries, 0, sizeof(g_entries));
  return g_entries;
}

}  // namespace

TEST(OtaReleaseParse, RealisticPrettyPrinted) {
  OtaReleaseInfo& info = makeReleaseInfo();
  parseOtaRelease(kRealisticPretty, strlen(kRealisticPretty), "firmware.bin", "manifest.json", info);

  EXPECT_TRUE(info.hasTag);
  EXPECT_STREQ(info.tagName, "v2.4.1");
  EXPECT_TRUE(info.hasFirmware);
  EXPECT_STREQ(info.firmwareUrl, "https://github.com/Belphemur/Xerif-eReader/releases/download/v2.4.1/firmware.bin");
  EXPECT_EQ(info.firmwareSize, 1572864u);
  EXPECT_FALSE(info.hasManifest);
  EXPECT_STREQ(info.manifestUrl, "");
}

TEST(OtaReleaseParse, RealisticMinified) {
  OtaReleaseInfo& info = makeReleaseInfo();
  parseOtaRelease(kRealisticMinified, strlen(kRealisticMinified), "firmware.bin", "manifest.json", info);

  EXPECT_TRUE(info.hasTag);
  EXPECT_STREQ(info.tagName, "v2.4.1");
  EXPECT_TRUE(info.hasFirmware);
  EXPECT_STREQ(info.firmwareUrl, "https://github.com/Belphemur/Xerif-eReader/releases/download/v2.4.1/firmware.bin");
  EXPECT_EQ(info.firmwareSize, 1572864u);
  EXPECT_FALSE(info.hasManifest);
}

TEST(OtaReleaseParse, PrettyAndMinifiedAgree) {
  OtaReleaseInfo& pretty = makeReleaseInfo();
  parseOtaRelease(kRealisticPretty, strlen(kRealisticPretty), "firmware.bin", "manifest.json", pretty);

  OtaReleaseInfo& minified = makeReleaseInfoAlt();
  parseOtaRelease(kRealisticMinified, strlen(kRealisticMinified), "firmware.bin", "manifest.json", minified);

  EXPECT_STREQ(pretty.tagName, minified.tagName);
  EXPECT_STREQ(pretty.firmwareUrl, minified.firmwareUrl);
  EXPECT_EQ(pretty.firmwareSize, minified.firmwareSize);
}

TEST(OtaReleaseParse, MissingTagName) {
  const char* json = R"({
      "name": "Some Release",
      "draft": false,
      "assets": [{
        "name": "firmware.bin",
        "browser_download_url": "https://example.com/fw.bin",
        "size": 1000
      }]
    })";

  OtaReleaseInfo& info = makeReleaseInfo();
  parseOtaRelease(json, strlen(json), "firmware.bin", "manifest.json", info);

  EXPECT_FALSE(info.hasTag);
  EXPECT_STREQ(info.tagName, "");
  EXPECT_TRUE(info.hasFirmware);
}

TEST(OtaReleaseParse, MissingFirmwareAsset) {
  const char* json = R"({
      "tag_name": "v1.0.0",
      "assets": [
        {"name": "source.zip", "browser_download_url": "https://example.com/src.zip", "size": 1000},
        {"name": "docs.tar.gz", "browser_download_url": "https://example.com/docs.tar.gz", "size": 2000}
      ]
    })";

  OtaReleaseInfo& info = makeReleaseInfo();
  parseOtaRelease(json, strlen(json), "firmware.bin", "manifest.json", info);

  EXPECT_TRUE(info.hasTag);
  EXPECT_FALSE(info.hasFirmware);
  EXPECT_STREQ(info.firmwareUrl, "");
  EXPECT_EQ(info.firmwareSize, 0u);
}

TEST(OtaReleaseParse, MissingManifestAsset) {
  OtaReleaseInfo& info = makeReleaseInfo();
  parseOtaRelease(kRealisticPretty, strlen(kRealisticPretty), "firmware.bin", "manifest.json", info);

  EXPECT_TRUE(info.hasFirmware);
  EXPECT_FALSE(info.hasManifest);
  EXPECT_STREQ(info.manifestUrl, "");
}

TEST(OtaReleaseParse, ManifestAssetCaptured) {
  OtaReleaseInfo& info = makeReleaseInfo();
  parseOtaRelease(kReleaseWithManifest, strlen(kReleaseWithManifest), "firmware-x4pro.bin", "manifest.json", info);

  EXPECT_TRUE(info.hasTag);
  EXPECT_STREQ(info.tagName, "v1.9.0");
  EXPECT_TRUE(info.hasFirmware);
  EXPECT_EQ(info.firmwareSize, 5435936u);
  EXPECT_TRUE(info.hasManifest);
  EXPECT_STREQ(info.manifestUrl, "https://github.com/Belphemur/Xerif-eReader/releases/download/v1.9.0/manifest.json");
}

TEST(OtaReleaseParse, ExtraUnknownKeysIgnored) {
  const char* json = R"({
      "node_id": "RE_kwDOAbCdEf4AADBN",
      "discussion_url": "https://github.com/Belphemur/Xerif-eReader/discussions/99",
      "author": {"login": "dev", "id": 1, "nested": {"deep": true}},
      "tag_name": "v6.0",
      "reactions": {"url": "https://reactions", "total_count": 0, "+1": 0},
      "labels": ["release", "stable"],
      "assets": [{
        "name": "firmware.bin",
        "uploader": {"login": "bot", "permissions": {"admin": false, "push": true}},
        "browser_download_url": "https://fw6",
        "size": 1111
      }],
      "mentions_count": 3
    })";

  OtaReleaseInfo& info = makeReleaseInfo();
  parseOtaRelease(json, strlen(json), "firmware.bin", "manifest.json", info);

  EXPECT_TRUE(info.hasTag);
  EXPECT_STREQ(info.tagName, "v6.0");
  EXPECT_TRUE(info.hasFirmware);
  EXPECT_STREQ(info.firmwareUrl, "https://fw6");
  EXPECT_EQ(info.firmwareSize, 1111u);
}

TEST(OtaReleaseParse, AssetsKeyOrderIndependent) {
  // browser_download_url and size appearing before name must not matter.
  const char* json = R"({
      "tag_name": "v3.1",
      "assets": [{
        "size": 3333,
        "browser_download_url": "https://example.com/fw2.bin",
        "name": "firmware.bin"
      }]
    })";

  OtaReleaseInfo& info = makeReleaseInfo();
  parseOtaRelease(json, strlen(json), "firmware.bin", "manifest.json", info);

  EXPECT_TRUE(info.hasFirmware);
  EXPECT_STREQ(info.firmwareUrl, "https://example.com/fw2.bin");
  EXPECT_EQ(info.firmwareSize, 3333u);
}

TEST(OtaReleaseParse, AssetNameExactMatchOnly) {
  const char* json = R"({
      "tag_name": "v1.0",
      "assets": [
        {"name": "firmware.bin.bak", "browser_download_url": "https://bak", "size": 100},
        {"name": "FIRMWARE.BIN", "browser_download_url": "https://upper", "size": 200},
        {"name": "firmware.bin2", "browser_download_url": "https://suffix", "size": 300}
      ]
    })";

  OtaReleaseInfo& info = makeReleaseInfo();
  parseOtaRelease(json, strlen(json), "firmware.bin", "manifest.json", info);

  EXPECT_FALSE(info.hasFirmware);
}

TEST(OtaReleaseParse, MalformedJsonLeavesInfoZeroed) {
  OtaReleaseInfo& info = makeReleaseInfo();
  info.hasTag = true;
  info.hasFirmware = true;

  parseOtaRelease("this is not json", 16, "firmware.bin", "manifest.json", info);

  EXPECT_FALSE(info.hasTag);
  EXPECT_FALSE(info.hasFirmware);
  EXPECT_FALSE(info.hasManifest);
  EXPECT_EQ(info.firmwareSize, 0u);
}

TEST(OtaManifestParse, RealV190Manifest) {
  ManifestBoardEntry* entries = makeEntries();
  int count = -1;
  char version[32] = {0};

  ASSERT_TRUE(parseOtaManifest(kManifestV190, strlen(kManifestV190), entries, OTA_MANIFEST_MAX_BOARDS, &count, version,
                               sizeof(version)));

  EXPECT_EQ(count, 4);
  EXPECT_STREQ(version, "1.9.0");

  const ManifestBoardEntry* x4pro = findBoardEntry(entries, count, "x4pro", 5);
  ASSERT_NE(x4pro, nullptr);
  EXPECT_STREQ(x4pro->url, "https://github.com/Belphemur/Xerif-eReader/releases/download/v1.9.0/firmware-x4pro.bin");
  EXPECT_EQ(x4pro->size, 5435936u);
  EXPECT_TRUE(x4pro->hasSha);
  static constexpr uint8_t kExpectedSha[32] = {0xa5, 0xf3, 0xc9, 0xd2, 0xe1, 0xb8, 0x07, 0x4a, 0x6f, 0x2c, 0x3d,
                                               0x4e, 0x5f, 0x60, 0x71, 0x82, 0x93, 0xa4, 0xb5, 0xc6, 0xd7, 0xe8,
                                               0xf9, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x60, 0x71, 0x82};
  EXPECT_EQ(memcmp(x4pro->sha256, kExpectedSha, 32), 0);

  for (const char* board : {"papermono", "sticky", "x4", "x4pro"}) {
    EXPECT_NE(findBoardEntry(entries, count, board, strlen(board)), nullptr) << board;
  }
}

TEST(OtaManifestParse, MissingBoardReturnsNull) {
  ManifestBoardEntry* entries = makeEntries();
  int count = 0;
  char version[32] = {0};

  ASSERT_TRUE(parseOtaManifest(kManifestV190, strlen(kManifestV190), entries, OTA_MANIFEST_MAX_BOARDS, &count, version,
                               sizeof(version)));

  EXPECT_EQ(findBoardEntry(entries, count, "x9", 2), nullptr);
  EXPECT_EQ(findBoardEntry(entries, count, "x4pro-plus", 9), nullptr);
  EXPECT_EQ(findBoardEntry(entries, count, "x4pr", 4), nullptr);
}

TEST(OtaManifestParse, InvalidHexShaMeansNoSha) {
  const char* json = R"({
      "version": "1.0.0",
      "boards": [
        {"board": "x4pro", "sha256": "zz93a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f607182", "size": 1,
         "url": "https://example.com/fw.bin"},
        {"board": "x4", "sha256": "a5f3c9d2", "size": 2, "url": "https://example.com/fw2.bin"}
      ]
    })";

  ManifestBoardEntry* entries = makeEntries();
  int count = 0;
  char version[32] = {0};

  ASSERT_TRUE(parseOtaManifest(json, strlen(json), entries, OTA_MANIFEST_MAX_BOARDS, &count, version, sizeof(version)));

  ASSERT_EQ(count, 2);
  EXPECT_FALSE(findBoardEntry(entries, count, "x4pro", 5)->hasSha);
  EXPECT_FALSE(findBoardEntry(entries, count, "x4", 2)->hasSha);
}

TEST(OtaManifestParse, DualFamilyManifestParsesAllEntries) {
  ManifestBoardEntry* entries = makeEntries();
  int count = -1;
  char version[32] = {0};

  ASSERT_TRUE(parseOtaManifest(kManifestDualFamily, strlen(kManifestDualFamily), entries, OTA_MANIFEST_MAX_BOARDS,
                               &count, version, sizeof(version)));

  EXPECT_EQ(count, 10);
  EXPECT_STREQ(version, "1.16.0");
  const ManifestBoardEntry* x4 = findBoardEntry(entries, count, "x4", 2);
  ASSERT_NE(x4, nullptr);
  EXPECT_STREQ(x4->url, "https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xpoint-1.16.0-x3-x4.bin");
}

TEST(OtaManifestParse, BoardEntryPrefersSelectedAssetUrl) {
  ManifestBoardEntry* entries = makeEntries();
  int count = -1;
  char version[32] = {0};

  ASSERT_TRUE(parseOtaManifest(kManifestDualFamily, strlen(kManifestDualFamily), entries, OTA_MANIFEST_MAX_BOARDS,
                               &count, version, sizeof(version)));

  const char* x4proUrl = "https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xerif-1.16.0-x4pro.bin";
  const ManifestBoardEntry* x4pro = findBoardEntryForUrl(entries, count, "x4pro", 5, x4proUrl);
  ASSERT_NE(x4pro, nullptr);
  EXPECT_STREQ(x4pro->url, x4proUrl);

  // A URL with no exact manifest entry falls back to the first board match.
  const ManifestBoardEntry* x4 = findBoardEntryForUrl(entries, count, "x4", 2, "https://example.com/other.bin");
  ASSERT_NE(x4, nullptr);
  EXPECT_STREQ(x4->url, "https://github.com/Belphemur/Xerif-eReader/releases/download/v1.16.0/xpoint-1.16.0-x3-x4.bin");
}

TEST(OtaManifestParse, MaxEntriesCapsParsedBoards) {
  ManifestBoardEntry* entries = makeEntries();
  int count = -1;
  char version[32] = {0};

  ASSERT_TRUE(parseOtaManifest(kManifestV190, strlen(kManifestV190), entries, 2, &count, version, sizeof(version)));

  EXPECT_EQ(count, 2);
  EXPECT_NE(findBoardEntry(entries, count, "papermono", 9), nullptr);
  EXPECT_NE(findBoardEntry(entries, count, "sticky", 6), nullptr);
  EXPECT_EQ(findBoardEntry(entries, count, "x4pro", 5), nullptr);
}

TEST(OtaManifestParse, MalformedJsonFailsClosed) {
  ManifestBoardEntry* entries = makeEntries();
  int count = -1;
  char version[32] = {0};

  EXPECT_FALSE(parseOtaManifest("not json", 8, entries, OTA_MANIFEST_MAX_BOARDS, &count, version, sizeof(version)));
  EXPECT_EQ(count, 0);
  EXPECT_STREQ(version, "");
}

TEST(OtaManifestParse, NullRequiredArgsRejected) {
  ManifestBoardEntry* entries = makeEntries();
  int count = -1;
  char version[32] = {0};

  EXPECT_FALSE(parseOtaManifest(nullptr, 0, entries, OTA_MANIFEST_MAX_BOARDS, &count, version, sizeof(version)));
  EXPECT_FALSE(parseOtaManifest("{}", 2, nullptr, OTA_MANIFEST_MAX_BOARDS, &count, version, sizeof(version)));
  EXPECT_FALSE(parseOtaManifest("{}", 2, entries, 0, &count, version, sizeof(version)));
  EXPECT_FALSE(parseOtaManifest("{}", 2, entries, OTA_MANIFEST_MAX_BOARDS, nullptr, version, sizeof(version)));
  EXPECT_EQ(count, 0);
}

TEST(OtaManifestParse, NullVersionBufferIsSkipped) {
  ManifestBoardEntry* entries = makeEntries();
  int count = -1;

  ASSERT_TRUE(parseOtaManifest(kManifestV190, strlen(kManifestV190), entries, OTA_MANIFEST_MAX_BOARDS, &count,
                               /*version=*/nullptr, /*versionSize=*/16));

  EXPECT_EQ(count, 4);
}

TEST(OtaManifestParse, NullVersionBufferWithZeroSizeIsSkipped) {
  ManifestBoardEntry* entries = makeEntries();
  int count = -1;

  ASSERT_TRUE(parseOtaManifest(kManifestV190, strlen(kManifestV190), entries, OTA_MANIFEST_MAX_BOARDS, &count,
                               /*version=*/nullptr, /*versionSize=*/0));

  EXPECT_EQ(count, 4);
}

TEST(OtaManifestParse, ZeroSizeVersionBufferIsNotWritten) {
  ManifestBoardEntry* entries = makeEntries();
  int count = -1;
  char version[8];
  char untouched[8];
  memset(version, 0xAB, sizeof(version));
  memset(untouched, 0xAB, sizeof(untouched));

  ASSERT_TRUE(
      parseOtaManifest(kManifestV190, strlen(kManifestV190), entries, OTA_MANIFEST_MAX_BOARDS, &count, version, 0));

  EXPECT_EQ(count, 4);
  // Nothing written past the zero-size capacity.
  EXPECT_EQ(memcmp(version, untouched, sizeof(version)), 0);
}

TEST(OtaVersionCompare, SameVersionWithBoardSuffixIsNotNewer) {
  EXPECT_FALSE(otaIsVersionNewer("1.9.0-x4pro", "v1.9.0"));
}

TEST(OtaVersionCompare, NewerReleaseIsDetected) {
  EXPECT_TRUE(otaIsVersionNewer("1.8.0-x4pro", "v1.9.0"));
  EXPECT_TRUE(otaIsVersionNewer("1.8.0", "v1.9.0"));
  EXPECT_TRUE(otaIsVersionNewer("1.9.0-x4pro", "v1.9.1"));
  EXPECT_TRUE(otaIsVersionNewer("1.9.0-x4pro", "v2.0.0"));
}

TEST(OtaVersionCompare, OlderOrEqualReleaseIsNotNewer) {
  EXPECT_FALSE(otaIsVersionNewer("1.9.0-x4pro", "v1.8.0"));
  EXPECT_FALSE(otaIsVersionNewer("2.0.0-x4pro", "v1.9.9"));
  EXPECT_FALSE(otaIsVersionNewer("1.9.0", "v1.9.0"));
}

TEST(OtaVersionCompare, RcBuildTreatsSameVersionStableAsNewer) {
  EXPECT_TRUE(otaIsVersionNewer("1.9.0-rc1", "v1.9.0"));
  EXPECT_TRUE(otaIsVersionNewer("1.9.0-rc1", "v1.9.1"));
}

TEST(OtaVersionCompare, MalformedTagsFailClosed) {
  EXPECT_FALSE(otaIsVersionNewer("1.9.0-x4pro", "not-a-version"));
  EXPECT_FALSE(otaIsVersionNewer("", "v1.9.0"));
  EXPECT_FALSE(otaIsVersionNewer("1.9.0-x4pro", ""));
}

// --- dev channel manifest (design §4) ---------------------------------------

namespace {
constexpr char kDevManifest[] = R"({
  "versions": [
    {"name": "xerif-dev-0123456789abcdef0123456789abcdef01234567-x4pro.bin",
     "url": "https://github.com/Belphemur/Xerif-eReader/releases/download/dev/xerif-dev-0123456789abcdef0123456789abcdef01234567-x4pro.bin",
     "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "date": "2026-11-07T12:00:00Z"},
    {"name": "xerif-dev-ffffffffffffffffffffffffffffffffffffffff-x4pro.bin",
     "url": "https://github.com/Belphemur/Xerif-eReader/releases/download/dev/xerif-dev-ffffffffffffffffffffffffffffffffffffffff-x4pro.bin",
     "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
     "date": "2026-11-06T12:00:00Z"}
  ]
})";
}  // namespace

TEST(OtaDevManifestParse, ParsesEntriesNewestFirst) {
  OtaDevVersionEntry entries[3];
  int count = -1;

  ASSERT_TRUE(parseOtaDevManifest(kDevManifest, strlen(kDevManifest), entries, 3, &count));
  ASSERT_EQ(count, 2);
  EXPECT_STREQ(entries[0].name, "xerif-dev-0123456789abcdef0123456789abcdef01234567-x4pro.bin");
  EXPECT_EQ(entries[0].url, std::string_view("https://github.com/Belphemur/Xerif-eReader/releases/download/dev/"
                                             "xerif-dev-0123456789abcdef0123456789abcdef01234567-x4pro.bin"));
  EXPECT_STREQ(entries[0].date, "2026-11-07T12:00:00Z");
  EXPECT_TRUE(entries[0].hasSha);
  // First digest byte decodes from "e3" (e3b0c4... is sha256 of "").
  EXPECT_EQ(entries[0].sha256[0], 0xE3);
  EXPECT_EQ(entries[0].sha256[31], 0x55);
}

TEST(OtaDevManifestParse, MalformedHexDigestSetsHasShaFalse) {
  constexpr char kBadSha[] =
      R"({"versions": [{"name": "a.bin", "url": "https://x/a.bin", "sha256": "nothex", "date": "d"}]})";
  OtaDevVersionEntry entries[3];
  int count = -1;

  ASSERT_TRUE(parseOtaDevManifest(kBadSha, strlen(kBadSha), entries, 3, &count));
  ASSERT_EQ(count, 1);
  EXPECT_FALSE(entries[0].hasSha);
}

TEST(OtaDevManifestParse, OverflowTruncatesToCapacity) {
  OtaDevVersionEntry entries[1];
  int count = -1;

  ASSERT_TRUE(parseOtaDevManifest(kDevManifest, strlen(kDevManifest), entries, 1, &count));
  EXPECT_EQ(count, 1);
  EXPECT_STREQ(entries[0].name, "xerif-dev-0123456789abcdef0123456789abcdef01234567-x4pro.bin");
}

TEST(OtaDevManifestParse, MalformedJsonFailsClosed) {
  OtaDevVersionEntry entries[3];
  int count = -1;

  EXPECT_FALSE(parseOtaDevManifest("not json", 8, entries, 3, &count));
  EXPECT_EQ(count, 0);
  EXPECT_FALSE(parseOtaDevManifest(nullptr, 0, entries, 3, &count));
  EXPECT_FALSE(parseOtaDevManifest("{}", 2, entries, 3, nullptr));
}

TEST(OtaDevManifestParse, MissingFieldsLeaveEmptyStrings) {
  constexpr char kSparse[] = R"({"versions": [{"name": "a.bin"}]})";
  OtaDevVersionEntry entries[3];
  int count = -1;

  ASSERT_TRUE(parseOtaDevManifest(kSparse, strlen(kSparse), entries, 3, &count));
  ASSERT_EQ(count, 1);
  EXPECT_STREQ(entries[0].name, "a.bin");
  EXPECT_EQ(entries[0].url[0], '\0');
  EXPECT_EQ(entries[0].date[0], '\0');
  EXPECT_FALSE(entries[0].hasSha);
}

TEST(OtaDevManifestParse, VersionFieldParsedWhenPresent) {
  constexpr char kVersioned[] =
      R"({"versions": [{"name": "xerif-dev-0123456789abcdef0123456789abcdef01234567-x4pro.bin", "url": "https://x/a.bin", "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "date": "2026-11-07T12:00:00Z", "version": "2.14.0-dev-develop-32c5038a4"}]})";
  OtaDevVersionEntry entries[3];
  int count = -1;

  ASSERT_TRUE(parseOtaDevManifest(kVersioned, strlen(kVersioned), entries, 3, &count));
  ASSERT_EQ(count, 1);
  EXPECT_STREQ(entries[0].version, "2.14.0-dev-develop-32c5038a4");
}

TEST(OtaDevManifestParse, AbsentVersionFieldLeavesEmptyForNameFallback) {
  // kDevManifest carries no version field (older-manifest shape): the device
  // falls back to the asset name for such rows.
  OtaDevVersionEntry entries[3];
  int count = -1;

  ASSERT_TRUE(parseOtaDevManifest(kDevManifest, strlen(kDevManifest), entries, 3, &count));
  ASSERT_EQ(count, 2);
  EXPECT_EQ(entries[0].version[0], '\0');
  EXPECT_EQ(entries[1].version[0], '\0');
}

TEST(OtaDevManifestParse, OverlongVersionClampsToFieldCapacity) {
  std::string json = R"({"versions": [{"name": "a.bin", "url": "https://x/a.bin", "version": ")";
  json += std::string(60, 'a');  // over version[48]
  json += R"("}]})";
  OtaDevVersionEntry entries[3];
  int count = -1;

  ASSERT_TRUE(parseOtaDevManifest(json.c_str(), json.size(), entries, 3, &count));
  ASSERT_EQ(count, 1);
  EXPECT_EQ(strlen(entries[0].version), sizeof(entries[0].version) - 1);
  for (size_t i = 0; i < sizeof(entries[0].version) - 1; ++i) {
    EXPECT_EQ(entries[0].version[i], 'a');
  }
}
