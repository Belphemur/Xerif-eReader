#include <gtest/gtest.h>

#include "network/OtaInstallRouting.h"

using ota_install_routing::classifySdFile;
using ota_install_routing::InstallRoute;
using ota_install_routing::routeForInstall;
using ota_install_routing::SdFileClass;
using ota_install_routing::targetForSdFile;

// ── §15 SD classification: the CI asset grammar ─────────────────────────────
//
// The CI lanes emit exactly: xerif-dev-<sha>-<device>.bin (dev lane),
// xerif-pr-<num>-<sha>-<device>.bin (PR lane), xerif-<version>-<device>.bin
// (release lane). Dev/pr prefixes are checked FIRST — xerif-dev- also matches
// xerif-.

TEST(OtaInstallRoutingTest, DevPrefixClassifiesDev) {
  EXPECT_EQ(classifySdFile("xerif-dev-abc123-x4pro.bin"), SdFileClass::Dev);
}

TEST(OtaInstallRoutingTest, PrPrefixClassifiesPr) {
  EXPECT_EQ(classifySdFile("xerif-pr-303-abc1234-x4pro.bin"), SdFileClass::Pr);
}

TEST(OtaInstallRoutingTest, DevPrPrefixesCheckedBeforePlainPrefix) {
  // xerif-dev- also matches xerif-: the dev/pr check MUST run first, or the
  // dev asset would classify stable.
  EXPECT_NE(classifySdFile("xerif-dev-abc123-x4pro.bin"), SdFileClass::Stable);
  EXPECT_NE(classifySdFile("xerif-pr-303-abc1234-x4pro.bin"), SdFileClass::Stable);
}

TEST(OtaInstallRoutingTest, ReleaseAssetClassifiesStable) {
  EXPECT_EQ(classifySdFile("xerif-2.11.0-x4pro.bin"), SdFileClass::Stable);
  EXPECT_EQ(classifySdFile("xerif-2.11.0-x3-x4.bin"), SdFileClass::Stable);
}

TEST(OtaInstallRoutingTest, UnknownPrefixClassifiesStable) {
  EXPECT_EQ(classifySdFile("crosspoint-1.6.0-x4pro.bin"), SdFileClass::Stable);
  EXPECT_EQ(classifySdFile("firmware.bin"), SdFileClass::Stable);
  EXPECT_EQ(classifySdFile(""), SdFileClass::Stable);
  EXPECT_EQ(classifySdFile(nullptr), SdFileClass::Stable);
}

TEST(OtaInstallRoutingTest, PrefixIsCaseSensitive) {
  // The CI lanes emit lowercase names; an uppercase variant is not a CI asset
  // and classifies stable (fail-safe direction: stable is the dance target
  // with the stricter worker check, not a silent dev-slot overwrite).
  EXPECT_EQ(classifySdFile("Xerif-dev-abc123-x4pro.bin"), SdFileClass::Stable);
}

TEST(OtaInstallRoutingTest, PrefixMustBeAtNameStart) {
  EXPECT_EQ(classifySdFile("my-xerif-dev-abc123-x4pro.bin"), SdFileClass::Stable);
}

// ── §15 source rule: classification → target slot ───────────────────────────

TEST(OtaInstallRoutingTest, DevAndPrFilesTargetDevSlot) {
  EXPECT_EQ(targetForSdFile(SdFileClass::Dev), ota_install_routing::InstallTarget::Dev);
  EXPECT_EQ(targetForSdFile(SdFileClass::Pr), ota_install_routing::InstallTarget::Dev);
}

TEST(OtaInstallRoutingTest, StableAndUnknownFilesTargetStableSlot) {
  EXPECT_EQ(targetForSdFile(SdFileClass::Stable), ota_install_routing::InstallTarget::Stable);
}

// ── §16 routing matrix ──────────────────────────────────────────────────────
//
// | Running | Source               | Target | Path                       |
// |---------|----------------------|--------|----------------------------|
// | stable  | gh_release           | stable | dance via dev worker       |
// | stable  | SD stable-classified | stable | dance via dev worker       |
// | stable  | SD dev/pr-classified | dev    | direct write               |
// | dev     | dev channel / PR     | dev    | dance via stable worker(§5)|
// | dev     | SD dev/pr-classified | dev    | dance via stable worker(§5)|
// | dev     | SD stable-classified | stable | direct write               |
// | dev     | gh_release           | stable | direct write               |
// | any     | swap worker unavail. | —      | refuse                     |

TEST(OtaInstallRoutingTest, MatrixStableGhReleaseDancesViaDevWorker) {
  EXPECT_EQ(routeForInstall(/*runningIsStable=*/true, /*targetIsStable=*/true, /*workerAvailable=*/true),
            InstallRoute::Dance);
}

TEST(OtaInstallRoutingTest, MatrixStableSdStableClassifiedDancesViaDevWorker) {
  EXPECT_EQ(routeForInstall(true, true, true), InstallRoute::Dance);
}

TEST(OtaInstallRoutingTest, MatrixStableSdDevClassifiedDirectWrite) {
  EXPECT_EQ(routeForInstall(/*runningIsStable=*/true, /*targetIsStable=*/false, /*workerAvailable=*/true),
            InstallRoute::DirectWrite);
}

TEST(OtaInstallRoutingTest, MatrixDevDevChannelDancesViaStableWorker) {
  EXPECT_EQ(routeForInstall(/*runningIsStable=*/false, /*targetIsStable=*/false, /*workerAvailable=*/true),
            InstallRoute::Dance);
}

TEST(OtaInstallRoutingTest, MatrixDevSdDevClassifiedDancesViaStableWorker) {
  EXPECT_EQ(routeForInstall(false, false, true), InstallRoute::Dance);
}

TEST(OtaInstallRoutingTest, MatrixDevSdStableClassifiedDirectWrite) {
  EXPECT_EQ(routeForInstall(/*runningIsStable=*/false, /*targetIsStable=*/true, /*workerAvailable=*/true),
            InstallRoute::DirectWrite);
}

TEST(OtaInstallRoutingTest, MatrixDevGhReleaseDirectWrite) {
  // The intended role replacement: running dev, gh_release writes app0.
  EXPECT_EQ(routeForInstall(false, true, true), InstallRoute::DirectWrite);
}

TEST(OtaInstallRoutingTest, MatrixWorkerUnavailableRefuses) {
  // The refusal only ever gates a DANCE (target == running slot); a direct
  // write does not need the worker and must never refuse because of it.
  EXPECT_EQ(routeForInstall(true, true, /*workerAvailable=*/false), InstallRoute::RefuseNoWorker);
  EXPECT_EQ(routeForInstall(false, false, false), InstallRoute::RefuseNoWorker);
  EXPECT_EQ(routeForInstall(true, false, false), InstallRoute::DirectWrite);
  EXPECT_EQ(routeForInstall(false, true, false), InstallRoute::DirectWrite);
}

TEST(OtaInstallRoutingTest, NoRouteWritesTheRunningSlot) {
  // §3 invariant: the running slot is never a write target. DirectWrite is
  // returned only when the target differs from the running slot; the dance
  // writes the target FROM the worker after a restart. There is no route that
  // writes the running slot in place.
  EXPECT_NE(routeForInstall(true, true, false), InstallRoute::DirectWrite);
  EXPECT_NE(routeForInstall(false, false, false), InstallRoute::DirectWrite);
  EXPECT_NE(routeForInstall(true, true, true), InstallRoute::DirectWrite);
  EXPECT_NE(routeForInstall(false, false, true), InstallRoute::DirectWrite);
}
