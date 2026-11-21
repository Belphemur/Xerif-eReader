#pragma once

// Build-time image sink selection (design Amendment B.2, owner-settled):
// BOARD_HAS_PSRAM alone gates the quality engine — PSRAM boards bake the
// blue-noise quality tone, no-PSRAM boards keep the legacy Bayer/Atkinson
// dither byte-identical to the pre-quality baseline. One switch, derived
// once; per-callsite capability predicates are deliberately NOT used (the
// five runtime grayscaleCapabilities() paint gates this replaces were the
// duplication this constant removes).
#if defined(BOARD_HAS_PSRAM)
#define IMAGEBAKE_QUALITY_ENGINE 1
#else
#define IMAGEBAKE_QUALITY_ENGINE 0
#endif

namespace imagebake {

// Whether decodes quantize through the quality blue-noise bake. Exposed as
// an inline constant (not the raw macro) so host tests can pin the letters.
inline constexpr bool kQualitySink = IMAGEBAKE_QUALITY_ENGINE != 0;

// Cache identity letters for the shared pixel-cache format. The letter lives
// inside the cache file (ImageBakeCache.h): a bake change bumps the letter
// and every stale cache fails the header check at open, so the extension
// token never has to change again.
//
// Framebuffer image-page caches (ImageBlock + the JPEG/PNG/GIF framebuffer
// converters): 'n' = the blue-noise paint, 'l' = the legacy Bayer dither.
inline constexpr char kFramebufferCacheLetter = kQualitySink ? 'n' : 'l';

}  // namespace imagebake
