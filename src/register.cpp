// register.cpp
#include "register.h"

#include <algorithm>
#include <string>

namespace {

constexpr wchar_t kProgId[] = L"PlainView.Image";
constexpr wchar_t kApplicationName[] = L"PlainView";
constexpr wchar_t kCapabilitiesPath[] = L"Software\\PlainView\\Capabilities";
constexpr wchar_t kClassesRoot[] = L"Software\\Classes\\";

const wchar_t* const kExtensions[] = {
    L".jpg", L".jpeg", L".jpe", L".jfif", L".png",  L".bmp", L".dib",
    L".gif", L".tif",  L".tiff", L".ico",  L".webp", L".jxr", L".wdp",
};

std::wstring ExecutablePath() {
    wchar_t buffer[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    if (length == 0 || length >= ARRAYSIZE(buffer)) return std::wstring();
    return std::wstring(buffer, length);
}

std::wstring OpenCommand() { return L"\"" + ExecutablePath() + L"\" \"%1\""; }

std::wstring IconSpec() { return L"\"" + ExecutablePath() + L"\",0"; }

std::wstring ErrorText(LONG code) {
    wchar_t* buffer = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(code), 0,
                       reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (length && buffer) {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
            text.pop_back();
        }
    }
    if (buffer) LocalFree(buffer);
    if (text.empty()) {
        wchar_t fallback[32];
        swprintf_s(fallback, L"error %ld", code);
        text = fallback;
    }
    return text;
}

bool SetString(const std::wstring& subkey, const wchar_t* name, const std::wstring& value,
               LONG* errorCode) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                              &key, nullptr);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (rc != ERROR_SUCCESS && errorCode) *errorCode = rc;
    return rc == ERROR_SUCCESS;
}

bool SetEmptyValue(const std::wstring& subkey, const wchar_t* name, DWORD type, LONG* errorCode) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                              &key, nullptr);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(key, name, 0, type, nullptr, 0);
        RegCloseKey(key);
    }
    if (rc != ERROR_SUCCESS && errorCode) *errorCode = rc;
    return rc == ERROR_SUCCESS;
}

bool DeleteTree(const std::wstring& subkey) {
    const LONG rc = RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

bool DeleteValue(const std::wstring& subkey, const wchar_t* name) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_SET_VALUE, &key);
    if (rc == ERROR_FILE_NOT_FOUND) return true;
    if (rc == ERROR_SUCCESS) {
        rc = RegDeleteValueW(key, name);
        RegCloseKey(key);
        if (rc == ERROR_FILE_NOT_FOUND) return true;
    }
    return rc == ERROR_SUCCESS;
}

bool ReadString(const std::wstring& subkey, const wchar_t* name, std::wstring& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LONG rc = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (rc == ERROR_SUCCESS && type == REG_SZ && bytes > 0) {
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        rc = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()),
                              &bytes);
        if (rc == ERROR_SUCCESS) {
            value.resize(wcslen(value.c_str()));
            out = value;
        }
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

}  // namespace

std::vector<std::wstring> SupportedImageExtensions() {
    return std::vector<std::wstring>(std::begin(kExtensions), std::end(kExtensions));
}

