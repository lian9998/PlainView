// options.h
#pragma once

#include "common.h"
#include "settings.h"

// Shows the modal Options dialog. Returns true when the user accepted changes;
// `settings` is updated in place.
bool ShowOptionsDialog(HWND parent, HINSTANCE instance, AppSettings& settings);
