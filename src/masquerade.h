// What the game sees of the formats the plugin changed.
//
// The plugin gives the back buffer and the upgraded render targets 16-bit float formats, but the game was written for
// the 8-bit ones it asked for. Gamebryo looks the back buffer's format up among the formats it knows, and at the first
// start with the plugin it crashed on the empty entry it found for A16B16G16R16F. So each surface and texture whose
// format the plugin changed carries the game's format as private data, and every way the game reads a format reports
// that one: a surface's GetDesc, a texture's GetLevelDesc and the swap chain's GetPresentParameters. What depends on
// the real format goes through an 8-bit render target of the game's format: locks of such surfaces, and copies to and
// from system memory, which DXVK makes only between equal formats. The plugin's own calls (InternalScope) see the real
// formats.
#pragma once

#include <d3d9.h>

namespace masquerade {

// Hooks the methods that report formats or lock surfaces. All surfaces, all textures and all swap chains share their
// vtables, so this covers every one the game will create.
void install(IDirect3DDevice9* device);

// Records the format the game asked for on a resource whose format the plugin changed.
void tag(IDirect3DResource9* resource, D3DFORMAT shown);
// tag() for a texture and each of its levels.
void tagTexture(IDirect3DTexture9* texture, D3DFORMAT shown);
// tag() for every back buffer of the implicit swap chain.
void tagBackBuffers(IDirect3DDevice9* device, D3DFORMAT shown);

// The format the game asked for on a tagged resource, D3DFMT_UNKNOWN on any other.
D3DFORMAT shownFormat(IDirect3DResource9* resource);
// GetDesc past the hook.
HRESULT realDesc(IDirect3DSurface9* surface, D3DSURFACE_DESC* desc);

// For the device's GetRenderTargetData and UpdateSurface hooks, after DXVK refused a copy: if the copy failed only
// because the plugin changed a format, they make it through an 8-bit render target, put its result in `hr` and return
// true. `caller` is the game's return address, for the log.
bool getRenderTargetData(IDirect3DDevice9* device, IDirect3DSurface9* source, IDirect3DSurface9* dest, HRESULT& hr,
                         const void* caller);
bool updateSurface(IDirect3DDevice9* device, IDirect3DSurface9* source, const RECT* rect, IDirect3DSurface9* dest,
                   const POINT* point, HRESULT& hr, const void* caller);

// Releases the surfaces the conversions go through (they are in the default pool: before a Reset).
void releaseResources();

}  // namespace masquerade
