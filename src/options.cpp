// options.cpp — Options dialog (zoom speed, background color, initial sizing).
#include "options.h"

#include "resource.h"

#include <d2d1helper.h>

#include <commdlg.h>
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
    swprintf_s(buffer, L"#%02X%02X%02X", channel(color.r), channel(color.g), channel(color.b));
    return buffer;
}

COLORREF ToColorRef(const D2D1_COLOR_F& color) {
    auto channel = [](float value) {
        return static_cast<BYTE>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return RGB(channel(color.r), channel(color.g), channel(color.b));
}

D2D1_COLOR_F FromColorRef(COLORREF color) {
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                        GetBValue(color) / 255.0f, 1.0f);
}

// The dialog template is authored at 96 DPI. Per-monitor v2 processes do not get
// automatic dialog scaling, so scale every control once, when the dialog opens.
void ScaleDialogToDpi(HWND dialog, OptionsContext* context) {
    const UINT dpi = GetDpiForWindow(dialog);
    if (dpi == 0 || dpi == 96) return;
    const double scale = dpi / 96.0;

    RECT dialogRect{};
    GetWindowRect(dialog, &dialogRect);
    SetWindowPos(dialog, nullptr, 0, 0,
                 static_cast<int>((dialogRect.right - dialogRect.left) * scale),
                 static_cast<int>((dialogRect.bottom - dialogRect.top) * scale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    struct ScaleData {
        double scale;
        HWND dialog;
    } data{scale, dialog};

    EnumChildWindows(
        dialog,
        [](HWND child, LPARAM param) -> BOOL {
            auto* data = reinterpret_cast<ScaleData*>(param);
            RECT rect{};
            GetWindowRect(child, &rect);
            MapWindowPoints(HWND_DESKTOP, data->dialog, reinterpret_cast<POINT*>(&rect), 2);
            SetWindowPos(child, nullptr, static_cast<int>(rect.left * data->scale),
                         static_cast<int>(rect.top * data->scale),
                         static_cast<int>((rect.right - rect.left) * data->scale),
                         static_cast<int>((rect.bottom - rect.top) * data->scale),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&data));

    const HFONT baseFont = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
    LOGFONTW logFont{};
    if (baseFont && GetObjectW(baseFont, sizeof(logFont), &logFont) == sizeof(logFont)) {
        logFont.lfHeight = static_cast<LONG>(logFont.lfHeight * scale);
        if (logFont.lfWidth) logFont.lfWidth = static_cast<LONG>(logFont.lfWidth * scale);
        context->scaledFont = CreateFontIndirectW(&logFont);
        if (context->scaledFont) {
            EnumChildWindows(
                dialog,
                [](HWND child, LPARAM param) -> BOOL {
                    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(context->scaledFont));
        }
    }
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

            HWND parent = GetParent(dialog);
            ScaleDialogToDpi(dialog, context);

            RECT dialogRect{}, parentRect{};
            GetWindowRect(dialog, &dialogRect);
            if (parent && GetWindowRect(parent, &parentRect)) {
                const int width = dialogRect.right - dialogRect.left;
                const int height = dialogRect.bottom - dialogRect.top;
                const int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
                const int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
                SetWindowPos(dialog, nullptr, x, y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_BGCHOOSE: {
                    static COLORREF customColors[16] = {};
                    CHOOSECOLORW choose{};
                    choose.lStructSize = sizeof(choose);
                    choose.hwndOwner = dialog;
                    choose.rgbResult = ToColorRef(context->settings.background);
                    choose.lpCustColors = customColors;
                    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
                    if (ChooseColorW(&choose)) {
                        context->settings.background = FromColorRef(choose.rgbResult);
                        SetDlgItemTextW(dialog, IDC_BGHEX,
                                        FormatColorHex(context->settings.background).c_str());
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
