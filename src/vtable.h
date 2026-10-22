// Vtable slots of the D3D9 interfaces the plugin hooks, in the declaration order of d3d9.h (tests/vtable_check.py
// checks them against the SDK header), and the patch helper.
#pragma once

namespace vt {

// IDirect3D9
constexpr int kCreateDevice = 16;

// IDirect3D9Ex
constexpr int kCreateDeviceEx = 20;

// IDirect3DDevice9
constexpr int kReset = 16;
constexpr int kPresent = 17;
constexpr int kSetGammaRamp = 21;
constexpr int kCreateTexture = 23;
constexpr int kCreateRenderTarget = 28;
constexpr int kUpdateSurface = 30;
constexpr int kGetRenderTargetData = 32;
constexpr int kStretchRect = 34;
constexpr int kColorFill = 35;
constexpr int kSetRenderTarget = 37;
constexpr int kSetDepthStencilSurface = 39;
constexpr int kBeginScene = 41;
constexpr int kEndScene = 42;
constexpr int kClear = 43;
constexpr int kDrawPrimitive = 81;
constexpr int kDrawIndexedPrimitive = 82;
constexpr int kDrawPrimitiveUP = 83;
constexpr int kDrawIndexedPrimitiveUP = 84;
constexpr int kCreateVertexShader = 91;
constexpr int kSetVertexShader = 92;
constexpr int kCreatePixelShader = 106;
constexpr int kSetPixelShader = 107;
constexpr int kSetPixelShaderConstantF = 109;

// IDirect3DSwapChain9
constexpr int kSwapChainPresent = 3;

// Replaces slot `index` of the COM object's vtable with `hook` and returns what was there. Every object of the class
// shares the vtable, so the hook covers all of them. Returns nullptr if the slot already holds `hook`.
void* patch(void* object, int index, void* hook);

}  // namespace vt
