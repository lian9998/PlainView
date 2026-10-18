// dialogutil.cpp
#include "dialogutil.h"

#include <windowsx.h>

void ScaleDialogToDpi(HWND dialog, HFONT* font) {
    // The dialog manager lays a template out at the *system* DPI, so the only
    // scaling still to do is the difference between the system DPI and the monitor
    // the dialog was opened on. Comparing against 96 dpi would scale twice on a
    // high-dpi system (a 200% machine produced a 400% dialog).
    const UINT monitorDpi = GetDpiForWindow(dialog);
    const UINT systemDpi = GetDpiForSystem();
    if (monitorDpi == 0 || systemDpi == 0 || monitorDpi == systemDpi) return;
    const double scale = static_cast<double>(monitorDpi) / systemDpi;

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
        HFONT scaled = CreateFontIndirectW(&logFont);
        if (scaled) {
            EnumChildWindows(
                dialog,
                [](HWND child, LPARAM param) -> BOOL {
                    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(scaled));
            if (font) *font = scaled;
            else DeleteObject(scaled);
        }
    }
}

void CenterDialogOverOwner(HWND dialog, HWND owner) {
    RECT dialogRect{}, ownerRect{};
    GetWindowRect(dialog, &dialogRect);
    if (!owner || !GetWindowRect(owner, &ownerRect)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    }
    const int width = dialogRect.right - dialogRect.left;
    const int height = dialogRect.bottom - dialogRect.top;
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
