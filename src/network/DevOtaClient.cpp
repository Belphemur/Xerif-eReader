#include "DevOtaClient.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first (same pin as OtaUpdater.cpp).
#include "HttpDownloader.h"
#include <Logging.h>
#include <esp_ota_ops.h>
// clang-format on

#include <mbedtls/sha256.h>
#include <nvs.h>

#include <cstring>

#include "FirmwareFlasher.h"
#include "OtaBootSwitch.h"
#include "OtaSlots.h"

namespace dev_ota {

namespace {
constexpr char devReleaseUrl[] = "https://api.github.com/repos/" OTA_REPO "/releases/tags/dev";
constexpr size_t MAX_RELEASE_BYTES = 65536;
constexpr size_t MAX_MANIFEST_BYTES = 4096;

// Single staging file for the download. Overwritten on every install and
// removed afterwards, so dev installs never accumulate SD garbage.
constexpr char STAGING_PATH[] = "/.crosspoint/dev_ota.bin";

constexpr char NVS_NAMESPACE[] = "devota";
constexpr char NVS_PENDING_KEY[] = "pend";

// SHA-256 of the file at `path`, streamed in 4 KiB chunks. False on open or
// read failure.
bool hashFile(const char* path, uint8_t out[32]) {
  HalFile file;
  if (!Storage.openFileForRead("DEVOTA", path, file) || !file) return false;

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, /*is224=*/0);

  uint8_t buf[4096];
  bool ok = true;
  const size_t size = file.fileSize();
  while (ok && file.position() < size) {
    size_t want = size - file.position();
    if (want > sizeof(buf)) want = sizeof(buf);
    const int got = file.read(buf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) {
      ok = false;
      break;
    }
    mbedtls_sha256_update(&sha, buf, want);
  }
  if (ok) mbedtls_sha256_finish(&sha, out);
  mbedtls_sha256_free(&sha);
  file.close();
  return ok;
}
}  // namespace

Error fetchDevManifest(OtaDevVersionEntry* entries, int maxEntries, int* outCount) {
  if (outCount) *outCount = 0;
  if (!entries || maxEntries <= 0 || !outCount) return JSON_PARSE_ERROR;

  std::string releaseBody;
  if (!HttpDownloader::fetchUrl(devReleaseUrl, releaseBody, "", "", MAX_RELEASE_BYTES)) {
    LOG_ERR("DEVOTA", "Dev release fetch failed");
    return HTTP_ERROR;
  }

  // Reuse the release parser to locate the manifest.json asset (name + URL);
  // the dev release carries no firmware asset under the release-asset name.
  OtaReleaseInfo info;
  parseOtaRelease(releaseBody.data(), releaseBody.size(), nullptr, "manifest.json", info);
  if (!info.hasManifest) {
    LOG_ERR("DEVOTA", "Dev release has no manifest.json");
    return NO_MANIFEST;
  }

  std::string manifestBody;
  if (!HttpDownloader::fetchUrl(info.manifestUrl, manifestBody, "", "", MAX_MANIFEST_BYTES)) {
    LOG_ERR("DEVOTA", "Dev manifest fetch failed");
    return HTTP_ERROR;
  }
  if (!parseOtaDevManifest(manifestBody.data(), manifestBody.size(), entries, maxEntries, outCount)) {
    LOG_ERR("DEVOTA", "Dev manifest parse failed");
    return JSON_PARSE_ERROR;
  }
  if (*outCount == 0) {
    LOG_ERR("DEVOTA", "Dev manifest lists no versions");
    return NO_VERSIONS;
  }
  return Error::OK;
}

Error installFromManifestEntry(const OtaDevVersionEntry& entry, ProgressCallback onProgress, void* ctx) {
  const esp_partition_t* dev = ota_slots::devSlot();
  if (!dev) {
    LOG_ERR("DEVOTA", "No dev slot (app1) in partition table");
    return Error::NO_DEV_SLOT;
  }
  if (!entry.hasSha) {
    // A manifest entry without a digest can never be verified: refuse instead
    // of flashing unverified bytes (fail closed, design §4).
    LOG_ERR("DEVOTA", "Manifest entry has no SHA-256");
    return Error::SHA_MISMATCH;
  }

  // 0→100% over the download (writing=false), then the flash run maps onto
  // the same range again with writing=true; the UI labels the two phases.
  size_t dlLastPct = 101;
  const auto dlProgress = [&](size_t downloaded, size_t total) {
    if (!onProgress || total == 0) return;
    const size_t pct = downloaded * 100 / total;
    if (pct != dlLastPct) {
      dlLastPct = pct;
      onProgress(ctx, static_cast<int>(pct), false);
    }
  };
  const auto err = HttpDownloader::downloadToFile(entry.url, STAGING_PATH, dlProgress);
  if (err != HttpDownloader::OK) {
    LOG_ERR("DEVOTA", "Download failed (%d)", static_cast<int>(err));
    Storage.remove(STAGING_PATH);
    return Error::HTTP_ERROR;
  }

  uint8_t actual[32];
  if (!hashFile(STAGING_PATH, actual) || memcmp(actual, entry.sha256, 32) != 0) {
    LOG_ERR("DEVOTA", "SHA-256 mismatch — dropping download");
    Storage.remove(STAGING_PATH);
    return Error::SHA_MISMATCH;
  }

  // Full image validation (magic, segments, checksum, SHA trailer, board tag)
  // against the dev slot's size, then the shared raw-write path. The write
  // progress reuses the same percent-edge callback with writing=true so the
  // UI can label the phase.
  struct FlashCtx {
    ProgressCallback cb;
    void* ctx;
    int lastPct;
  };
  FlashCtx fctx{onProgress, ctx, 101};
  const auto wrappedProgress = [](size_t written, size_t total, void* raw) {
    auto* p = static_cast<FlashCtx*>(raw);
    if (!p->cb || total == 0) return;
    const int pct = static_cast<int>(written * 100 / total);
    if (pct != p->lastPct) {
      p->lastPct = pct;
      p->cb(p->ctx, pct, true);
    }
  };

  const firmware_flash::Result flash = firmware_flash::flashFromSdPathInto(STAGING_PATH, dev, wrappedProgress, &fctx,
                                                                           /*alreadyValidated=*/true);
  Storage.remove(STAGING_PATH);
  if (flash != firmware_flash::Result::OK) {
    LOG_ERR("DEVOTA", "Flash failed: %s", firmware_flash::resultName(flash));
    return Error::FLASH_FAILED;
  }
  LOG_INF("DEVOTA", "Dev build written to %s", dev->label);
  return Error::OK;
}

bool pendingInstallGet(char* versionOut, size_t cap) {
  if (!versionOut || cap == 0) return false;
  versionOut[0] = '\0';
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t stored = cap;
  const esp_err_t err = nvs_get_str(handle, NVS_PENDING_KEY, versionOut, &stored);
  nvs_close(handle);
  return err == ESP_OK;
}

bool pendingInstallSet(const char* version) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool ok =
      nvs_set_str(handle, NVS_PENDING_KEY, version ? version : "") == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) LOG_ERR("DEVOTA", "pending-install flag write failed");
  return ok;
}

void pendingInstallClear() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
  nvs_erase_key(handle, NVS_PENDING_KEY);
  nvs_commit(handle);
  nvs_close(handle);
}

}  // namespace dev_ota
