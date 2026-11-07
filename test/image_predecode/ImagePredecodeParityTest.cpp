#include <gtest/gtest.h>

#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <Epub/converters/JpegToFramebufferConverter.h>

// The decode-surface mirror the converters run against (see the file comment).
#include "GfxRenderer.h"
#include "HalDisplay.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Pixel-cache parity between the foreground decode path and the image
// predecode worker's decode path (docs/design/2026-11-06-image-predecode.md
// §3.3, §6.1): the same image bytes decoded with the same RenderConfig must
// produce byte-identical .pxc payloads regardless of which GfxRenderer state
// the decode runs against — the worker renderer sits at its defaults (BW,
// portrait, no strip target) while the foreground renderer may be mid
// grayscale pass (mode + active strip target + dual plane). The dither origin
// (absolute outX/outY) is the one legitimate input the cache depends on, so
// position shifts must change the bytes — that is why a predecode job carries
// the capture-time position.
//
// The real JPEG converter runs here (JPEGDEC fetched + patched like the
// firmware libdeps); the GfxRenderer is the test harness's decode-surface
// mirror (stubs/GfxRenderer.h) because the real renderer does not host-build.

namespace {

// 16x16 grayscale gradient JPEG (baseline, quality 90; cjpeg -grayscale).
const char* GRADIENT_JPEG_HEX =
    "FFD8FFE000104A46494600010100000100010000FFDB00430003020203020203030303040303"
    "04050805050404050A070706080C0A0C0C0B0A0B0B0D0E12100D0E110E0B0B10161011131415"
    "15150C0F171816141812141514FFC0000B080040001001011100FFC4001F0000010501010101"
    "010100000000000000000102030405060708090A0BFFC400B510000201030302040305050404"
    "0000017D01020300041105122131410613516107227114328191A1082342B1C11552D1F02433"
    "627282090A161718191A25262728292A3435363738393A434445464748494A53545556575859"
    "5A636465666768696A737475767778797A838485868788898A92939495969798999AA2A3A4A5"
    "A6A7A8A9AAB2B3B4B5B6B7B8B9BAC2C3C4C5C6C7C8C9CAD2D3D4D5D6D7D8D9DAE1E2E3E4E5E6"
    "E7E8E9EAF1F2F3F4F5F6F7F8F9FAFFDA0008010100003F00F88FE12689FEA7E5F4AFB57E1268"
    "9FEA7E5F4AF9ABE12689FEA7E5F4AFB57E12689FEA7E5F4AF9ABE12689FEA7E5F4AFB57E1268"
    "9FEA7E5F4AF9ABE12689FEA7E5F4AFB57E12689FEA7E5F4AF9ABE12689FEA7E5F4AFA8350F1A"
    "D8FC14F847E29F1DEA096F25BE81A64D7CB6F7374B6AB752AA1F2ADC48C0856964D91AF04967"
    "50031201F1FF00849A27FA9F97D2B94FF82A0F8DFF00E113FD9C3C39E0CB5D47ECB7FE2DD6A3"
    "F3ECBECFBFED5616ABE6CBF3952136DC3589E1958E7032BBC57B5FC24D13FD4FCBE95F057FC1"
    "493C6B7DE34FDAD5FC2F225C41A6F83B4CB4B18206BA69219659E25BB92E163C011B32CF144D"
    "8C9616E849E8ABFA41E10BED2FC0DE15D4FC49ADDC7D8B45D1ECA6D42FAE7CB693C982243248"
    "FB50166C2A9385049C7009AFC5FD1B5ABEF895E3FD7FC5FA9C16F06A5E20D4EE755BA8AD1596"
    "1496795A57540C58850CE4004938C649EB5FFFD9";

std::vector<uint8_t> fromHex(const char* hex) {
  std::vector<uint8_t> bytes;
  for (const char* p = hex; p[0] != '\0' && p[1] != '\0'; p += 2) {
    bytes.push_back(static_cast<uint8_t>(std::stoul(std::string(p, 2), nullptr, 16)));
  }
  return bytes;
}

std::vector<uint8_t> readAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

HalDisplay sharedDisplay;

class ImagePredecodeParityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    jpegBytes_ = fromHex(GRADIENT_JPEG_HEX);
    dir_ = std::filesystem::temp_directory_path() / "crosspoint-image-predecode";
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::vector<uint8_t> jpegBytes_;
  std::filesystem::path dir_;
};

// Foreground-mid-render configuration: grayscale mode with an ACTIVE strip
// target (tiled gray pass) and a dual-plane scratch. If the cache payload
// depended on any of this, the two paths would diverge and the test fails.
static void configureAsForeground(GfxRenderer& renderer) {
  static uint8_t band[8 * 4];
  static uint8_t dual[8 * 4];
  memset(band, 0xAA, sizeof(band));
  memset(dual, 0x55, sizeof(dual));
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderer.beginStripTarget(band, 0, 4, dual);
  renderer.setDitherImagesInBw(false);
}

