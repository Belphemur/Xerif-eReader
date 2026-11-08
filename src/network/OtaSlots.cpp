#include "OtaSlots.h"

#include <Logging.h>
#include <esp_ota_ops.h>

#include <algorithm>
#include <cstring>

namespace ota_slots {

namespace {
// esp_app_desc_t sits at the start of the DROM segment, the first segment of
// the app image: image header (24 B) + segment header (8 B) = offset 0x20.
// Layout per esp_app_desc.h: magic_word, secure_version, reserv1[2], then
// version[32] at +16.
constexpr size_t APP_DESC_OFFSET = 0x20;
constexpr size_t VERSION_OFFSET_IN_DESC = 16;
constexpr uint32_t APP_DESC_MAGIC = 0xABCD5432;

const esp_partition_t* findOtaAppSlot(esp_partition_subtype_t subtype) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
}

// esp_image_header_t is 24 bytes on every supported chip (C3/S3 carry the
// extended header); each segment adds an 8-byte header (load address +
// data length).
constexpr size_t kImageHeaderSize = 24;
constexpr size_t kSegmentHeaderSize = 8;
constexpr size_t kMaxSegments = 17;  // IDF caps images at 16 segments
constexpr size_t kScanChunk = 2048;
}  // namespace

// Shared scan: walks `app`'s segment chain to bound the search to the real
// image extent, then chunk-scans with overlapping reads so a marker straddling
// a chunk boundary is still found.
bool imageContainsMarker(const esp_partition_t* app, const char* marker, size_t markerLen) {
  if (!app || app->size < kImageHeaderSize + kSegmentHeaderSize) return false;

  uint8_t header[kImageHeaderSize];
  if (esp_partition_read(app, 0, header, sizeof(header)) != ESP_OK) return false;
  if (header[0] != 0xE9) return false;  // ESP_IMAGE_MAGIC
  const size_t segmentCount = header[1];
  if (segmentCount == 0 || segmentCount > kMaxSegments) return false;

  // Walk the segment chain to bound the scan to the real image extent; the
  // erased flash beyond it can never contain the marker.
  size_t extent = kImageHeaderSize;
  for (size_t i = 0; i < segmentCount; ++i) {
    uint8_t segHeader[kSegmentHeaderSize];
    if (extent > app->size - kSegmentHeaderSize) return false;
    if (esp_partition_read(app, extent, segHeader, sizeof(segHeader)) != ESP_OK) return false;
    uint32_t dataLen;
    memcpy(&dataLen, segHeader + 4, sizeof(dataLen));
    if (dataLen > app->size - extent - kSegmentHeaderSize) return false;
    extent += kSegmentHeaderSize + dataLen;
  }

  // Chunked scan with overlapping reads: each chunk re-reads the previous
  // chunk's tail (markerLen - 1 bytes) so a marker straddling a chunk
  // boundary is still found. The final partial chunk never needs an overlap
  // because the marker cannot extend past the image.
  static_assert(kScanChunk > 0, "scan chunk must be positive");
  if (markerLen == 0 || markerLen >= kScanChunk) return false;
  const size_t step = kScanChunk - markerLen + 1;
  const uint8_t* markerBytes = reinterpret_cast<const uint8_t*>(marker);
  uint8_t buf[kScanChunk];
  for (size_t start = 0; start < extent; start += step) {
    size_t len = extent - start;
    if (len > kScanChunk) len = kScanChunk;
    if (esp_partition_read(app, start, buf, len) != ESP_OK) return false;
    if (std::search(buf, buf + len, markerBytes, markerBytes + markerLen) != buf + len) return true;
  }
  return false;
}

bool imageHasResumeMarker(const esp_partition_t* app) {
  return imageContainsMarker(app, kResumeHookMarker, sizeof(kResumeHookMarker) - 1);
}

bool imageHasSwapKindsMarker(const esp_partition_t* app) {
  return imageContainsMarker(app, kSwapKindsMarker, sizeof(kSwapKindsMarker) - 1);
}

const esp_partition_t* stableSlot() { return findOtaAppSlot(ESP_PARTITION_SUBTYPE_APP_OTA_0); }

const esp_partition_t* devSlot() { return findOtaAppSlot(ESP_PARTITION_SUBTYPE_APP_OTA_1); }

bool sameSlot(const esp_partition_t* a, const esp_partition_t* b) {
  return a != nullptr && b != nullptr && a->address == b->address && a->size == b->size;
}

const esp_partition_t* runningSlot() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  if (sameSlot(run, stableSlot())) return stableSlot();
  if (sameSlot(run, devSlot())) return devSlot();
  return nullptr;
}

bool readSlotInfo(const esp_partition_t* app, SlotInfo& out) {
  out = SlotInfo{};
  if (!app) return false;

  // magic_word + secure_version + reserv1[2] + version[32].
  uint8_t buf[16 + 32];
  if (app->size < APP_DESC_OFFSET + sizeof(buf)) return false;
  if (esp_partition_read(app, APP_DESC_OFFSET, buf, sizeof(buf)) != ESP_OK) {
    LOG_ERR("SLOTS", "read failed: %s", app->label);
    return false;
  }

  uint32_t magic;
  memcpy(&magic, buf, sizeof(magic));
  if (magic != APP_DESC_MAGIC) {
    LOG_DBG("SLOTS", "no app desc in %s (magic 0x%08x)", app->label, static_cast<unsigned>(magic));
    return false;
  }

  // version[] is NUL-padded by the image tool; copy up to the first NUL (or
  // erased 0xFF byte, or the full 32 bytes when neither appears) and force
  // termination so a padded, erased, or full-width field still reads as a
  // C string.
  size_t len = 0;
  while (len < 32 && buf[16 + len] != '\0' && buf[16 + len] != 0xFF) ++len;
  memcpy(out.version, buf + 16, len);
  out.version[len] = '\0';
  out.present = len > 0;
  return out.present;
}

}  // namespace ota_slots
