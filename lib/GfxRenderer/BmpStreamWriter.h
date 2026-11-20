#pragma once

#include <cstdint>
#include <memory>

class Atkinson1BitDitherer;
class AtkinsonDitherer;
class Print;

bool calculateBmpOutputSize(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight, bool crop,
                            int& outputWidth, int& outputHeight);

// The tone bake writeRow() applies (design 2026-11-20-cover-image-pipeline.md
// section 2). BlueNoise is the reader's ordered blue-noise bake; Legacy keeps
// the Atkinson error-diffusion ditherers (GIF covers stay on it this PR,
// design decision 3).
enum class BmpBake { Legacy, BlueNoise };

class BmpStreamWriter {
 public:
  BmpStreamWriter();
  ~BmpStreamWriter();

  BmpStreamWriter(const BmpStreamWriter&) = delete;
  BmpStreamWriter& operator=(const BmpStreamWriter&) = delete;

  bool begin(Print& output, int width, int height, bool oneBit, bool originalThresholds = false,
             BmpBake bake = BmpBake::Legacy);
  bool writeRow(const uint8_t* grayRow, int y);

  int getWidth() const { return width; }
  int getHeight() const { return height; }
  uint8_t* getScratchRow();

 private:
  Print* output{nullptr};
  int width{0};
  int height{0};
  int bytesPerRow{0};
  bool oneBit{false};
  bool blueNoise{false};  // bake == BlueNoise && kUseBlueNoiseBake (BmpStreamWriter.cpp)
  uint8_t rowsSinceYield{0};
  std::unique_ptr<uint8_t[]> packedRow;
  std::unique_ptr<uint8_t[]> scratchRow;
  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;
};

class GrayRowScaler {
 public:
  bool begin(int sourceWidth, int sourceHeight, BmpStreamWriter& output);
  bool writeRow(const uint8_t* sourceRow, int sourceY);

 private:
  int sourceWidth{0};
  int outputWidth{0};
  int outputHeight{0};
  uint32_t scaleX{0};
  uint32_t scaleY{0};
  uint32_t nextOutputBoundary{0};
  int currentOutputY{0};
  int accumulatedSourceRows{0};
  BmpStreamWriter* output{nullptr};
  std::unique_ptr<uint32_t[]> rowSums;
};
