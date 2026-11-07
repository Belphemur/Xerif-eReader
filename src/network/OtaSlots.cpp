#include "OtaSlots.h"

#include <Logging.h>
#include <esp_ota_ops.h>

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
}  // namespace

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
