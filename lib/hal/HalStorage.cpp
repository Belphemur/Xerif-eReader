#include "HalStorage.h"

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <LogFileSink.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#if FREEINK_CAP_USB_MSC
#include <UsbMassStorage.h>
#endif

#include <cassert>

#define SDCard SDCardManager::getInstance()

namespace {
#if FREEINK_CAP_USB_MSC
freeink::UsbMassStorage usbMassStorage;
#endif
}  // namespace

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// Setup calls begin() before any storage users start.
bool HalStorage::begin() { return SDCard.begin(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

bool HalStorage::freeBytes(uint64_t* outFreeBytes) {
  StorageLock lock;
  const uint64_t total = SDCard.sdTotalBytes();
  const uint64_t used = SDCard.sdUsedBytes();
  // Only an unreadable capacity counts as unknown. `used >= total` is a real
  // reading of a full card and reports a genuine 0 so callers can refuse the
  // work instead of discovering it halfway through a transfer.
  if (total == 0) {
    *outFreeBytes = 0;
    return false;
  }
  *outFreeBytes = used >= total ? 0 : total - used;
  return true;
}

void HalStorage::prepareForDeepSleep() {
  // The SD debug log sink (default OFF) gets its pending bytes out — without
  // the flush debounce — and releases its append handle before the card is
  // unmounted; an empty buffer costs zero SD operations. The setting stays
  // enabled, so the next boot re-opens the file lazily.
  logfile::flushAndCloseLogFile();
  StorageLock lock;
  SDCard.shutdown();
}

bool HalStorage::beginUsbDrive() {
#if FREEINK_CAP_USB_MSC
  // USB Drive owns the raw card: flush and release the log sink's append
  // handle first, so the host sees the complete file.
  logfile::flushLogFile();
  logfile::setLogFileEnabled(false);
  StorageLock lock;
  auto* const blockDevice = SDCard.detachFilesystemForRawAccess();
  if (!blockDevice) {
    LOG_ERR("USB", "USB Drive requires a mounted SD filesystem");
    return false;
  }

  if (!usbMassStorage.begin(blockDevice)) {
    LOG_ERR("USB", "USB Drive MSC initialization failed");
    if (!SDCard.begin()) {
      LOG_ERR("USB", "Unable to remount SD card after USB Drive startup failure");
    }
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool HalStorage::disconnectUsbDriveHost() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbMassStorage.disconnectHost();
#else
  return false;
#endif
}

bool HalStorage::usbDriveHostSuspended() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbMassStorage.hostSuspended();
#else
  return false;
#endif
}

void HalStorage::endUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  usbMassStorage.end();
#endif
}

UsbDriveState HalStorage::usbDriveState() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  switch (usbMassStorage.state()) {
    case freeink::UsbMassStorageState::WaitingForHost:
      return UsbDriveState::WaitingForHost;
    case freeink::UsbMassStorageState::Connected:
    case freeink::UsbMassStorageState::Accessed:
      return UsbDriveState::Connected;
    case freeink::UsbMassStorageState::Ejected:
      return UsbDriveState::Ejected;
    case freeink::UsbMassStorageState::Disconnected:
      return UsbDriveState::Disconnected;
    case freeink::UsbMassStorageState::IoError:
      return UsbDriveState::IoError;
    case freeink::UsbMassStorageState::Idle:
      break;
  }
#endif
  return UsbDriveState::Unsupported;
}

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

