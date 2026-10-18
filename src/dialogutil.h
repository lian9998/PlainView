// dialogutil.h — shared dialog helpers.
#pragma once

#include "common.h"

// The dialog manager creates a template at the system DPI. When the dialog opens on
// a monitor with a different DPI, this rescales it (and its font) by that ratio.
// The created font is returned in `font` and must be deleted by the caller.
void ScaleDialogToDpi(HWND dialog, HFONT* font);

// Centres `dialog` over `owner` (or the screen when there is no owner).
void CenterDialogOverOwner(HWND dialog, HWND owner);
