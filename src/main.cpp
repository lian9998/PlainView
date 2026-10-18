// main.cpp — PlainView entry point, window plumbing, bench and capture modes.
#include "common.h"
#include "decode.h"
#include "options.h"
#include "register.h"
#include "resource.h"
#include "settings.h"
#include "viewer.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <psapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr UINT_PTR kBenchTimer = 1;
constexpr UINT kBenchTimeoutMs = 20000;
constexpr UINT kHideMenuMessage = WM_APP + 3;

struct BenchRow {
    std::wstring file;
    bool success = false;
    double openMs = 0.0;
    double decodeMs = 0.0;
    double totalMs = 0.0;
    double firstFrameMs = 0.0;
    double readyMs = 0.0;
    bool fromCache = false;
    double requestMs = 0.0;
    double uploadMs = 0.0;
    SIZE_T peakWorkingSet = 0;
    bool tiled = false;
    bool colorManaged = false;
    UINT orientation = 1;
    UINT width = 0;
    UINT height = 0;
};

struct App {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HMENU menu = nullptr;       // owned; attached only while Alt is held
    bool menuVisible = false;
    Viewer viewer;
    AppSettings settings;

    bool bench = false;
    std::wstring reportPath;
    std::vector<std::wstring> benchFiles;
    size_t benchIndex = 0;
    std::vector<BenchRow> rows;
};

std::wstring FormatDouble(double value, int decimals = 1) {
    wchar_t buffer[64];
    swprintf_s(buffer, L"%.*f", decimals, value);
    return buffer;
}

SIZE_T CurrentPeakWorkingSet() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        return counters.PeakWorkingSetSize;
    }
    return 0;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok == TRUE && written == text.size();
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

void ShowOpenDialog(App& app) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }

    const COMDLG_FILTERSPEC filters[] = {
        {L"Images", L"*.jpg;*.jpeg;*.jpe;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.webp;*.heic;*.heif;*.avif;*.jxl;*.ico;*.wdp;*.jxr"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(L"Open image");

    if (FAILED(dialog->Show(app.hwnd))) return;

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        app.viewer.OpenFile(path);
        CoTaskMemFree(path);
    }
}

void ShowOptions(App& app) {
    AppSettings updated = app.settings;
    if (!ShowOptionsDialog(app.hwnd, app.instance, updated)) return;
    app.settings = updated;
    app.viewer.SetSettings(updated);
    app.viewer.RefreshBackground();
    SaveSettings(updated);
    if (app.hwnd) InvalidateRect(app.hwnd, nullptr, FALSE);
}

void RecordBenchRow(App& app, bool success) {
    KillTimer(app.hwnd, kBenchTimer);

    BenchRow row;
    row.file = app.benchFiles[app.benchIndex];
    const Viewer::Timing& timing = app.viewer.LastTiming();
    row.success = success && timing.success;
    row.openMs = timing.openMs;
    row.decodeMs = timing.decodeMs;
    row.totalMs = timing.totalMs;
    row.firstFrameMs = timing.firstFrameMs > 0.0 ? timing.firstFrameMs : NowMs();
    row.peakWorkingSet = CurrentPeakWorkingSet();
    row.tiled = timing.tiled;
    row.readyMs = timing.readyMs;
    row.requestMs = timing.requestMs;
    row.fromCache = timing.fromCache;
    row.uploadMs = timing.uploadMs;
    row.colorManaged = timing.colorManaged;
    row.orientation = timing.orientation;
    row.width = timing.width;
    row.height = timing.height;
    app.rows.push_back(std::move(row));

    ++app.benchIndex;
    if (app.benchIndex < app.benchFiles.size()) {
        SetTimer(app.hwnd, kBenchTimer, kBenchTimeoutMs, nullptr);
        app.viewer.OpenFile(app.benchFiles[app.benchIndex]);
    } else {
        PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
    }
}

