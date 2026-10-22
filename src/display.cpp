#include "display.h"

#include <dxgi1_6.h>

#include <cwchar>

DisplayInfo queryDisplay(HWND window) {
  DisplayInfo info;
  HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
  // The system's DXGI, loaded here rather than linked so that nothing changes for the game if it is missing.
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  if (!dxgi)
    return info;
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  auto createFactory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  IDXGIFactory1* factory = nullptr;
  if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    return info;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; !info.found && factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; !info.found && adapter->EnumOutputs(o, &output) == S_OK; ++o) {
      IDXGIOutput6* output6 = nullptr;
      DXGI_OUTPUT_DESC1 desc = {};
      if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) &&
          SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor) {
        info.found = true;
        info.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        info.minNits = desc.MinLuminance;
        info.maxNits = desc.MaxLuminance;
        info.maxFullFrameNits = desc.MaxFullFrameLuminance;
        wcsncpy_s(info.name, desc.DeviceName, _TRUNCATE);
      }
      if (output6)
        output6->Release();
      output->Release();
    }
    adapter->Release();
  }
  factory->Release();
  return info;
}
