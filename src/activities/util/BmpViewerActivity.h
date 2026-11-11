#pragma once

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderImage();
#if defined(CROSSPOINT_TTF_READER)
  // Engine-board JPEG/PNG display: the production TTF image pipeline + the
  // reader's absolute-arm plane sequence (design amendment §7).
  bool renderImageEngine(const MappedInputManager::Labels& labels);
#endif
  bool saveJpegSleepCover();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};
