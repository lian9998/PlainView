// common.h — shared Windows helpers and app-wide message ids.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Posted to the main window by worker threads.
#define WM_APP_DECODE (WM_APP + 1)   // lParam = DecodeResult*
#define WM_APP_FOLDER (WM_APP + 2)   // lParam = FolderList*

std::wstring HResultMessage(HRESULT hr);
std::wstring GetExeDirectory();
std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf);
std::wstring FileNameOf(const std::wstring& path);
std::wstring DirectoryOf(const std::wstring& path);
bool IsFileExisting(const std::wstring& path);
bool IsDirectoryExisting(const std::wstring& path);
std::wstring LocalAppDataDirectory();
double NowMs();  // monotonic, milliseconds since process start (QPC based)

// Frees a COM pointer-ish handle at scope exit.
template <typename Fn>
struct ScopeExit {
    Fn fn;
    ~ScopeExit() { fn(); }
};
template <typename Fn>
ScopeExit<Fn> MakeScopeExit(Fn fn) {
    return ScopeExit<Fn>{fn};
}
