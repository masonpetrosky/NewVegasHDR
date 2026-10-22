// What Windows reports for the display a window is on: whether HDR is on and the luminance range (from the Windows HDR
// Calibration profile when there is one, else from the display's EDID).
#pragma once

#include <windows.h>

struct DisplayInfo {
  bool found = false;
  bool hdr = false;
  float minNits = 0, maxNits = 0, maxFullFrameNits = 0;
  wchar_t name[32] = {};
};

DisplayInfo queryDisplay(HWND window);
