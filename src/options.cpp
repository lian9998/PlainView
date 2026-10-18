// options.cpp — Options dialog (zoom speed, background color, initial sizing).
#include "options.h"

#include "resource.h"

#include "colordialog.h"
#include "dialogutil.h"

#include <d2d1helper.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct OptionsContext {
    AppSettings settings;
    HFONT scaledFont = nullptr;
};

std::wstring FormatZoomStep(double value) {
    wchar_t buffer[32];
    swprintf_s(buffer, L"%.2f", value);
    std::wstring text = buffer;
    while (text.size() > 1 && text.back() == L'0') text.pop_back();
    if (!text.empty() && text.back() == L'.') text.pop_back();
    return text;
}

std::wstring FormatColorHex(const D2D1_COLOR_F& color) {
    auto channel = [](float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    wchar_t buffer[16];
    if (channel(color.a) < 255) {
        swprintf_s(buffer, L"#%02X%02X%02X%02X", channel(color.a), channel(color.r),
                   channel(color.g), channel(color.b));
    } else {
        swprintf_s(buffer, L"#%02X%02X%02X", channel(color.r), channel(color.g), channel(color.b));
    }
    return buffer;
}

INT_PTR CALLBACK OptionsProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<OptionsContext*>(GetWindowLongPtrW(dialog, DWLP_USER));

    switch (message) {
        case WM_INITDIALOG: {
            context = reinterpret_cast<OptionsContext*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));

            SetDlgItemTextW(dialog, IDC_ZOOMSTEP, FormatZoomStep(context->settings.zoomStep).c_str());
            CheckRadioButton(dialog, IDC_SIZING_FIT, IDC_SIZING_ACTUAL,
                             context->settings.initialSizing == AppSettings::InitialSizing::Fit
                                 ? IDC_SIZING_FIT
                                 : IDC_SIZING_ACTUAL);
            SetDlgItemTextW(dialog, IDC_BGHEX, FormatColorHex(context->settings.background).c_str());

            ScaleDialogToDpi(dialog, &context->scaledFont);
            CenterDialogOverOwner(dialog, GetParent(dialog));
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_BGCHOOSE: {
                    D2D1_COLOR_F color = context->settings.background;
                    if (ShowBackgroundColorDialog(dialog, GetModuleHandleW(nullptr), color)) {
                        context->settings.background = color;
                        SetDlgItemTextW(dialog, IDC_BGHEX, FormatColorHex(color).c_str());
                    }
                    return TRUE;
                }
                case IDOK: {
                    wchar_t text[32] = {};
                    GetDlgItemTextW(dialog, IDC_ZOOMSTEP, text, ARRAYSIZE(text));
                    wchar_t* end = nullptr;
                    const double value = wcstod(text, &end);
                    if (end == text || !std::isfinite(value) || value < 1.01 || value > 3.0) {
                        MessageBoxW(dialog,
                                    L"Zoom per wheel notch must be a number between 1.01 and 3.00.",
                                    L"Options", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(dialog, IDC_ZOOMSTEP));
                        return TRUE;
                    }
                    context->settings.zoomStep = value;
                    context->settings.initialSizing =
                        IsDlgButtonChecked(dialog, IDC_SIZING_FIT) == BST_CHECKED
                            ? AppSettings::InitialSizing::Fit
                            : AppSettings::InitialSizing::Actual;
                    EndDialog(dialog, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dialog, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            return FALSE;

        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;

        case WM_DESTROY:
            if (context && context->scaledFont) {
                DeleteObject(context->scaledFont);
                context->scaledFont = nullptr;
            }
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

}  // namespace

bool ShowOptionsDialog(HWND parent, HINSTANCE instance, AppSettings& settings) {
    OptionsContext context;
    context.settings = settings;

    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_OPTIONS), parent,
                                           OptionsProc, reinterpret_cast<LPARAM>(&context));
    if (result != IDOK) return false;
    settings = context.settings;
    return true;
}