bool RegisterImageHandler(std::wstring* error) {
    const std::wstring exe = ExecutablePath();
    if (exe.empty()) {
        if (error) *error = L"The path of PlainView.exe could not be determined.";
        return false;
    }

    LONG code = ERROR_SUCCESS;
    const std::wstring classes = kClassesRoot;
    const std::wstring progIdKey = classes + kProgId;
    const std::wstring appKey = classes + L"Applications\\" + FileNameOf(exe);

    auto fail = [&](const wchar_t* what) {
        if (error) {
            *error = std::wstring(what) + L": " + ErrorText(code);
        }
        return false;
    };

    // ProgID: how the file type is described, displayed and opened.
    if (!SetString(progIdKey, nullptr, L"PlainView image", &code)) return fail(L"ProgID");
    if (!SetString(progIdKey + L"\\DefaultIcon", nullptr, IconSpec(), &code)) return fail(L"Icon");
    if (!SetString(progIdKey + L"\\shell\\open\\command", nullptr, OpenCommand(), &code)) {
        return fail(L"Open command");
    }
    if (!SetString(progIdKey + L"\\Application", L"ApplicationName", kApplicationName, &code)) {
        return fail(L"Application name");
    }
    if (!SetString(progIdKey + L"\\Application", L"ApplicationIcon", IconSpec(), &code)) {
        return fail(L"Application icon");
    }

    // Applications\PlainView.exe — this is what puts the entry in "Open with".
    if (!SetString(appKey, L"FriendlyAppName", kApplicationName, &code)) return fail(L"App name");
    if (!SetString(appKey + L"\\DefaultIcon", nullptr, IconSpec(), &code)) return fail(L"App icon");
    if (!SetString(appKey + L"\\shell\\open\\command", nullptr, OpenCommand(), &code)) {
        return fail(L"App command");
    }

    // RegisteredApplications + Capabilities — makes the app appear in
    // Settings > Apps > Default apps (the only place a default may be chosen).
    if (!SetString(kCapabilitiesPath, L"ApplicationName", kApplicationName, &code)) {
        return fail(L"Capabilities");
    }
    if (!SetString(kCapabilitiesPath, L"ApplicationDescription",
                   L"A fast, minimal image viewer", &code)) {
        return fail(L"Capabilities description");
    }

    const std::wstring openWithKeySuffix = L"\\OpenWithProgids";
    for (const wchar_t* extension : kExtensions) {
        const std::wstring ext = extension;
        const std::wstring capabilities = std::wstring(kCapabilitiesPath) + L"\\FileAssociations";
        if (!SetString(capabilities, ext.c_str(), kProgId, &code)) return fail(L"File association");
        if (!SetString(appKey + L"\\SupportedTypes", ext.c_str(), L"", &code)) {
            return fail(L"Supported types");
        }
        // Advertise the ProgID for the extension so "Open with" offers PlainView.
        if (!SetEmptyValue(classes + ext + openWithKeySuffix, kProgId, REG_NONE, &code)) {
            return fail(L"Open with list");
        }
    }

    if (!SetString(L"Software\\RegisteredApplications", kApplicationName,
                   L"Software\\PlainView\\Capabilities", &code)) {
        return fail(L"Registered applications");
    }

    return true;
}

bool UnregisterImageHandler(std::wstring* error) {
    if (ExecutablePath().empty()) {
        if (error) *error = L"The path of PlainView.exe could not be determined.";
        return false;
    }

    const std::wstring classes = kClassesRoot;
    bool ok = DeleteTree(classes + kProgId);
    ok = DeleteTree(classes + L"Applications\\" + FileNameOf(ExecutablePath())) && ok;
    ok = DeleteTree(L"Software\\PlainView") && ok;
    ok = DeleteValue(L"Software\\RegisteredApplications", kApplicationName) && ok;

    for (const wchar_t* extension : kExtensions) {
        HKEY key = nullptr;
        const std::wstring openWith = classes + extension + L"\\OpenWithProgids";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, openWith.c_str(), 0, KEY_SET_VALUE, &key) ==
            ERROR_SUCCESS) {
            RegDeleteValueW(key, kProgId);
            RegCloseKey(key);
        }
    }

    if (!ok && error) *error = L"Some registration entries could not be removed.";
    return ok;
}

bool IsImageHandlerRegistered() {
    const std::wstring exe = ExecutablePath();
    if (exe.empty()) return false;
    std::wstring command;
    const std::wstring appKey = std::wstring(kClassesRoot) + L"Applications\\" +
                                FileNameOf(exe) + L"\\shell\\open\\command";
    if (!ReadString(appKey, nullptr, command)) return false;
    return _wcsicmp(command.c_str(), OpenCommand().c_str()) == 0;
}