void FinishBenchReport(App& app) {
    std::wstring text = L"PlainView benchmark\n";
    text += L"===================\n\n";
    for (const BenchRow& row : app.rows) {
        text += L"file            : " + row.file + L"\n";
        if (row.success) {
            text += L"image size      : " + std::to_wstring(row.width) + L" x " +
                    std::to_wstring(row.height) + L"  (exif orientation " +
                    std::to_wstring(row.orientation) + L", color managed " +
                    (row.colorManaged ? L"yes" : L"no") + L", " +
                    (row.tiled ? L"tiled" : L"single texture") + L")\n";
            text += L"decode worker   : open+metadata " + FormatDouble(row.openMs) +
                    L" ms, pixel decode " + FormatDouble(row.decodeMs) + L" ms, total " +
                    FormatDouble(row.totalMs) + L" ms\n";
            text += L"request issued  : " + FormatDouble(row.requestMs) +
                    L" ms after process start\n";
            if (row.fromCache) {
                text += L"source          : session cache (no decode)\n";
            }
            text += L"first frame     : " + FormatDouble(row.firstFrameMs) +
                    L" ms after process start\n";
            if (row.uploadMs > 0.0) {
                text += L"texture upload  : " + FormatDouble(row.uploadMs) + L" ms\n";
            }
        } else {
            text += L"result          : failed to decode (content area stays black)\n";
        }
        text += L"peak working set: " + FormatDouble(static_cast<double>(row.peakWorkingSet) /
                                                        (1024.0 * 1024.0), 1) +
                L" MB\n\n";
    }
    WriteTextFile(app.reportPath, ToUtf8(text));
}

// ---------------------------------------------------------------------------
// Offscreen capture (development aid: renders the real pipeline to a PNG)
// ---------------------------------------------------------------------------

bool SavePng(IWICImagingFactory* wic, IWICBitmapSource* source, const std::wstring& path) {
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(encoder->CreateNewFrame(&frame, &properties))) return false;
    if (FAILED(frame->Initialize(properties.Get()))) return false;

    UINT width = 0, height = 0;
    if (FAILED(source->GetSize(&width, &height))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(source, format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    if (FAILED(frame->WriteSource(converter.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

int RunCapture(const std::wstring& outputPath, const std::wstring& imagePath, UINT width,
               UINT height, double zoom, UINT tileThreshold) {
    ComPtr<ID2D1Factory> factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory)))) {
        return 2;
    }
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CreateWicFactory(&wic))) return 3;

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                 &bitmap))) {
        return 4;
    }

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);

    ComPtr<ID2D1RenderTarget> target;
    if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target))) {
        return 5;
    }

    AppSettings settings;
    LoadSettings(settings);

    Viewer viewer;
    viewer.SetSettings(settings);
    viewer.AttachOffscreenTarget(target.Get(), tileThreshold);
    if (!viewer.LoadBlocking(imagePath)) return 6;
    if (zoom > 0.0) viewer.SetZoom(zoom);

    const HRESULT drawResult = viewer.Draw(target.Get(), D2D1::SizeF(static_cast<float>(width),
                                                                    static_cast<float>(height)));
    if (FAILED(drawResult)) return 7;
    return SavePng(wic.Get(), bitmap.Get(), outputPath) ? 0 : 8;
}

// ---------------------------------------------------------------------------
// Registration UI
// ---------------------------------------------------------------------------

void ShowRegistrationMessage(HWND owner, bool ok, bool registered, const std::wstring& error) {
    std::wstring text;
    UINT flags = MB_OK | MB_ICONINFORMATION;

    if (!ok) {
        text = L"Registration failed.\n\n" + error;
        flags = MB_OK | MB_ICONERROR;
    } else if (registered) {
        text =
            L"PlainView is registered as an image viewer.\n\n"
            L"It now appears in Explorer's \"Open with\" menu for JPEG, PNG and the other "
            L"formats Windows can decode.\n\n"
            L"To make it the default, pick PlainView under Settings > Apps > Default apps.\n\n"
            L"Open that page now?";
        flags = MB_YESNO | MB_ICONINFORMATION;
    } else {
        text = L"PlainView's image viewer registration has been removed.";
    }

    const int answer = MessageBoxW(owner, text.c_str(), L"PlainView", flags);
    if (ok && registered && answer == IDYES) {
        // Windows 11 can open the page for this specific app; fall back otherwise.
        const HINSTANCE result =
            ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=PlainView",
                          nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr,
                          SW_SHOWNORMAL);
        }
    }
}

// ---------------------------------------------------------------------------
// Auto-hiding menu bar: hidden until Alt is pressed, hidden again once closed.
// ---------------------------------------------------------------------------

void ShowMenuBar(App& app) {
    if (app.menuVisible || !app.menu || !app.hwnd) return;
    SetMenu(app.hwnd, app.menu);
    app.menuVisible = true;
    DrawMenuBar(app.hwnd);
    // Hand control to the menu exactly as a plain Alt press would.
    SendMessageW(app.hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0);
}

