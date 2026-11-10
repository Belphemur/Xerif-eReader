#include "DevOtaClient.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first (same pin as OtaUpdater.cpp).
#include "HttpDownloader.h"
#include <Arduino.h>
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
constexpr char NVS_PENDING_KIND_KEY[] = "pendk";
constexpr char NVS_PENDING_DIGEST_KEY[] = "pendd";
constexpr char NVS_PENDING_WRITE_KEY[] = "pendw";

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

bool sha256OfFile(const char* path, uint8_t out[32]) { return hashFile(path, out); }

bool pendingInstallSetDigest(const uint8_t digest[32]) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool ok = nvs_set_blob(handle, NVS_PENDING_DIGEST_KEY, digest, 32) == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) LOG_ERR("DEVOTA", "pending-install digest write failed");
  return ok;
}

bool pendingInstallGetDigest(uint8_t digestOut[32]) {
  if (!digestOut) return false;
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t stored = 32;
  const esp_err_t err = nvs_get_blob(handle, NVS_PENDING_DIGEST_KEY, digestOut, &stored);
  nvs_close(handle);
  return err == ESP_OK && stored == 32;
}

bool pendingInstallMarkWriteStarted() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool ok = nvs_set_u8(handle, NVS_PENDING_WRITE_KEY, 1) == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) LOG_ERR("DEVOTA", "write-phase latch write failed");
  return ok;
}

bool pendingInstallWriteStarted() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    // Fail-safe: an unreadable latch means the slot may be modified.
    return true;
  }
  uint8_t stored = 0;
  const esp_err_t err = nvs_get_u8(handle, NVS_PENDING_WRITE_KEY, &stored);
  nvs_close(handle);
  return err != ESP_ERR_NVS_NOT_FOUND && (err != ESP_OK || stored != 0);
}

