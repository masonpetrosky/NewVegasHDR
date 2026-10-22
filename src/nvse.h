// The parts of xNVSE's plugin interface (nvse/PluginAPI.h upstream) this plugin uses. The layouts must match xNVSE's;
// members after isEditor are not used and left out.
#pragma once

#include <cstdint>

namespace nvse {

struct Interface {
  uint32_t nvseVersion;
  uint32_t runtimeVersion;
  uint32_t editorVersion;
  uint32_t isEditor;
};

struct PluginInfo {
  enum { kInfoVersion = 1 };
  uint32_t infoVersion;
  const char* name;
  uint32_t version;
};

}  // namespace nvse
