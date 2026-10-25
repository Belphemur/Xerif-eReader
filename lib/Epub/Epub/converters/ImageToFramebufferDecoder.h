#pragma once
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

class GfxRenderer;

// Read-only, seekable view over a memory buffer (PSRAM pool buffer on PSRAM
// boards). Satisfies the HalFile-shaped read/seek surface that the PNG BMP
// converter's chunk walker consumes, so the same decode core runs from SD
// files and PSRAM buffers.
class HalMemoryFile {
 public:
  HalMemoryFile() = default;
  HalMemoryFile(const uint8_t* data, size_t byteLength) : data_(data), size_(byteLength) {}

  void attach(const uint8_t* data, size_t byteLength) {
    data_ = data;
    size_ = byteLength;
    pos_ = 0;
  }

  int read(void* buf, size_t count) {
    if (data_ == nullptr) return -1;
    if (pos_ >= size_) return 0;
    // Subtraction form: pos_ is proven <= size_ by the seek contract, so this
    // cannot overflow (pos_ + count could).
    if (count > size_ - pos_) count = size_ - pos_;
    memcpy(buf, data_ + pos_, count);
    pos_ += count;
    return static_cast<int>(count);
  }

  bool seek(size_t pos) {
    if (pos > size_) return false;
    pos_ = pos;
    return true;
  }
  bool seekSet(size_t pos) { return seek(pos); }
  bool seekCur(int64_t offset) { return seek(static_cast<size_t>(static_cast<int64_t>(pos_) + offset)); }

  size_t position() const { return pos_; }
  size_t size() const { return size_; }
  int available() const { return static_cast<int>(size_ - pos_); }
  bool isOpen() const { return data_ != nullptr; }
  explicit operator bool() const { return data_ != nullptr; }
  static bool close() { return true; }
  static void flush() {}

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
};

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x = 0;
  int y = 0;
  int maxWidth = 0;
  int maxHeight = 0;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  float sourceCropX = 0.0f;         // Fraction cropped equally from the left and right edges
  float sourceCropY = 0.0f;         // Fraction cropped equally from the top and bottom edges
  bool preserveAlpha = false;       // Skip transparent pixels instead of compositing them against white
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  // Memory-backed variant used by PSRAM image staging. Decoders that do not
  // support it leave the default false so callers fall back to the SD path.
  virtual bool decodeToFramebuffer(uint8_t* data, size_t size, GfxRenderer& renderer, const RenderConfig& config) {
    (void)data;
    (void)size;
    (void)renderer;
    (void)config;
    return false;
  }

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual bool getDimensions(const uint8_t* data, size_t size, ImageDimensions& dims) const {
    (void)data;
    (void)size;
    (void)dims;
    return false;
  }

  virtual const char* getFormatName() const = 0;

  // Call from per-row/per-MCU decode callbacks (free functions, hence public):
  // yields one tick at most every 250 ms so multi-second decodes keep the idle
  // task (and its watchdog) fed. `lastYieldMs` is caller-held state,
  // initialized to the decode start time.
  static void yieldDuringDecode(uint32_t& lastYieldMs);

  // Validate decoder/header dimensions before narrowing them into the layout
  // representation. Shared by header probing and decoder fallbacks.
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format);

 protected:
  // Size validation helpers. The cap bounds decode TIME, not memory: both decoders
  // stream (JPEG in MCU bands at 1/2..1/8 coarse scale, PNG scanline-by-scanline
  // with its own width-based row-buffer guard), so RAM never scales with source
  // area. 8 MP admits real-world ebook covers (KDP recommends 1600x2560 and
  // 2000x3000) while keeping a worst-case single decode in single-digit seconds;
  // the row callbacks yield periodically so a long decode cannot starve the idle
  // task's watchdog.
  static constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;
  static constexpr int64_t MAX_SOURCE_PIXELS = 8388608;  // 8 MP (e.g. 2048 * 4096)

  static void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