Error fetchDevManifest(OtaDevVersionEntry* entries, int maxEntries, int* outCount) {
  if (outCount) *outCount = 0;
  if (!entries || maxEntries <= 0 || !outCount) return Error::JSON_PARSE_ERROR;

  std::string releaseBody;
  {
    // [TLS-PROBE] (docs/design/2026-11-10-tls-teardown.md §5): free/maxBlock
    // around each fetch shows whether anything survives a single request.
    const uint32_t preFree = ESP.getFreeHeap();
    const uint32_t preMax = ESP.getMaxAllocHeap();
    const uint32_t fetchStart = millis();
    const bool ok = HttpDownloader::fetchUrl(devReleaseUrl, releaseBody, "", "", MAX_RELEASE_BYTES);
    LOG_DBG("DEVOTA", "[TLS-PROBE] release-json %ums free %u->%u maxBlock %u->%u amt=%zu ok=%d",
            static_cast<unsigned>(millis() - fetchStart), preFree, ESP.getFreeHeap(), preMax, ESP.getMaxAllocHeap(),
            releaseBody.size(), ok ? 1 : 0);
    if (!ok) {
      LOG_ERR("DEVOTA", "Dev release fetch failed");
      return Error::HTTP_ERROR;
    }
  }

  // Reuse the release parser to locate the manifest.json asset (name + URL);
  // the dev release carries no firmware asset under the release-asset name.
  OtaReleaseInfo info;
  parseOtaRelease(releaseBody.data(), releaseBody.size(), nullptr, "manifest.json", info);
  if (!info.hasManifest) {
    LOG_ERR("DEVOTA", "Dev release has no manifest.json");
    return Error::NO_MANIFEST;
  }

  std::string manifestBody;
  {
    const uint32_t preFree = ESP.getFreeHeap();
    const uint32_t preMax = ESP.getMaxAllocHeap();
    const uint32_t fetchStart = millis();
    const bool ok = HttpDownloader::fetchUrl(info.manifestUrl, manifestBody, "", "", MAX_MANIFEST_BYTES);
    LOG_DBG("DEVOTA", "[TLS-PROBE] manifest %ums free %u->%u maxBlock %u->%u amt=%zu ok=%d",
            static_cast<unsigned>(millis() - fetchStart), preFree, ESP.getFreeHeap(), preMax, ESP.getMaxAllocHeap(),
            manifestBody.size(), ok ? 1 : 0);
    if (!ok) {
      LOG_ERR("DEVOTA", "Dev manifest fetch failed");
      return Error::HTTP_ERROR;
    }
  }
  if (!parseOtaDevManifest(manifestBody.data(), manifestBody.size(), entries, maxEntries, outCount)) {
    LOG_ERR("DEVOTA", "Dev manifest parse failed");
    return Error::JSON_PARSE_ERROR;
  }
  if (*outCount == 0) {
    LOG_ERR("DEVOTA", "Dev manifest lists no versions");
    return Error::NO_VERSIONS;
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

  // The SDK file-write API does not create parent directories: a fresh SD
  // card has no /.crosspoint yet and the staging open would fail before any
  // byte is fetched. ensureDirectoryExists() creates missing parents.
  if (!Storage.ensureDirectoryExists("/.crosspoint")) {
    LOG_ERR("DEVOTA", "cannot create staging directory");
    return Error::HTTP_ERROR;
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
  const uint32_t preFree = ESP.getFreeHeap();
  const uint32_t preMax = ESP.getMaxAllocHeap();
  const uint32_t dlStart = millis();
  const auto err = HttpDownloader::downloadToFile(entry.url, STAGING_PATH, dlProgress);
  LOG_DBG("DEVOTA", "[TLS-PROBE] asset-download %ums free %u->%u maxBlock %u->%u",
          static_cast<unsigned>(millis() - dlStart), preFree, ESP.getFreeHeap(), preMax, ESP.getMaxAllocHeap());
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

  // alreadyValidated=false: the manifest SHA-256 pinned the downloaded bytes,
  // but the flasher still runs the full structural image check (magic,
  // segments, checksum, SHA trailer, board tag) against the dev slot's size
  // before writing. The write progress reuses the same percent-edge callback
  // with writing=true so the UI can label the phase.
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

  // §16 write-phase latch: set before the first erase so a later resume can
  // tell the dev slot may already be partially written. The dance's refusal
  // path reads it; a manual install leaves an inert lone latch key behind
  // (harmless — no pending record exists to dispatch it).
  pendingInstallMarkWriteStarted();

  const firmware_flash::Result flash = firmware_flash::flashFromSdPathInto(STAGING_PATH, dev, wrappedProgress, &fctx,
                                                                           /*alreadyValidated=*/false);
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
  nvs_erase_key(handle, NVS_PENDING_WRITE_KEY);
  const bool ok =
      nvs_set_str(handle, NVS_PENDING_KEY, version ? version : "") == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) LOG_ERR("DEVOTA", "pending-install flag write failed");
  return ok;
}

bool pendingInstallSet(const char* payload, const PendingKind kind) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  // A fresh record is not mid-write: clear any latch a previous aborted
  // dance left behind before stamping the new one.
  nvs_erase_key(handle, NVS_PENDING_WRITE_KEY);
  const bool ok = nvs_set_str(handle, NVS_PENDING_KEY, payload ? payload : "") == ESP_OK &&
                  nvs_set_u8(handle, NVS_PENDING_KIND_KEY, static_cast<uint8_t>(kind)) == ESP_OK &&
                  nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) LOG_ERR("DEVOTA", "pending-install flag write failed");
  return ok;
}

PendingKind pendingInstallKind() {
  PendingKind kind = PendingKind::DevChannel;
  pendingInstallGetKind(kind);
  return kind;
}

bool pendingInstallGetKind(PendingKind& kindOut) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  uint8_t stored = 0;
  const esp_err_t err = nvs_get_u8(handle, NVS_PENDING_KIND_KEY, &stored);
  nvs_close(handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    // No kind key: the flag was written by pre-§16 firmware whose only dance
    // was the §5 dev dance.
    kindOut = PendingKind::DevChannel;
    return true;
  }
  if (err != ESP_OK) return false;
  kindOut = stored <= static_cast<uint8_t>(PendingKind::StablePicked) ? static_cast<PendingKind>(stored)
                                                                      : PendingKind::DevChannel;
  return true;
}

bool pendingInstallClear() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    LOG_ERR("DEVOTA", "pending-install clear: nvs_open failed");
    return false;
  }
  nvs_erase_key(handle, NVS_PENDING_KEY);
  nvs_erase_key(handle, NVS_PENDING_KIND_KEY);
  nvs_erase_key(handle, NVS_PENDING_DIGEST_KEY);
  nvs_erase_key(handle, NVS_PENDING_WRITE_KEY);
  const bool ok = nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) {
    LOG_ERR("DEVOTA", "pending-install clear: commit failed; record may still be armed");
    return false;
  }
  // Verify: the disarm only counts when nothing readable remains.
  char payload[dev_ota::kPendingPayloadCap];
  return !pendingInstallGet(payload, sizeof(payload));
}

}  // namespace dev_ota
