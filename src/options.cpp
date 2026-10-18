// options.cpp — Options dialog: zoom speed, background colour, session-cache
// budget, prefetch depth and initial sizing.
#include "options.h"

#include "resource.h"

#include "cache.h"
#include "colordialog.h"
#include "dialogutil.h"

#include <commctrl.h>
#include <d2d1helper.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

// Cache budgets offered by the slider. Each stop is 1.5x the previous one, so the
// slider is logarithmic in bytes and every second stop is a round power of two:
// 16 MB, 24 MB, 32 MB, ... 96 GB, 128 GB. Editing settings.json by hand still
// accepts any value in between; the slider simply snaps to the nearest stop.
constexpr int kCacheStops[] = {
    16,    24,    32,    48,    64,    96,    128,   192,   256,   384,
    512,   768,   1024,  1536,  2048,  3072,  4096,  6144,  8192,  12288,
    16384, 24576, 32768, 49152, 65536, 98304, 131072,
};
constexpr int kCacheStopCount = ARRAYSIZE(kCacheStops);
constexpr int kDefaultCacheStop = 8;  // 256 MB

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

std::wstring FormatMegabytes(int megabytes) {
    if (megabytes >= 1024) {
        if (megabytes % 1024 == 0) return std::to_wstring(megabytes / 1024) + L" GB";
        wchar_t buffer[32];
        swprintf_s(buffer, L"%.1f GB", megabytes / 1024.0);
        return buffer;
    }
    return std::to_wstring(megabytes) + L" MB";
}

// Nearest stop in logarithmic distance, so 500 MB lands on 512 MB and not on 384 MB.
int NearestCacheStop(int megabytes) {
    if (megabytes <= 0) return kDefaultCacheStop;
    int best = 0;
    double bestDistance = 1e9;
    for (int i = 0; i < kCacheStopCount; ++i) {
        const double distance =
            std::fabs(std::log(static_cast<double>(megabytes) / kCacheStops[i]));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

// Roughly what one cached image of this window costs: the exact size is decided per
// image, but the preview is scaled to the window's longest edge.
size_t EntryBytesForWindow(HWND window) {
    RECT client{};
    if (!window || !GetClientRect(window, &client)) return 0;
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return 0;

    const UINT edge = PreviewEdgeForWindow(static_cast<UINT>(width), static_cast<UINT>(height));
    const double scale = std::min(1.0, static_cast<double>(edge) / std::max(width, height));
    const double pixels = std::floor(width * scale) * std::floor(height * scale);
    return static_cast<size_t>(pixels) * 4;
}

void UpdateCacheUi(HWND dialog, OptionsContext* context) {
    (void)context;
    const bool unlimited = IsDlgButtonChecked(dialog, IDC_CACHEUNLIMITED) == BST_CHECKED;
    const HWND slider = GetDlgItem(dialog, IDC_CACHE);
    EnableWindow(slider, !unlimited);

    int stop = 0;
    if (slider) stop = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    stop = std::clamp(stop, 0, kCacheStopCount - 1);
    const int megabytes = kCacheStops[stop];

    SetDlgItemTextW(dialog, IDC_CACHELABEL,
                    unlimited ? L"No limit" : FormatMegabytes(megabytes).c_str());

    std::wstring hint;
    if (unlimited) {
        hint = L"Memory use is left to Windows.";
    } else if (const size_t entry = EntryBytesForWindow(GetParent(dialog)); entry > 0) {
        const size_t entries = static_cast<size_t>(megabytes) * 1024 * 1024 / entry;
        if (entries == 0) {
            hint = L"Smaller than one screen-sized image.";
        } else {
            hint = L"about " + std::to_wstring(entries) + L" screen-sized images";
        }
    }
    SetDlgItemTextW(dialog, IDC_CACHEHINT, hint.c_str());
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
            SetDlgItemInt(dialog, IDC_PREFETCH, static_cast<UINT>(context->settings.prefetchDepth),
                          FALSE);

            const bool unlimited =
                context->settings.cacheBudgetMB == kCacheBudgetUnlimitedMB;
            CheckDlgButton(dialog, IDC_CACHEUNLIMITED,
                           unlimited ? BST_CHECKED : BST_UNCHECKED);
            if (const HWND slider = GetDlgItem(dialog, IDC_CACHE)) {
                SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kCacheStopCount - 1));
                SendMessageW(slider, TBM_SETPAGESIZE, 0, 4);
                SendMessageW(slider, TBM_SETPOS, TRUE,
                             NearestCacheStop(unlimited ? 0 : context->settings.cacheBudgetMB));
            }

            ScaleDialogToDpi(dialog, &context->scaledFont);
            CenterDialogOverOwner(dialog, GetParent(dialog));
            UpdateCacheUi(dialog, context);
            return TRUE;
        }

        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == GetDlgItem(dialog, IDC_CACHE)) {
                UpdateCacheUi(dialog, context);
                return TRUE;
            }
            return FALSE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_CACHEUNLIMITED:
                    UpdateCacheUi(dialog, context);
                    return TRUE;
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
                    BOOL numberOk = FALSE;
                    const UINT prefetch = GetDlgItemInt(dialog, IDC_PREFETCH, &numberOk, FALSE);
                    if (!numberOk || prefetch > 20) {
                        MessageBoxW(dialog, L"Prefetch must be a number between 0 and 20.",
                                    L"Options", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(dialog, IDC_PREFETCH));
                        return TRUE;
                    }

                    const bool unlimited =
                        IsDlgButtonChecked(dialog, IDC_CACHEUNLIMITED) == BST_CHECKED;
                    const int stop = std::clamp(
                        static_cast<int>(
                            SendDlgItemMessageW(dialog, IDC_CACHE, TBM_GETPOS, 0, 0)),
                        0, kCacheStopCount - 1);

                    context->settings.zoomStep = value;
                    context->settings.prefetchDepth = static_cast<int>(prefetch);
                    context->settings.cacheBudgetMB =
                        unlimited ? kCacheBudgetUnlimitedMB : kCacheStops[stop];
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
