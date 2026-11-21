#pragma once

// ImageDecodeSource — the decode engine's source seam (design Amendment B.2):
// a decode front end consumes its compressed input through one seek/read
// surface, so an SD file and (in principle) a zip entry present identically
// and the JPEGDEC/PNGLE/GIF decode loops run unchanged behind it.
//
// Shipped consumers: the JPEG framebuffer converter's file path (the
// per-open heap HalFile is gone — the HalFile is caller-owned). The PNG
// front end already runs templated over this shape (HalFileView /
// HalMemoryFile, PngToBmpConverter.cpp), and the memory paths keep their
// native zero-copy opens (JPEGDEC::openRAM / AnimatedGIF::openRAM) — an
// indirection over a resident buffer would add nothing.
//
// POD vtable, caller-owned lifetime, zero heap.

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

namespace imagebake {

struct DataSource {
  // `size` receives the source's byte length; open positions at 0.
  bool (*open)(void* ctx, int32_t* size) = nullptr;
  // Absolute seek within [0, size); read advances the position.
  bool (*seek)(void* ctx, int32_t pos) = nullptr;
  int32_t (*read)(void* ctx, uint8_t* buf, int32_t len) = nullptr;
  void (*close)(void* ctx) = nullptr;
  void* ctx = nullptr;

  bool openSource(int32_t* size) { return open != nullptr && open(ctx, size); }
  bool seekSource(int32_t pos) { return seek != nullptr && seek(ctx, pos); }
  int32_t readSource(uint8_t* buf, int32_t len) { return read != nullptr ? read(ctx, buf, len) : 0; }
  void closeSource() {
    if (close != nullptr) close(ctx);
  }
};

// SD-file adapter over a caller-owned HalFile.
inline bool halFileSourceOpen(void* ctx, int32_t* size) {
  auto* f = static_cast<HalFile*>(ctx);
  if (!f->isOpen()) return false;
  if (!f->seek(0)) return false;
  *size = static_cast<int32_t>(f->size());
  return true;
}
inline bool halFileSourceSeek(void* ctx, int32_t pos) { return static_cast<HalFile*>(ctx)->seek(pos); }
inline int32_t halFileSourceRead(void* ctx, uint8_t* buf, int32_t len) {
  return static_cast<HalFile*>(ctx)->read(buf, static_cast<size_t>(len));
}
inline void halFileSourceClose(void*) {}
inline DataSource halFileSource(HalFile& f) {
  return DataSource{&halFileSourceOpen, &halFileSourceSeek, &halFileSourceRead, &halFileSourceClose, &f};
}

}  // namespace imagebake
