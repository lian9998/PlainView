// colordialog.h — background colour picker (opaque RGB only).
#pragma once

#include "common.h"

#include <d2d1.h>

bool ShowBackgroundColorDialog(HWND parent, HINSTANCE instance, D2D1_COLOR_F& color);