void HideMenuBar(App& app) {
    if (!app.menuVisible || !app.hwnd) return;
    SetMenu(app.hwnd, nullptr);
    app.menuVisible = false;
    DrawMenuBar(app.hwnd);
}

// ---------------------------------------------------------------------------
// Window procedure and message loop
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = reinterpret_cast<App*>(create->lpCreateParams);
            app->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            if (app) app->viewer.OnPaint();
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (app) app->viewer.OnSize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (app) app->viewer.OnDpiChanged();
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            if (app) app->viewer.OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), point);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (app) {
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                app->viewer.OnLButtonDown(point);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (app) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                app->viewer.OnMouseMove(point);
            }
            return 0;
        case WM_LBUTTONUP:
            if (app) {
                ReleaseCapture();
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                app->viewer.OnLButtonUp(point);
            }
            return 0;
        case WM_KEYDOWN:
            if (app) app->viewer.OnKeyDown(static_cast<UINT>(wParam));
            return 0;
        case WM_DROPFILES: {
            auto drop = reinterpret_cast<HDROP>(wParam);
            if (app) app->viewer.OnDropFiles(drop);
            DragFinish(drop);
            return 0;
        }
        case WM_APP_DECODE:
            if (app) app->viewer.OnDecodeResult(reinterpret_cast<DecodeResult*>(lParam));
            return 0;
        case WM_APP_FOLDER:
            if (app) app->viewer.OnFolderList(reinterpret_cast<FolderList*>(lParam));
            return 0;
        case WM_COMMAND:
            if (!app) return 0;
            switch (LOWORD(wParam)) {
                case ID_FILE_OPEN:
                    ShowOpenDialog(*app);
                    return 0;
                case ID_FILE_OPTIONS:
                    ShowOptions(*app);
                    return 0;
                case ID_FILE_REGISTER: {
                    const bool registered = IsImageHandlerRegistered();
                    std::wstring error;
                    const bool ok =
                        registered ? UnregisterImageHandler(&error) : RegisterImageHandler(&error);
                    ShowRegistrationMessage(hwnd, ok, !registered, error);
                    return 0;
                }
                case ID_FILE_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    return 0;
            }
        case WM_INITMENUPOPUP: {
            // One menu entry that reflects (and toggles) the current state.
            HMENU menu = GetMenu(hwnd);
            if (menu && HIWORD(lParam) == FALSE && LOWORD(lParam) == 0) {
                HMENU fileMenu = GetSubMenu(menu, 0);
                if (fileMenu) {
                    ModifyMenuW(fileMenu, ID_FILE_REGISTER, MF_BYCOMMAND | MF_STRING,
                                ID_FILE_REGISTER,
                                IsImageHandlerRegistered()
                                    ? L"Remove Image Viewer Registration"
                                    : L"Register as Image Viewer...");
                }
            }
            return 0;
        }
        case WM_SYSKEYDOWN:
            if (app && wParam == VK_MENU) {
                ShowMenuBar(*app);
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_MENUSELECT:
            // The menu system signals "closed" with item 0xFFFF / flags 0xFFFF.
            if (app && HIWORD(lParam) == 0xFFFF && LOWORD(wParam) == 0xFFFF) {
                PostMessageW(hwnd, kHideMenuMessage, 0, 0);
            }
            return 0;
        case WM_UNINITMENUPOPUP:
            if (app) PostMessageW(hwnd, kHideMenuMessage, 0, 0);
            return 0;
        case kHideMenuMessage:
            if (app) HideMenuBar(*app);
            return 0;
        case WM_TIMER:
            if (app && app->bench && wParam == kBenchTimer) {
                RecordBenchRow(*app, false);
                return 0;
            }
            if (app && app->viewer.OnTimer(wParam)) return 0;
            return 0;
        case WM_CLOSE:
            if (app && app->bench) FinishBenchReport(*app);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (app && app->menu) {
                SetMenu(hwnd, nullptr);
                DestroyMenu(app->menu);
                app->menu = nullptr;
                app->menuVisible = false;
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool ParseSize(const std::wstring& text, UINT& width, UINT& height) {
    const size_t x = text.find_first_of(L"xX");
    if (x == std::wstring::npos) return false;
    width = static_cast<UINT>(wcstoul(text.substr(0, x).c_str(), nullptr, 10));
    height = static_cast<UINT>(wcstoul(text.substr(x + 1).c_str(), nullptr, 10));
    return width > 0 && height > 0;
}

void SetDpiAwareness() {
    // The manifest already declares PerMonitorV2; this covers manifest-less runs.
    using SetDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto* fn = reinterpret_cast<SetDpiAwarenessContextFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

int RunGui(App& app, int argc, wchar_t** argv) {
    SetDpiAwareness();
    LoadSettings(app.settings);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = app.instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        app.instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        app.instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    windowClass.lpszClassName = L"PlainViewWindow";
    if (!RegisterClassExW(&windowClass)) return 1;

    app.menu = LoadMenuW(app.instance, MAKEINTRESOURCEW(IDR_MAINMENU));
    HACCEL accelerators = LoadAcceleratorsW(app.instance, MAKEINTRESOURCEW(IDR_ACCEL));

    const UINT dpi = GetDpiForSystem();
    RECT rect{0, 0, static_cast<LONG>(1200.0 * dpi / 96.0), static_cast<LONG>(800.0 * dpi / 96.0)};
    // The menu bar is not attached at startup, so it must not be reserved here.
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);

    app.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, windowClass.lpszClassName, L"PlainView",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                               app.instance, &app);
    if (!app.hwnd) return 1;

    app.viewer.SetSettings(app.settings);
    if (!app.viewer.InitWorkers(app.hwnd)) return 1;
    DragAcceptFiles(app.hwnd, TRUE);

    // Request the image before creating the Direct2D device: the decode then runs
    // concurrently with GPU initialisation.
    if (app.bench) {
        app.viewer.SetImageReadyCallback([&app] { RecordBenchRow(app, true); });
        if (!app.benchFiles.empty()) {
            SetTimer(app.hwnd, kBenchTimer, kBenchTimeoutMs, nullptr);
            app.viewer.OpenFile(app.benchFiles.front());
        }
    } else {
        std::wstring initial;
        for (int i = 1; i < argc; ++i) {
            if (argv[i][0] != L'-') {
                initial = argv[i];
                break;
            }
        }
        if (!initial.empty() && IsFileExisting(initial)) app.viewer.OpenFile(initial);
    }

    if (!app.viewer.InitGraphics(app.hwnd)) return 1;
    ShowWindow(app.hwnd, SW_SHOW);
    UpdateWindow(app.hwnd);

    if (app.bench && app.benchFiles.empty()) PostMessageW(app.hwnd, WM_CLOSE, 0, 0);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!accelerators || !TranslateAcceleratorW(app.hwnd, accelerators, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    const double processStart = NowMs();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        CoUninitialize();
        return 1;
    }

    int exitCode = 0;
    bool handled = false;

    bool wantsRegister = false;
    bool wantsUnregister = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--register") == 0) wantsRegister = true;
        else if (_wcsicmp(argv[i], L"--unregister") == 0) wantsUnregister = true;
    }
    if (wantsRegister || wantsUnregister) {
        std::wstring error;
        const bool ok = wantsRegister ? RegisterImageHandler(&error) : UnregisterImageHandler(&error);
        ShowRegistrationMessage(nullptr, ok, wantsRegister, error);
        LocalFree(argv);
        CoUninitialize();
        return ok ? 0 : 1;
    }

    if (argc >= 4 && _wcsicmp(argv[1], L"--capture") == 0) {
        const std::wstring output = argv[2];
        const std::wstring image = argv[3];
        UINT width = 1600, height = 1000;
        double zoom = 0.0;
        UINT tileThreshold = 0;
        for (int i = 4; i + 1 < argc; ++i) {
            if (_wcsicmp(argv[i], L"--size") == 0) ParseSize(argv[i + 1], width, height);
            else if (_wcsicmp(argv[i], L"--zoom") == 0) zoom = wcstod(argv[i + 1], nullptr);
            else if (_wcsicmp(argv[i], L"--tile-threshold") == 0)
                tileThreshold = static_cast<UINT>(wcstoul(argv[i + 1], nullptr, 10));
        }
        exitCode = RunCapture(output, image, width, height, zoom, tileThreshold);
        handled = true;
    } else if (argc >= 4 && _wcsicmp(argv[1], L"--bench") == 0) {
        App app;
        app.instance = instance;
        app.bench = true;
        app.reportPath = argv[2];
        for (int i = 3; i < argc; ++i) app.benchFiles.emplace_back(argv[i]);
        exitCode = RunGui(app, argc, argv);
        (void)processStart;
        handled = true;
    }

    if (!handled) {
        App app;
        app.instance = instance;
        exitCode = RunGui(app, argc, argv);
    }

    LocalFree(argv);
    CoUninitialize();
    return exitCode;
}
