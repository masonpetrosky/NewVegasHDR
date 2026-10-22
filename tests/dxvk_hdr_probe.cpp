// Checks what a d3d9.dll offers for HDR output without starting the game: loads the DLL by path, unlocks DXVK's
// additional formats, creates an FP16 back buffer on an off-screen window, asks the swap chain for the scRGB and
// HDR10 colour spaces, switches it to scRGB and presents a few frames. DXVK's log (DXVK_LOG_PATH) then shows the
// Vulkan swap chain it really made.
//
// usage: dxvk_hdr_probe <path to d3d9.dll>
#include <windows.h>
#include <d3d9.h>

#include <cstdio>

#include "dxvk_ext.h"

namespace {

const char* yesNo(bool b) { return b ? "yes" : "no"; }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: dxvk_hdr_probe <path to d3d9.dll>\n");
    return 2;
  }
  HMODULE dll = LoadLibraryA(argv[1]);
  if (!dll) {
    std::printf("LoadLibrary failed: %lu\n", GetLastError());
    return 1;
  }
  using CreateFn = IDirect3D9*(WINAPI*)(UINT);
  auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "Direct3DCreate9"));
  IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
  if (!d3d) {
    std::printf("Direct3DCreate9 failed\n");
    return 1;
  }

  ID3D9VkExtInterface* ext = nullptr;
  d3d->QueryInterface(__uuidof(ID3D9VkExtInterface), reinterpret_cast<void**>(&ext));
  std::printf("ID3D9VkExtInterface: %s\n", yesNo(ext));
  if (ext) {
    ext->UnlockAdditionalFormats();
    ext->Release();
  }

  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "NewVegasHDRProbe";
  RegisterClassA(&wc);
  // Far off screen and out of the taskbar: the swap chain needs a real window, nobody needs to see it.
  HWND hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "NewVegasHDR probe", WS_POPUP,
                              -20000, -20000, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 640;
  pp.BackBufferHeight = 360;
  pp.BackBufferFormat = D3DFMT_A16B16G16R16F;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  IDirect3DDevice9* dev = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  std::printf("CreateDevice with an A16B16G16R16F back buffer: 0x%08lx\n", static_cast<unsigned long>(hr));
  if (FAILED(hr)) {
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    std::printf("CreateDevice with an X8R8G8B8 back buffer: 0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr))
      return 1;
  }

  IDirect3DSwapChain9* swapChain = nullptr;
  dev->GetSwapChain(0, &swapChain);
  ID3D9VkExtSwapchain* xsc = nullptr;
  if (swapChain)
    swapChain->QueryInterface(__uuidof(ID3D9VkExtSwapchain), reinterpret_cast<void**>(&xsc));
  std::printf("ID3D9VkExtSwapchain: %s\n", yesNo(xsc));
  int status = 1;
  if (xsc) {
    const bool srgb = xsc->CheckColorSpaceSupport(dxvk::kSrgbNonlinear);
    const bool scrgb = xsc->CheckColorSpaceSupport(dxvk::kExtendedSrgbLinear);
    const bool hdr10 = xsc->CheckColorSpaceSupport(dxvk::kHdr10St2084);
    std::printf("colour spaces: sRGB %s, scRGB %s, HDR10 %s\n", yesNo(srgb), yesNo(scrgb), yesNo(hdr10));

    dxvk::OutputMetadata out = {};
    if (SUCCEEDED(xsc->GetCurrentOutputDesc(&out)))
      std::printf("display: min %.4f nits, max %.0f nits, max full frame %.0f nits, red (%.3f, %.3f), white (%.4f, %.4f)\n",
                  out.minLuminance, out.maxLuminance, out.maxFullFrameLuminance, out.redPrimary[0], out.redPrimary[1],
                  out.whitePoint[0], out.whitePoint[1]);

    hr = xsc->SetColorSpace(dxvk::kExtendedSrgbLinear);
    std::printf("SetColorSpace(scRGB): 0x%08lx\n", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
      dxvk::HdrMetadata md = {};
      md.redPrimary[0] = 0.640f, md.redPrimary[1] = 0.330f;
      md.greenPrimary[0] = 0.300f, md.greenPrimary[1] = 0.600f;
      md.bluePrimary[0] = 0.150f, md.bluePrimary[1] = 0.060f;
      md.whitePoint[0] = 0.3127f, md.whitePoint[1] = 0.3290f;
      md.maxLuminance = 1000.0f;
      md.minLuminance = 0.0f;
      md.maxContentLightLevel = 1000.0f;
      md.maxFrameAverageLightLevel = 400.0f;
      hr = xsc->SetHDRMetaData(&md);
      std::printf("SetHDRMetaData: 0x%08lx\n", static_cast<unsigned long>(hr));
      status = 0;
    }
    xsc->Release();
  }

  for (int i = 0; i < 5; ++i) {
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(128, 128, 128), 1.0f, 0);
    hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
  }
  std::printf("Present: 0x%08lx\n", static_cast<unsigned long>(hr));

  if (swapChain)
    swapChain->Release();
  dev->Release();
  d3d->Release();
  DestroyWindow(hwnd);
  return status;
}
