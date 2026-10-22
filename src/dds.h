// Writes a locked surface to a .dds file for offline analysis (tools/capture.py reads them).
#pragma once

#include <windows.h>
#include <d3d9.h>

#include <string>

// Bytes per pixel of the formats the writer handles, 0 for the rest.
UINT ddsBytesPerPixel(D3DFORMAT format);

bool writeDds(const std::wstring& path, UINT width, UINT height, D3DFORMAT format, const void* data, UINT pitch);
