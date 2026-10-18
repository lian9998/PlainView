// register.h — per-user file-handler registration.
//
// Everything is written under HKEY_CURRENT_USER: no administrator rights, no
// machine-wide changes, and nothing that alters the user's existing default
// associations. Registration makes PlainView show up in Explorer's "Open with"
// list and in Settings > Apps > Default apps, where the user can make it default.
#pragma once

#include "common.h"

#include <vector>

// Extensions the Windows Imaging Component decodes out of the box (plus WebP,
// which ships as an inbox extension on Windows 11). HEIC/AVIF are deliberately
// absent: they need paid Store extensions, and claiming them would offer
// PlainView for files it usually cannot open.
std::vector<std::wstring> SupportedImageExtensions();

bool RegisterImageHandler(std::wstring* error);
bool UnregisterImageHandler(std::wstring* error);
// True when the stored command line points at this executable.
bool IsImageHandlerRegistered();
