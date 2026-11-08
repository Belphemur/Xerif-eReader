#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <Memory.h>
#include <esp_ota_ops.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <string>

#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"
#include "OtaJson.h"
#include "OtaSignature.h"
#include "OtaSlots.h"

namespace {
// This fork (Xerif) publishes its firmware as GitHub releases on the
// "Belphemur/Xerif-eReader" repo (renamed from crosspoint-x-reader). The updater reads
// the latest release + a signed manifest attached to it. The manifest is what
// we actually verify; the firmware is stream-checked against the manifest's
// signed SHA-256 (not locked/co-signed).
//
// OTA_REPO is defined as a build flag in platformio.ini [base] so different
// builds (e.g. an upstream-synced branch) can point at a different host without
// touching this file.
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/" OTA_REPO "/releases/latest";
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Reset the manifest-derived trust state so a fresh check can't inherit a
  // stale SHA-256 pinned by a previous (possibly failed) check.
  haveExpectedSha = false;
  memset(expectedSha, 0, sizeof(expectedSha));

  // Buffer the ~32KB release JSON and parse it with ArduinoJson, the same way
  // the signed-manifest path buffers its body. The fetch is capped so an
  // oversized/hostile response can't exhaust the heap, which also bounds the
  // parser's pool. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  constexpr size_t MAX_RELEASE_BYTES = 65536;
  std::string releaseBody;
  // Each board updates from xpoint-<version>-<device>.bin, then falls back to
  // the crosspoint-<version>-<device>.bin compatibility name. The combined C3
  // X4/X3 image uses the x3-x4 device tag; other asset suffixes match the
  // firmware board tag. The version embeds the release tag, which is only
  // known after parsing, so the body is parsed once for the tag and again
  // (below) to match the assets.
  const bool isX4 = board_tag::boardNameLen() == 2 && memcmp(board_tag::boardName(), "x4", 2) == 0;
  char assetSuffix[20] = "-x3-x4";
  if (!isX4) {
    snprintf(assetSuffix, sizeof(assetSuffix), "-%.*s", static_cast<int>(board_tag::boardNameLen()),
             board_tag::boardName());
  }
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, releaseBody, "", "", MAX_RELEASE_BYTES);
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  OtaReleaseInfo info;
  parseOtaRelease(releaseBody.data(), releaseBody.size(), nullptr, "manifest.json", info);

  if (!info.hasTag) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  // Fork release tags are "v"-prefixed ("v1.15.0"); the asset name carries the
  // bare version ("xerif-1.16.0-x4pro.bin").
  const char* version = info.tagName;
  if (*version == 'v' || *version == 'V') ++version;
  char assetName[48];
  snprintf(assetName, sizeof(assetName), "xerif-%s%s.bin", version, assetSuffix);
  parseOtaRelease(releaseBody.data(), releaseBody.size(), assetName, "manifest.json", info);

  // Legacy fallback: releases published before either versioned naming still
  // carry firmware[-<board>].bin assets.
  if (!info.hasFirmware) {
    snprintf(assetName, sizeof(assetName), isX4 ? "firmware.bin" : "firmware-%.*s.bin",
             static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
    parseOtaRelease(releaseBody.data(), releaseBody.size(), assetName, "manifest.json", info);
  }

  if (!info.hasFirmware) {
    LOG_INF("OTA", "No OTA firmware asset in latest release");
    return NO_UPDATE;
  }

  latestVersion = info.tagName;
  otaUrl = info.firmwareUrl;
  otaSize = info.firmwareSize;
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());

  // Fetch + verify the signed manifest if the release carries one. We do NOT
  // hard-fail here if it is missing (older/third-party releases) — we simply
  // skip signature verification and rely on the existing chip/board guards.
  manifestUrl.clear();
  if (info.hasManifest) {
    manifestUrl = info.manifestUrl;
    const auto mres = fetchAndVerifyManifest(manifestUrl);
    if (mres != OK) {
      LOG_ERR("OTA", "Signed manifest check failed (%d)", mres);
      return mres;
    }
  } else {
    LOG_INF("OTA", "Release has no signed manifest; skipping signature verification");
  }

  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::fetchAndVerifyManifest(const std::string& url) {
  // Pull the manifest body into memory (it is tiny — a few hundred bytes) so
  // we can verify its Ed25519 signature. Cap both buffers: the manifest and its
  // signature are untrusted until verified, and we never want an oversized
  // response to exhaust the heap on the constrained device (see #8).
  std::string manifestJson;
  std::string sigJson;
  constexpr size_t MAX_MANIFEST_BYTES = 4096;
  constexpr size_t MAX_SIG_BYTES = 256;
  const bool mOk = HttpDownloader::fetchUrl(url, manifestJson, "", "", MAX_MANIFEST_BYTES);
  if (!mOk) {
    LOG_ERR("OTA", "Manifest fetch failed (or exceeded %u bytes)", (unsigned)MAX_MANIFEST_BYTES);
    return HTTP_ERROR;
  }
  const std::string sigUrl = url + ".sig";
  const bool sOk = HttpDownloader::fetchUrl(sigUrl, sigJson, "", "", MAX_SIG_BYTES);
  if (!sOk) {
    LOG_ERR("OTA", "Manifest signature fetch failed (or exceeded %u bytes)", (unsigned)MAX_SIG_BYTES);
    return HTTP_ERROR;
  }

  if (!ota_signature::verifyManifest(manifestJson, sigJson)) {
    LOG_ERR("OTA", "Manifest signature verification FAILED");
    return SIGNATURE_ERROR;
  }

  // Parse the now-trusted manifest and pin the running board's entry (URL,
  // size, SHA-256). installUpdate() re-stream-checks the firmware against it.
  // NOTE: we keep the release tag as latestVersion (shown to the user in the UI,
  // e.g. "1.2.3-x4pro"); the manifest's version is only the bare semver.
  auto entries = makeUniqueNoThrow<ManifestBoardEntry[]>(OTA_MANIFEST_MAX_BOARDS);
  if (!entries) {
    LOG_ERR("OTA", "OOM: manifest board entries");
    return OOM_ERROR;
  }
  char manifestVersion[32] = {0};
  int nEntries = 0;
  if (!parseOtaManifest(manifestJson.data(), manifestJson.size(), entries.get(), OTA_MANIFEST_MAX_BOARDS, &nEntries,
                        manifestVersion, sizeof(manifestVersion))) {
    LOG_ERR("OTA", "Manifest JSON parse failed");
    return JSON_PARSE_ERROR;
  }

  const ManifestBoardEntry* entry =
      findBoardEntryForUrl(entries.get(), nEntries, board_tag::boardName(), board_tag::boardNameLen(), otaUrl.c_str());
  if (!entry) {
    LOG_INF("OTA", "Manifest has no entry for this board (%.*s)", static_cast<int>(board_tag::boardNameLen()),
            board_tag::boardName());
    return NO_UPDATE;
  }
  // A signed release MUST carry a SHA-256 for our board. A missing hash for an
  // entry that passed signature verification means the manifest is malformed,
  // so we fail closed rather than flash an unverified image.
  if (!entry->hasSha) {
    LOG_ERR("OTA", "Signed manifest entry for this board has no SHA-256; refusing to flash unverified image");
    return SIGNATURE_ERROR;
  }
  memcpy(expectedSha, entry->sha256, 32);
  haveExpectedSha = true;
  // Prefer the manifest's authoritative URL/size over the release asset entry
  // (the manifest is what was signed).
  if (entry->url[0] != '\0') {
    otaUrl = entry->url;
    otaSize = entry->size;
    totalSize = entry->size;
  }
  LOG_INF("OTA", "Manifest verified; board %.*s url=%s", static_cast<int>(board_tag::boardNameLen()),
          board_tag::boardName(), otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  // Compares only the leading N.N.N segments: the release tag is
  // "v"-prefixed and the running version carries a per-board suffix
  // ("1.8.0-x4pro"), which must not feed sscanf.
  return otaIsVersionNewer(CROSSPOINT_VERSION, latestVersion.c_str());
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  // §15 source rule: a gh_release install ALWAYS targets the stable slot
  // (app0). The former esp_ota_get_next_update_partition(nullptr) wrote the
  // INACTIVE slot, so running stable silently destroyed the dev install in
  // app1. The caller (OtaUpdateActivity) routes the §16 stable→stable dance;
  // this function runs in the completion pass (the running slot is the dev
  // worker) or the direct-write path (running dev). The host-buildable
  // routing decision points are pinned in ota_install_routing::.
  const esp_partition_t* updatePartition = ota_slots::stableSlot();
  if (!updatePartition) {
    LOG_ERR("OTA", "No stable slot (app0) in partition table");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  // The image streams in chunks; only the first bytes carry the header. Buffer
  // the first 14 bytes so we can read chip_id (esp_image_header_t offset 12)
  // and reject a wrong-MCU image before it overwrites the OTA partition.
  uint8_t hdr[14];
  size_t hdrLen = 0;
  bool wrongChip = false;
  // All S3 boards share a chip_id, so also scan the stream for the embedded
  // board tag (FirmwareBoardTag.h). An untagged image passes; a tag naming a
  // different board aborts the download. The wrong image may partially land in
  // the inactive OTA slot, but esp_ota_abort() below means it never becomes
  // the boot target.
  board_tag::Scanner tagScanner;
  ota_signature::Sha256Stream sha;
  const bool checkSha = haveExpectedSha;
  const auto fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    if (hdrLen < sizeof(hdr)) {
      const size_t take = std::min(len, sizeof(hdr) - hdrLen);
      std::memcpy(hdr + hdrLen, data, take);
      hdrLen += take;
      if (hdrLen == sizeof(hdr)) {
        uint16_t imageChip;
        std::memcpy(&imageChip, hdr + 12, sizeof(imageChip));
        const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
        if (deviceChip != 0xFFFF && imageChip != deviceChip) {
          LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
          wrongChip = true;
          return false;  // abort the transfer
        }
      }
    }
    tagScanner.feed(data, len);
    if (tagScanner.mismatch()) {
      LOG_ERR("OTA", "wrong board: image=%s device=%.*s", tagScanner.foundName(),
              static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
      return false;  // abort the transfer
    }
    if (checkSha) sha.update(data, len);
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });

  if (wrongChip || tagScanner.mismatch()) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  // The signed manifest gave us a trusted SHA-256; confirm the streamed image
  // matches before we mark the partition bootable. A mismatch means the bytes
  // on the wire (MITM / CDN corruption) differed from what was signed.
  if (checkSha) {
    uint8_t computedSha[32];
    sha.finish(computedSha);
    if (memcmp(computedSha, expectedSha, 32) != 0) {
      LOG_ERR("OTA", "Firmware SHA-256 does not match signed manifest");
      esp_ota_abort(otaHandle);
      return SIGNATURE_ERROR;
    }
    LOG_INF("OTA", "Firmware SHA-256 matches signed manifest");
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::stageUpdateToSd(const char* stagingPath, ProgressCallback onProgress,
                                                        void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }
  // The signed manifest pinned the expected SHA-256; without it the staged
  // bytes could never be verified. Fail closed (the §4 rule).
  if (!haveExpectedSha) {
    LOG_ERR("OTA", "no manifest SHA-256 pinned; refusing to stage");
    return SIGNATURE_ERROR;
  }

  // The SDK file-write API does not create parent directories: a fresh SD
  // card has no /.crosspoint yet (same prerequisite as the dev installer).
  if (!Storage.ensureDirectoryExists("/.crosspoint")) {
    LOG_ERR("OTA", "cannot create staging directory");
    return INTERNAL_UPDATE_ERROR;
  }

  // Stream the asset to the staging file while computing the manifest
  // SHA-256 — one pass, no second read of the 5 MB image. The chip/board-tag
  // stream guards of installUpdate() are deliberately not repeated here: the
  // completion pass runs the full validateImageFile() (chip + board tag
  // included) before any byte reaches flash, and a wrong-device image cannot
  // pass the signed-manifest SHA-256 check below in the first place.
  // Write to a temp file first and publish with replaceFile() only after the
  // download and SHA-256 verification succeed: a power loss or a failed flush
  // must never leave a partial image at the path the completion pass flashes.
  const std::string tmpPath = std::string(stagingPath) + ".tmp";
  Storage.remove(tmpPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("OTA", tmpPath.c_str(), file) || !file) {
    LOG_ERR("OTA", "cannot open staging file %s", tmpPath.c_str());
    return INTERNAL_UPDATE_ERROR;
  }
  ota_signature::Sha256Stream sha;
  processedSize = 0;
  int lastReportedPct = -1;
  const auto fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    if (file.write(data, len) != len) {
      LOG_ERR("OTA", "staging write failed");
      return false;  // abort the transfer
    }
    sha.update(data, len);
    processedSize += len;
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });
  const bool closed = file.close();
  if (!fetchOk) {
    LOG_ERR("OTA", "staging download failed");
    Storage.remove(tmpPath.c_str());
    return HTTP_ERROR;
  }
  if (!closed) {
    LOG_ERR("OTA", "staging file close failed; image may be incomplete");
    Storage.remove(tmpPath.c_str());
    return INTERNAL_UPDATE_ERROR;
  }

  uint8_t computedSha[32];
  sha.finish(computedSha);
  if (memcmp(computedSha, expectedSha, 32) != 0) {
    LOG_ERR("OTA", "staged firmware SHA-256 does not match signed manifest");
    Storage.remove(tmpPath.c_str());
    return SIGNATURE_ERROR;
  }
  if (!Storage.replaceFile(tmpPath.c_str(), stagingPath)) {
    LOG_ERR("OTA", "staging publish failed");
    Storage.remove(tmpPath.c_str());
    return INTERNAL_UPDATE_ERROR;
  }
  LOG_INF("OTA", "Firmware staged to %s (SHA-256 verified)", stagingPath);
  return OK;
}
