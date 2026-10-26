#include <gtest/gtest.h>
#include <hal/TzDetectParse.h>

#include <string>

// Host-tested pure parsers + validator for the IP-geolocation time-zone
// detect. Fixture JSON matches the live endpoints as served on 2026-10-26
// (see the design doc addendum in docs/design/2026-10-26-x3-wifi-connect-abort.md).

namespace {

freeink::TimeZoneInfo parse(bool (*fn)(const std::string&, freeink::TimeZoneInfo&), const std::string& body) {
  freeink::TimeZoneInfo out;
  fn(body, out);
  return out;
}

// ── IANA id validation (tamper mitigation) ────────────────────────────────

TEST(TzDetectParseTest, AcceptsTypicalZoneIds) {
  EXPECT_TRUE(tzdetect::isValidIanaZoneId("America/Toronto"));
  EXPECT_TRUE(tzdetect::isValidIanaZoneId("UTC"));
  EXPECT_TRUE(tzdetect::isValidIanaZoneId("Etc/GMT+5"));
  EXPECT_TRUE(tzdetect::isValidIanaZoneId("Europe/Paris"));
  EXPECT_TRUE(tzdetect::isValidIanaZoneId("America/Argentina/Buenos_Aires"));
  EXPECT_TRUE(tzdetect::isValidIanaZoneId("Asia/Kolkata"));
}

TEST(TzDetectParseTest, RejectsGarbageAndEmpty) {
  EXPECT_FALSE(tzdetect::isValidIanaZoneId(nullptr));
  EXPECT_FALSE(tzdetect::isValidIanaZoneId(""));
  EXPECT_FALSE(tzdetect::isValidIanaZoneId("not a zone"));
  EXPECT_FALSE(tzdetect::isValidIanaZoneId("America/Toronto; DROP TABLE"));
  EXPECT_FALSE(tzdetect::isValidIanaZoneId("<script>"));
  EXPECT_FALSE(tzdetect::isValidIanaZoneId("../etc/passwd"));
}

TEST(TzDetectParseTest, RejectsBadSlashPlacement) {
  EXPECT_FALSE(tzdetect::isValidIanaZoneId("/America"));
  EXPECT_FALSE(tzdetect::isValidIanaZoneId("America/"));
  EXPECT_FALSE(tzdetect::isValidIanaZoneId("America//Toronto"));
  EXPECT_FALSE(tzdetect::isValidIanaZoneId("America/Toronto/UTC/Europe"));
}

TEST(TzDetectParseTest, EnforcesLengthCap) {
  // 39 characters fit exactly in TimeZoneInfo::id (40 with NUL).
  std::string max38 = "Ab/" + std::string(tzdetect::kMaxIanaIdLen - 3, 'a');
  EXPECT_EQ(max38.size(), tzdetect::kMaxIanaIdLen);
  EXPECT_TRUE(tzdetect::isValidIanaZoneId(max38.c_str()));

  std::string tooLong = max38 + "a";
  EXPECT_EQ(tooLong.size(), tzdetect::kMaxIanaIdLen + 1);
  EXPECT_FALSE(tzdetect::isValidIanaZoneId(tooLong.c_str()));
}

// ── ipwho.is (nested shape) ───────────────────────────────────────────────

TEST(TzDetectParseTest, ParsesIpWhoIsScopedResponse) {
  // Captured live from http://ipwho.is/?fields=success,timezone on 2026-10-26:
  // the scoped response still carries success (the parser requires it).
  const std::string body =
      R"({"success":true,"timezone":{"id":"America/Toronto","abbr":"EDT","is_dst":true,"offset":-14400,"utc":"-04:00"}})";
  const auto out = parse(&tzdetect::parseIpWhoIs, body);
  EXPECT_TRUE(out.valid);
  EXPECT_STREQ(out.id, "America/Toronto");
  EXPECT_EQ(out.offsetMin, -240);  // -14400 s / 60
  EXPECT_TRUE(out.isDst);
}

TEST(TzDetectParseTest, ParsesIpWhoIsFullResponse) {
  // The unscoped (full) response carries success — the parser serves both.
  const std::string body =
      R"({"ip":"1.2.3.4","success":true,"type":"IPv4","timezone":{"id":"Europe/Berlin","abbr":"CEST","is_dst":true,"offset":7200,"utc":"+02:00"}})";
  const auto out = parse(&tzdetect::parseIpWhoIs, body);
  EXPECT_TRUE(out.valid);
  EXPECT_STREQ(out.id, "Europe/Berlin");
  EXPECT_EQ(out.offsetMin, 120);
  EXPECT_TRUE(out.isDst);
}

TEST(TzDetectParseTest, RejectsIpWhoIsFailureAndGarbage) {
  EXPECT_FALSE(parse(&tzdetect::parseIpWhoIs, R"({"success":false,"message":"...")").valid);
  EXPECT_FALSE(parse(&tzdetect::parseIpWhoIs, "not json").valid);
  EXPECT_FALSE(parse(&tzdetect::parseIpWhoIs, R"({"timezone":{"id":""}})").valid);
  EXPECT_FALSE(parse(&tzdetect::parseIpWhoIs, R"({"success":true,"timezone":{}})").valid);
}

// ── ip-api (flat shape) ───────────────────────────────────────────────────

TEST(TzDetectParseTest, ParsesIpApiFlatResponse) {
  // Captured live from http://ip-api.com/json/?fields=timezone (30 bytes).
  const auto out = parse(&tzdetect::parseIpApi, R"({"timezone":"America/Toronto"})");
  EXPECT_TRUE(out.valid);
  EXPECT_STREQ(out.id, "America/Toronto");
  // No offset/DST in the response: the AceTime zone resolution computes the
  // live offset from the id, so the cached values must stay neutral.
  EXPECT_EQ(out.offsetMin, 0);
  EXPECT_FALSE(out.isDst);
}

TEST(TzDetectParseTest, RejectsIpApiGarbageAndMissing) {
  EXPECT_FALSE(parse(&tzdetect::parseIpApi, "{}").valid);
  EXPECT_FALSE(parse(&tzdetect::parseIpApi, R"({"timezone":""})").valid);
  EXPECT_FALSE(parse(&tzdetect::parseIpApi, R"({"timezone":123})").valid);
  EXPECT_FALSE(parse(&tzdetect::parseIpApi, "not json").valid);
}

}  // namespace