uint64_t HalStorage::sdTotalBytes() { HAL_STORAGE_WRAPPED_CALL(sdTotalBytes); }
// The fork's SDCardManager::sdUsedBytes() returns uint64_t directly (0 when not
// mounted or the cluster count cannot be determined) instead of taking an
// out-param, so the wrapper maps the SDK value onto the fork API: ready() gates
// "unavailable" and a mounted-but-empty card reports a valid zero.
bool HalStorage::sdUsedBytes(uint64_t& usedBytes) {
  HalStorage::StorageLock lock;
  if (!SDCard.ready()) {
    return false;
  }
  usedBytes = SDCard.sdUsedBytes();
  return true;
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

// Composed of already-locked HalStorage/HalFile operations; needs no
// StorageLock of its own.
bool HalStorage::readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out) {
  out.clear();
  HalFile file;
  if (!openFileForRead(moduleName, path, file)) return false;
  if (file.isDirectory()) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > cap) return false;
  // string growth is a bare allocation under -fno-exceptions; probe first so
  // a large file on a fragmented heap fails soft instead of abort()ing.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < size + 512) {
    LOG_ERR(moduleName, "readFileToString OOM: %u bytes for %s", static_cast<unsigned>(size), path.c_str());
    return false;
  }
  out.resize(size);
  return file.read(out.data(), size) == static_cast<int>(size);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in CLAUDE.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;

  // Sub-file view (design Amendment B.1, the combined cover cache): a view
  // handle translates view-local seeks to [viewBase, viewBase + viewLen) and
  // clamps reads at the window end so a section's Bitmap parse can never
  // bleed into the next section. Base 0 + len UINT64_MAX = plain handle, the
  // common path every regular open uses.
  uint64_t viewBase = 0;
  uint64_t viewLen = UINT64_MAX;

  bool hasView() const { return viewLen != UINT64_MAX; }
  // Non-const on purpose: SdFat's FsFile::position() is not a const member,
  // and every caller reaches this through the (non-const-pointee) impl
  // pointer, including HalFile's const observers.
  uint64_t positionInView() {
    const uint64_t abs = file.position();
    return hasView() && abs >= viewBase ? abs - viewBase : abs;
  }
  bool seekInView(uint64_t pos) {
    if (hasView()) {
      if (pos > viewLen) return false;
      pos += viewBase;
    }
    return file.seekSet(pos);
  }
  bool seekCurInView(int64_t offset) {
    uint64_t abs = file.position();
    if (hasView()) {
      // Translate through the window: out-of-window ends fail rather than
      // silently leaving the view.
      const int64_t rel = static_cast<int64_t>(abs) - static_cast<int64_t>(viewBase) + offset;
      if (rel < 0 || static_cast<uint64_t>(rel) > viewLen) return false;
      abs = viewBase + static_cast<uint64_t>(rel);
    }
    return file.seekSet(abs);
  }
  size_t readInView(void* buf, size_t count) {
    if (hasView()) {
      const uint64_t pos = positionInView();
      if (pos >= viewLen) return 0;
      const uint64_t avail = viewLen - pos;
      if (count > avail) count = static_cast<size_t>(avail);
    }
    return file.read(buf, count);
  }
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::replaceFile(const char* tmpPath, const char* path) {
  StorageLock lock;
  // The staged temp must exist before the destination is touched: a missing
  // temp must never cost the caller its previous file.
  if (!SDCard.exists(tmpPath)) return false;
  return SDCard.replaceFile(tmpPath, path);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileSubview(const char* moduleName, const char* path, uint64_t offset, uint64_t length,
                                 HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  const bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  if (!ok) return false;
  // A window outside the file's extent is an invalid section (a stale
  // directory from another generation) — refuse it instead of serving a
  // short read.
  const uint64_t fileSize = fsFile.fileSize();
  if (length == 0 || offset > fileSize || length > fileSize - offset) {
    file.close();
    return false;
  }
  file.impl->viewBase = offset;
  file.impl->viewLen = length;
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() {
  assert(impl != nullptr);
  // A view handle reports its window — size() stays coherent with the
  // clamped reads. Regular handles: the whole file, unchanged.
  return impl->hasView() ? static_cast<size_t>(impl->viewLen) : impl->file.size();
}
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
uint32_t HalFile::modificationTime() {
  HalStorage::StorageLock lock;
  uint16_t date = 0;
  uint16_t time = 0;
  if (!impl || !impl->file.getModifyDateTime(&date, &time) || date == 0) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
bool HalFile::seek(size_t pos) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->seekInView(pos);
}
bool HalFile::seek64(uint64_t pos) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->seekInView(pos);
}
bool HalFile::seekCur(int64_t offset) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->seekCurInView(offset);
}
bool HalFile::seekSet(size_t offset) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->seekInView(offset);
}
bool HalFile::truncate(uint64_t size) { HAL_FILE_WRAPPED_CALL(truncate, size); }
bool HalFile::sync() { HAL_FILE_WRAPPED_CALL(sync, ); }
int HalFile::available() const {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  if (impl->hasView()) {
    const uint64_t pos = impl->positionInView();
    return pos < impl->viewLen ? static_cast<int>(impl->viewLen - pos) : 0;
  }
  return impl->file.available();
}
size_t HalFile::position() const {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return static_cast<size_t>(impl->positionInView());
}
int HalFile::read(void* buf, size_t count) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return static_cast<int>(impl->readInView(buf, count));
}
int HalFile::read() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  if (impl->hasView() && impl->positionInView() >= impl->viewLen) return -1;  // the view's EOF
  return impl->file.read();
}
size_t HalFile::write(const uint8_t* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
bool HalFile::close() {
  // Total operation, matching SdFat: close on an already-closed file is a
  // no-op, so a never-opened handle (null impl — e.g. a member closed
  // defensively before its first open) closes as no-op instead of asserting.
  // Every other wrapped call still asserts: they would deref null impl.
  if (impl == nullptr) return true;
  HalStorage::StorageLock lock;
  return impl->file.close();
}
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return HalFile(std::make_unique<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }

void HalFile::setSubview(uint64_t offset, uint64_t length) {
  assert(impl != nullptr);
  impl->viewBase = offset;
  impl->viewLen = length;
}