static bool decodeWith(GfxRenderer& renderer, const std::vector<uint8_t>& jpeg, const int x, const int y,
                       const int width, const int height, const std::string& cachePath,
                       bool (*abortPoll)(void*), void* abortCtx) {
  JpegToFramebufferConverter converter;
  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.cachePath = cachePath;
  config.abortPoll = abortPoll;
  config.abortCtx = abortCtx;
  return converter.decodeToFramebuffer(const_cast<uint8_t*>(jpeg.data()), jpeg.size(), renderer, config);
}

TEST_F(ImagePredecodeParityTest, WorkerDefaultsMatchForegroundMidRenderStateAtOneToOne) {
  GfxRenderer foreground(sharedDisplay);
  configureAsForeground(foreground);
  GfxRenderer worker(sharedDisplay);  // ctor defaults: BW, portrait, no strip

  const std::string foregroundCache = (dir_ / "fg.pxc").string();
  const std::string workerCache = (dir_ / "wk.pxc").string();
  ASSERT_TRUE(decodeWith(foreground, jpegBytes_, 0, 0, 16, 16, foregroundCache, nullptr, nullptr));
  ASSERT_TRUE(decodeWith(worker, jpegBytes_, 0, 0, 16, 16, workerCache, nullptr, nullptr));

  const std::vector<uint8_t> foregroundBytes = readAll(foregroundCache);
  const std::vector<uint8_t> workerBytes = readAll(workerCache);
  ASSERT_FALSE(foregroundBytes.empty());
  EXPECT_EQ(foregroundBytes, workerBytes);
}

TEST_F(ImagePredecodeParityTest, WorkerDefaultsMatchForegroundMidRenderStateDownscaled) {
  GfxRenderer foreground(sharedDisplay);
  configureAsForeground(foreground);
  GfxRenderer worker(sharedDisplay);

  const std::string foregroundCache = (dir_ / "fg-small.pxc").string();
  const std::string workerCache = (dir_ / "wk-small.pxc").string();
  ASSERT_TRUE(decodeWith(foreground, jpegBytes_, 0, 0, 8, 8, foregroundCache, nullptr, nullptr));
  ASSERT_TRUE(decodeWith(worker, jpegBytes_, 0, 0, 8, 8, workerCache, nullptr, nullptr));

  EXPECT_EQ(readAll(foregroundCache), readAll(workerCache));
}

TEST_F(ImagePredecodeParityTest, SameConfigAndPositionIsDeterministic) {
  GfxRenderer first(sharedDisplay);
  GfxRenderer second(sharedDisplay);
  const std::string firstCache = (dir_ / "d1.pxc").string();
  const std::string secondCache = (dir_ / "d2.pxc").string();
  ASSERT_TRUE(decodeWith(first, jpegBytes_, 2, 1, 16, 16, firstCache, nullptr, nullptr));
  ASSERT_TRUE(decodeWith(second, jpegBytes_, 2, 1, 16, 16, secondCache, nullptr, nullptr));
  EXPECT_EQ(readAll(firstCache), readAll(secondCache));
}

TEST_F(ImagePredecodeParityTest, PositionShiftChangesCacheBytes) {
  GfxRenderer renderer(sharedDisplay);
  const std::string atOrigin = (dir_ / "p0.pxc").string();
  const std::string atShift = (dir_ / "p1.pxc").string();
  ASSERT_TRUE(decodeWith(renderer, jpegBytes_, 0, 0, 16, 16, atOrigin, nullptr, nullptr));
  ASSERT_TRUE(decodeWith(renderer, jpegBytes_, 2, 1, 16, 16, atShift, nullptr, nullptr));
  // The Bayer dither indexes absolute screen coordinates (design §3.3(b)):
  // this is why a predecode job must carry the capture-time position.
  EXPECT_NE(readAll(atOrigin), readAll(atShift));
}

TEST_F(ImagePredecodeParityTest, AbortedDecodeFailsAndDeletesThePartialCache) {
  GfxRenderer renderer(sharedDisplay);
  const std::string cachePath = (dir_ / "abort.pxc").string();

  int callbacks = 0;
  const auto abortPoll = +[](void* ctx) { return ++(*static_cast<int*>(ctx)) > 2; };

  EXPECT_FALSE(decodeWith(renderer, jpegBytes_, 0, 0, 16, 16, cachePath, abortPoll, &callbacks));
  EXPECT_GT(callbacks, 2);
  // No torn cache may survive an abort (design §3.4 rule 1): the converter's
  // failure path drops the partial file.
  EXPECT_FALSE(std::filesystem::exists(cachePath));
}

}  // namespace
