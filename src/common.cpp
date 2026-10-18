// common.cpp
#include "common.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <cstdio>

std::wstring HResultMessage(HRESULT hr) {
    wchar_t* buffer = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, static_cast<DWORD>(hr), 0,
                             reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (n && buffer) {
        text.assign(buffer, n);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
            text.pop_back();
        }
    } else {
        wchar_t raw[64];
        swprintf_s(raw, L"0x%08X", static_cast<unsigned>(hr));
        text = raw;
    }
    if (buffer) LocalFree(buffer);
    return text;
}

std::wstring GetExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0) return std::wstring();
    std::wstring full(path, n);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return full.substr(0, slash);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf) {
    if (dir.empty()) return leaf;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    out += leaf;
    return out;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

bool IsFileExisting(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsDirectoryExisting(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring LocalAppDataDirectory() {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        result = raw;
        CoTaskMemFree(raw);
    }
    return result;
}

double NowMs() {
    static const double kPeriodMs = [] {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return 1000.0 / static_cast<double>(freq.QuadPart);
    }();
    static const LONGLONG kStart = [] {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - kStart) * kPeriodMs;
}
