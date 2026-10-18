// colordialog.cpp — the native Windows colour dialog.
//
// Alpha is deliberately not offered: the background is opaque, so COLORREF's 24-bit
// RGB is exactly what the viewer needs.
#include "colordialog.h"

#include <commdlg.h>

#include <algorithm>
#include <cmath>

namespace {

BYTE ToByte(float value) {
    return static_cast<BYTE>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

bool ShowBackgroundColorDialog(HWND parent, HINSTANCE, D2D1_COLOR_F& color) {
    static COLORREF customColors[16] = {};

    CHOOSECOLORW choose{};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = parent;
    choose.rgbResult = RGB(ToByte(color.r), ToByte(color.g), ToByte(color.b));
    choose.lpCustColors = customColors;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&choose)) return false;

    color = D2D1::ColorF(GetRValue(choose.rgbResult) / 255.0f,
                         GetGValue(choose.rgbResult) / 255.0f,
                         GetBValue(choose.rgbResult) / 255.0f, 1.0f);
    return true;
}
