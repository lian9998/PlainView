// shellicon.h — the shell's icon for a file, as a Direct2D bitmap.
#pragma once

#include "common.h"

#include <d2d1.h>

// Loads the jumbo (256 px) shell icon for `path`, falling back to the large icon.
// Used when a file cannot be decoded, so an unsupported file shows the icon it has
// in Explorer instead of an empty black window.
HRESULT CreateShellIconBitmap(ID2D1RenderTarget* target, const std::wstring& path,
                              ComPtr<ID2D1Bitmap>& bitmap, UINT* pixelSize);
