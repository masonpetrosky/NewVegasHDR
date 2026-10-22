#include "config.h"

#include <windows.h>

#include <cwchar>

namespace {

std::wstring readString(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
  wchar_t buffer[256] = {};
  GetPrivateProfileStringW(section, key, L"", buffer, 256, path.c_str());
  return buffer;
}

void readFloat(const std::wstring& path, const wchar_t* section, const wchar_t* key, float& value) {
  std::wstring s = readString(path, section, key);
  if (!s.empty())
    value = static_cast<float>(_wtof(s.c_str()));
}

void readBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool& value) {
  std::wstring s = readString(path, section, key);
  if (!s.empty())
    value = _wtoi(s.c_str()) != 0;
}

}  // namespace

bool loadConfig(const std::wstring& path, Config& config) {
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    return false;
  Config c;
  readBool(path, L"HDR", L"Enabled", c.hdr);
  readFloat(path, L"HDR", L"PeakNits", c.peakNits);
  readFloat(path, L"HDR", L"PaperWhiteNits", c.paperWhiteNits);
  readFloat(path, L"HDR", L"ShoulderStart", c.shoulderStart);
  readFloat(path, L"HDR", L"Gamma", c.gamma);
  std::wstring toneMapper = readString(path, L"HDR", L"ToneMapper");
  if (_wcsicmp(toneMapper.c_str(), L"Vanilla") == 0)
    c.toneMapper = ToneMapper::Vanilla;
  else if (_wcsicmp(toneMapper.c_str(), L"VanillaPlus") == 0)
    c.toneMapper = ToneMapper::VanillaPlus;
  readBool(path, L"Advanced", L"ReplaceShaders", c.replaceShaders);
  readBool(path, L"Advanced", L"UpgradeRenderTargets", c.upgradeRenderTargets);
  readBool(path, L"Advanced", L"SdrExact", c.sdrExact);
  readBool(path, L"Advanced", L"ForceHdr", c.forceHdr);

  // Keep the numbers sane whatever the file says.
  if (c.peakNits < 0)
    c.peakNits = 0;
  if (c.paperWhiteNits < 40)
    c.paperWhiteNits = 40;
  if (c.peakNits > 0 && c.peakNits < c.paperWhiteNits)
    c.peakNits = c.paperWhiteNits;
  if (c.shoulderStart < 0.1f)
    c.shoulderStart = 0.1f;
  if (c.shoulderStart > 0.95f)
    c.shoulderStart = 0.95f;
  if (c.gamma < 1.5f || c.gamma > 3)
    c.gamma = 2.2f;
  config = c;
  return true;
}
