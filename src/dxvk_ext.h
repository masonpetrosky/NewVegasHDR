// DXVK's public D3D9 extension interfaces (src/d3d9/d3d9_interfaces.h upstream), declared here with only what this
// project uses. The layouts and IIDs must match DXVK's; the Vulkan types are spelled out so that no Vulkan headers are
// needed.
#pragma once

#include <windows.h>
#include <unknwn.h>

namespace dxvk {

// VkColorSpaceKHR values
enum ColorSpace : int {
  kSrgbNonlinear = 0,             // VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
  kExtendedSrgbLinear = 1000104002,  // VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT (scRGB)
  kHdr10St2084 = 1000104008,      // VK_COLOR_SPACE_HDR10_ST2084_EXT
};

// VkHdrMetadataEXT
struct HdrMetadata {
  int sType = 1000105000;  // VK_STRUCTURE_TYPE_HDR_METADATA_EXT
  const void* pNext = nullptr;
  float redPrimary[2];
  float greenPrimary[2];
  float bluePrimary[2];
  float whitePoint[2];
  float maxLuminance;
  float minLuminance;
  float maxContentLightLevel;
  float maxFrameAverageLightLevel;
};

// D3D9VkExtOutputMetadata
struct OutputMetadata {
  float redPrimary[2];
  float greenPrimary[2];
  float bluePrimary[2];
  float whitePoint[2];
  float minLuminance;
  float maxLuminance;
  float maxFullFrameLuminance;
};

}  // namespace dxvk

// Queried from IDirect3D9. UnlockAdditionalFormats allows A16B16G16R16F back buffers; call it before CreateDevice.
MIDL_INTERFACE("65b55086-e3e3-4c3e-b3a0-86815cce2c4c")
ID3D9VkExtInterface : public IUnknown {
  virtual void STDMETHODCALLTYPE UnlockAdditionalFormats() = 0;
};

// Queried from IDirect3DSwapChain9.
MIDL_INTERFACE("13776e93-4aa9-430a-a4ec-fe9e281181d5")
ID3D9VkExtSwapchain : public IUnknown {
  virtual BOOL STDMETHODCALLTYPE CheckColorSpaceSupport(dxvk::ColorSpace colorSpace) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetColorSpace(dxvk::ColorSpace colorSpace) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetHDRMetaData(const dxvk::HdrMetadata* metadata) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentOutputDesc(dxvk::OutputMetadata* desc) = 0;
  virtual void STDMETHODCALLTYPE UnlockAdditionalFormats() = 0;  // deprecated upstream
};
