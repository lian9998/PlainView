// dialogutil.h — shared dialog helpers.
#pragma once

#include "common.h"

// The dialog templates are authored at 96 DPI. Per-monitor v2 processes are not
// scaled automatically, so dialogs scale themselves once on open.
// The created font is returned in `font` and must be deleted by the caller.
void ScaleDialogToDpi(HWND dialog, HFONT* font);

// Centres `dialog` over `owner` (or the screen when there is no owner).
void CenterDialogOverOwner(HWND dialog, HWND owner);
