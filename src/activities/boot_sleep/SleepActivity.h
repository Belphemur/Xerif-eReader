#pragma once
#include <string>

#include "activities/Activity.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;

  // Resolves (and generates if needed) the cover BMP for a book path.
  // `renderer` picks the dither thresholds for absolute-capable SSD1677 panels.
  static bool resolveCoverBmpPath(const GfxRenderer& renderer, const std::string& bookPath, std::string& outPath);
  // Renders the final shutdown screen for the auto power off wake, then powers down.
  static void renderShutdownScreen(GfxRenderer& renderer);

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  bool renderCustomSleepBmp(const std::string& path) const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool preserveBackground = false, int progressPercent = -1,
                               uint32_t timeLeftSeconds = 0, int pagesLeft = -1) const;
  bool renderSleepOverlayFile(HalFile& file, const char* pathForLog, int progressPercent = -1,
                              uint32_t timeLeftSeconds = 0, int pagesLeft = -1) const;
  bool renderTransparentOverlayPng(const std::string& path, int progressPercent = -1, uint32_t timeLeftSeconds = 0,
                                   int pagesLeft = -1) const;
  bool renderSleepOverlayPath(const std::string& path, int progressPercent = -1, uint32_t timeLeftSeconds = 0,
                              int pagesLeft = -1) const;
  void renderLastScreenSleepScreen() const;
  void renderTransparentCustomSleepScreen(int progressPercent = -1, uint32_t timeLeftSeconds = 0,
                                          int pagesLeft = -1) const;
  void renderBlankSleepScreen() const;

  // Issues the white pre-clean that onEnter deferred (sleepPreCleanPending_):
  // the clean is skipped only while the selected image screen is expected to
  // repaint the panel through its absolute/Direct gray base pass; every path
  // that ends up flushing plain B/W content calls this first, before its
  // frame is drawn.
  void issueDeferredPreClean() const;

  bool fromTimeout = false;
  // Set by onEnter when the white pre-clean was deferred to the image render
  // path on a grayscale-capable non-Metalio panel; false once issued or once
  // the gray base pass has repainted the panel in its place.
  mutable bool sleepPreCleanPending_ = false;
  // Cover + Transparent: the cover under the overlay, and the same cover while its gray planes are redrawn.
  mutable const Bitmap* overlayCover = nullptr;
  mutable const Bitmap* overlayGrayCover = nullptr;
};
