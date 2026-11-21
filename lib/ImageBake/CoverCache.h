#pragma once

// CoverCache — the combined cover cache file (design Amendment B.1): ONE
// decode of a book's cover art produces ONE file with two seekable
// sections, killing today's double decode (generateCoverBmp +
// generateThumbBmp each decode independently):
//
//   [CacheIdentity: magic 'C','P', revision letter 'c', u32 hash32]  (11 B)
//   [section directory: {u32 offset, u32 length, u16 w, u16 h} x 2]  (24 B)
//   [section 0: 1-bit thumb payload — self-contained BMP bytes]
//   [section 1: 2-bit full-cover payload — self-contained BMP bytes]
//
// Sections are self-contained BMP payloads so the existing Bitmap reader
// and all theme/sleep code consume them unchanged through a sub-file view
// (HalStorage::openFileSubview owns the base-offset seek translation;
// Bitmap::parseHeaders seeks absolute 0). The thumb height participates in
// the identity hash (BaseTheme 400, Lyra 226 — a theme switch regenerates
// once), and the sleep cover geometry (crop vs fit) keys the file name the
// same way.
//
// Consumers open through openCoverBitmap(): kind-agnostic on purpose — a
// legacy standalone BMP (GIF-covered books keep one, design decision 3)
// has no 'C','P' magic and opens whole-file, so callers need no fallback
// branches.
//
// PSRAM builds only emit this file (Amendment B.2); no-PSRAM builds keep
// the legacy cover/thumb BMP pair byte-identical to today.

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

#include "ImageBakeCache.h"

namespace imagebake {

// Revision letter for the combined cover cache: bump when a section bake
// changes so every stale file fails the identity check at open.
inline constexpr char kCoverCacheLetter = 'c';

// Section directory: {u32 offset, u32 length, u16 w, u16 h} per entry,
// little-endian, written after the CacheIdentity header.
struct CoverSectionEntry {
  uint32_t offset = 0;
  uint32_t length = 0;
  uint16_t w = 0;
  uint16_t h = 0;
};
inline constexpr size_t kCoverSectionEntryBytes = 12;
inline constexpr size_t kCoverSectionCount = 2;
inline constexpr size_t kCoverDirectoryBytes = kCoverSectionEntryBytes * kCoverSectionCount;
// Everything before section 0.
inline constexpr size_t kCoverPrefixBytes = kCacheHeaderBytes + kCoverDirectoryBytes;

enum class CoverSection : uint8_t { Thumb = 0, Cover = 1 };

// The identity hash: FNV-1a over the revision letter, the requested thumb
// height and the sleep cover geometry. Height participates per the design
// (Amendment B.1): a theme switch or a cover-mode change regenerates once
// under a fresh name, and a file opened for a different height fails the
// header check.
uint32_t coverCacheHash32(int thumbHeight, bool cropped);

// Final file name: <dir>/cover_c<cropChar>_<thumbHeight>.pxc — the [HEIGHT]
// template form below substitutes into the same shape.
bool coverCachePath(const char* dir, int thumbHeight, bool cropped, char* out, size_t cap);

// The RecentBook-cover template: <dir>/cover_c<cropChar>_[HEIGHT].pxc.
// UITheme::getCoverThumbPath's [HEIGHT] substitution yields the final name.
bool coverCacheTemplate(const char* dir, bool cropped, char* out, size_t cap);

// Validate + open one cover bitmap for a consumer:
// - combined cover cache (magic 'C','P'): identity header must match the
//   recomputed hash for (thumbHeight, cropped), the section entry must
//   carry the requested thumb height (Thumb section) / any cover geometry
//   (Cover section), and the payload must fit the file — then a sub-file
//   view over the section is handed out.
// - legacy standalone BMP (no magic): the whole file, unchanged.
// `thumbHeight` is only enforced for CoverSection::Thumb.
bool openCoverBitmap(const char* moduleName, const char* path, int thumbHeight, CoverSection section, HalFile& out);

// Non-opening validity probe (existence checks on the generation paths).
bool coverBitmapIsValid(const char* path, int thumbHeight, CoverSection section);

// Assemble + publish the combined file from two completed section temp
// files: identity header + directory + section bytes, .tmp staging, then
// the FAT publish. `thumbW/thumbH` and `coverW/coverH` are the BMP payload
// dims (also written into the directory). Returns false and leaves no new
// file behind on any failure.
bool assembleCoverCache(const char* finalPath, const char* thumbTmpPath, uint16_t thumbW, uint16_t thumbH,
                        const char* coverTmpPath, uint16_t coverW, uint16_t coverH, int thumbHeight, bool cropped);

}  // namespace imagebake
