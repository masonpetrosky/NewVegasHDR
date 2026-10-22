// NewVegasHDR.ini, next to the plugin. Read at start and again whenever the file changes while the game runs; the
// [HDR] values then apply from the next frame, the rest at the next start.
#pragma once

#include <string>

enum class ToneMapper { Auto, Vanilla, VanillaPlus };

struct Config {
  // [HDR]
  bool hdr = true;
  float peakNits = 0;  // 0: the display's own value, from Windows
  float paperWhiteNits = 203;
  ToneMapper toneMapper = ToneMapper::Auto;
  float shoulderStart = 0.5f;  // fraction of the peak where highlights start to roll off
  float highlightRange = 1;  // how much of the display's range Vanilla Plus Tonemapper's curve may use (0 = as in SDR)
  float gamma = 2.2f;  // how the game's SDR output is decoded to light

  // [Advanced]
  bool replaceShaders = true;
  bool upgradeRenderTargets = true;
  bool sdrExact = false;  // the replacement shaders reproduce the original SDR output (for A/B checks)
  bool forceHdr = false;  // treat the display as HDR even when Windows says it is not (for tests)
};

bool loadConfig(const std::wstring& path, Config& config);
