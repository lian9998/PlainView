// colordialog.cpp — background colour: the native Windows colour picker for RGB
// (COLORREF has no alpha channel) followed by a small alpha slider popup.
#include "colordialog.h"

#include "dialogutil.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {

BYTE ToByte(float value) {
    return static_cast<BYTE>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

// The background is composited over black, and the preview shows exactly that, so
// the alpha slider has a visible, predictable effect.
COLORREF CompositeOverBlack(const D2D1_COLOR_F& color) {
    const float alpha = std::clamp(color.a, 0.0f, 1.0f);
    return RGB(ToByte(color.r * alpha), ToByte(color.g * alpha), ToByte(color.b * alpha));
}

D2D1_COLOR_F FromColorRef(COLORREF rgb, float alpha) {
    return D2D1::ColorF(GetRValue(rgb) / 255.0f, GetGValue(rgb) / 255.0f, GetBValue(rgb) / 255.0f,
                        alpha);
}

COLORREF ToColorRef(const D2D1_COLOR_F& color) {
    return RGB(ToByte(color.r), ToByte(color.g), ToByte(color.b));
}

struct AlphaContext {
    D2D1_COLOR_F color = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
    bool updating = false;
    HFONT font = nullptr;
    HBRUSH previewBrush = nullptr;
    COLORREF previewColor = RGB(0, 0, 0);
    HWND preview = nullptr;
};

void RefreshPreview(HWND dialog, AlphaContext* context) {
    if (context->previewBrush) DeleteObject(context->previewBrush);
    context->previewColor = CompositeOverBlack(context->color);
    context->previewBrush = CreateSolidBrush(context->previewColor);
    if (context->preview) InvalidateRect(context->preview, nullptr, TRUE);
    (void)dialog;
}

void ApplyAlpha(HWND dialog, AlphaContext* context) {
    context->updating = true;
    const int alpha = ToByte(context->color.a);
    SendDlgItemMessageW(dialog, IDC_BG_A, TBM_SETPOS, TRUE, alpha);
    SetDlgItemInt(dialog, IDC_BG_AEDIT, static_cast<UINT>(alpha), FALSE);
    context->updating = false;
    RefreshPreview(dialog, context);
}

INT_PTR CALLBACK AlphaProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<AlphaContext*>(GetWindowLongPtrW(dialog, DWLP_USER));

    switch (message) {
        case WM_INITDIALOG: {
            context = reinterpret_cast<AlphaContext*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
            context->preview = GetDlgItem(dialog, IDC_BG_PREVIEW);

            SendDlgItemMessageW(dialog, IDC_BG_A, TBM_SETRANGE, TRUE, MAKELONG(0, 255));
            SendDlgItemMessageW(dialog, IDC_BG_A, TBM_SETTICFREQ, 16, 0);
            ApplyAlpha(dialog, context);
            ScaleDialogToDpi(dialog, &context->font);
            CenterDialogOverOwner(dialog, GetParent(dialog));
            return TRUE;
        }

        case WM_HSCROLL:
            if (!context || context->updating) return TRUE;
            context->color.a =
                static_cast<float>(SendDlgItemMessageW(dialog, IDC_BG_A, TBM_GETPOS, 0, 0)) /
                255.0f;
            ApplyAlpha(dialog, context);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_BG_AEDIT: {
                    if (HIWORD(wParam) != EN_CHANGE || !context || context->updating) return TRUE;
                    BOOL ok = FALSE;
                    const UINT value = GetDlgItemInt(dialog, IDC_BG_AEDIT, &ok, FALSE);
                    if (!ok) return TRUE;
                    context->color.a = static_cast<float>(std::min<UINT>(value, 255)) / 255.0f;
                    context->updating = true;
                    SendDlgItemMessageW(dialog, IDC_BG_A, TBM_SETPOS, TRUE,
                                        std::min<UINT>(value, 255));
                    context->updating = false;
                    RefreshPreview(dialog, context);
                    return TRUE;
                }
                case IDOK:
                    EndDialog(dialog, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dialog, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            return FALSE;

        case WM_CTLCOLORSTATIC:
            if (context && reinterpret_cast<HWND>(lParam) == context->preview &&
                context->previewBrush) {
                SetBkColor(reinterpret_cast<HDC>(wParam), context->previewColor);
                return reinterpret_cast<INT_PTR>(context->previewBrush);
            }
            break;

        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;

        case WM_DESTROY:
            if (context) {
                if (context->previewBrush) {
                    DeleteObject(context->previewBrush);
                    context->previewBrush = nullptr;
                }
                if (context->font) {
                    DeleteObject(context->font);
                    context->font = nullptr;
                }
            }
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

}  // namespace

bool ShowBackgroundColorDialog(HWND parent, HINSTANCE instance, D2D1_COLOR_F& color) {
    static COLORREF customColors[16] = {};

    CHOOSECOLORW choose{};
    choose.lStructSize = sizeof(choose);
    choose.hwndOwner = parent;
    choose.rgbResult = ToColorRef(color);
    choose.lpCustColors = customColors;
    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&choose)) return false;

    // Alpha is not part of COLORREF, so it gets its own small popup.
    const float previousAlpha = color.a;
    AlphaContext context;
    context.color = FromColorRef(choose.rgbResult, previousAlpha);

    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_BACKGROUND), parent,
                                           AlphaProc, reinterpret_cast<LPARAM>(&context));
    if (result != IDOK) return false;

    color = context.color;
    return true;
}
