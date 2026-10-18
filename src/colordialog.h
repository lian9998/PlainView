// colordialog.h — RGBA background picker (the common colour dialog has no alpha).
#pragma once

#include "common.h"

#include <d2d1.h>

bool ShowBackgroundColorDialog(HWND parent, HINSTANCE instance, D2D1_COLOR_F& color);
