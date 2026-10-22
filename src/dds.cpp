#include "dds.h"

#include <cstdio>
#include <cstring>

namespace {

struct PixelFormat {
  DWORD size, flags, fourCC, bits, r, g, b, a;
};

struct Header {
  DWORD size, flags, height, width, pitch, depth, mips, reserved[11];
  PixelFormat format;
  DWORD caps, caps2, caps3, caps4, reserved2;
};

constexpr DWORD kCaps = 0x1, kHeight = 0x2, kWidth = 0x4, kPitch = 0x8, kPixelFormat = 0x1000;
constexpr DWORD kAlphaPixels = 0x1, kFourCC = 0x4, kRgb = 0x40;
constexpr DWORD kTexture = 0x1000;

}  // namespace

UINT ddsBytesPerPixel(D3DFORMAT format) {
  switch (format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_R32F:
    case D3DFMT_G16R16F:
      return 4;
    case D3DFMT_R16F:
      return 2;
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F:
      return 8;
    case D3DFMT_A32B32G32R32F:
      return 16;
    default:
      return 0;
  }
}

bool writeDds(const std::wstring& path, UINT width, UINT height, D3DFORMAT format, const void* data, UINT pitch) {
  const UINT bpp = ddsBytesPerPixel(format);
  if (!bpp)
    return false;
  Header h = {};
  h.size = sizeof(Header);
  h.flags = kCaps | kHeight | kWidth | kPitch | kPixelFormat;
  h.height = height;
  h.width = width;
  h.pitch = width * bpp;
  h.format.size = sizeof(PixelFormat);
  h.caps = kTexture;
  switch (format) {
    case D3DFMT_A8R8G8B8:
      h.format = {sizeof(PixelFormat), kRgb | kAlphaPixels, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000};
      break;
    case D3DFMT_X8R8G8B8:
      h.format = {sizeof(PixelFormat), kRgb, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0};
      break;
    case D3DFMT_A2R10G10B10:
      h.format = {sizeof(PixelFormat), kRgb | kAlphaPixels, 0, 32, 0x3FF00000, 0xFFC00, 0x3FF, 0xC0000000};
      break;
    default:  // the floating point formats go by their D3DFORMAT number, as legacy DDS readers expect
      h.format = {sizeof(PixelFormat), kFourCC, static_cast<DWORD>(format), 0, 0, 0, 0, 0};
      break;
  }
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f)
    return false;
  std::fwrite("DDS ", 1, 4, f);
  std::fwrite(&h, sizeof h, 1, f);
  const auto* row = static_cast<const unsigned char*>(data);
  for (UINT y = 0; y < height; ++y, row += pitch)
    std::fwrite(row, 1, width * bpp, f);
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}
