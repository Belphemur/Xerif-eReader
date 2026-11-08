#pragma once

#include <string>

class OtaUpdater {
 public:
  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    WRONG_DEVICE_ERROR,
    SIGNATURE_ERROR,  // manifest signature or firmware SHA-256 mismatch
  };

  using ProgressCallback = void (*)(void* ctx);

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);
  // §16 stable→stable dance (design §15/§16): stage the pinned gh_release
  // asset to an SD path (the swap space) with SHA-256 verification against
  // the signed manifest. Requires a prior checkForUpdate(); fails closed
  // without a pinned digest. The staged file is written by the dance's
  // completion pass and removed there after the flash.
  OtaUpdaterError stageUpdateToSd(const char* stagingPath, ProgressCallback onProgress = nullptr, void* ctx = nullptr);

 private:
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  std::string manifestUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

  // Trusted SHA-256 pinned from the signed manifest.
  uint8_t expectedSha[32] = {0};
  bool haveExpectedSha = false;

  // Fetch the release's manifest.json + .sig, verify the Ed25519 signature
  // against the baked-in public key, and pin the running board's entry.
  OtaUpdaterError fetchAndVerifyManifest(const std::string& url);
};
