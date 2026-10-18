// settings.cpp — a deliberately small, dependency-free reader/writer for the flat
// settings.json object: { "zoomStep": 1.15, "backgroundColor": "#000000",
// "initialSizing": "fit" }.
#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 1 << 20) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() || ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read,
                                            nullptr);
    CloseHandle(file);
    return ok == TRUE && read == out.size();
}

// Returns the raw text of a top-level member value ("1.15", "\"#000000\"", "true").
bool FindMember(const std::string& json, const char* key, std::string& rawValue) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;

    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (pos < json.size() && isSpace(json[pos])) ++pos;
    if (pos >= json.size()) return false;

    const size_t start = pos;
    if (json[pos] == '"') {
        ++pos;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\') ++pos;
            ++pos;
        }
        if (pos >= json.size()) return false;
        ++pos;  // closing quote
    } else {
        while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && !isSpace(json[pos])) {
            ++pos;
        }
    }
    rawValue = json.substr(start, pos - start);
    return true;
}

bool ParseNumber(const std::string& raw, double& out) {
    try {
        size_t used = 0;
        const double value = std::stod(raw, &used);
        if (used == 0 || !std::isfinite(value)) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseString(const std::string& raw, std::string& out) {
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return false;
    out = raw.substr(1, raw.size() - 2);
    return true;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseColor(const std::string& text, D2D1_COLOR_F& out) {
    // #RRGGBB or #AARRGGBB
    if (text.empty() || text[0] != '#') return false;
    if (text.size() != 7 && text.size() != 9) return false;

    auto byteAt = [&text](size_t offset, unsigned& value) {
        const int hi = HexDigit(text[offset]);
        const int lo = HexDigit(text[offset + 1]);
        if (hi < 0 || lo < 0) return false;
        value = static_cast<unsigned>(hi * 16 + lo);
        return true;
    };

    unsigned r = 0, g = 0, b = 0;
    // An old #AARRGGBB value is still accepted, but the alpha byte is ignored:
    // backgrounds are opaque.
    const size_t base = text.size() == 9 ? 3 : 1;
    if (!byteAt(base, r) || !byteAt(base + 2, g) || !byteAt(base + 4, b)) return false;

    out = D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    return true;
}

std::string FormatColor(const D2D1_COLOR_F& color) {
    auto channel = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", channel(color.r), channel(color.g),
                  channel(color.b));
    return buffer;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok == TRUE && written == text.size();
}

}  // namespace

std::wstring SettingsDirectory() {
    const std::wstring base = LocalAppDataDirectory();
    if (base.empty()) return std::wstring();
    return JoinPath(base, L"PlainView");
}

std::wstring SettingsPath() {
    const std::wstring dir = SettingsDirectory();
    return dir.empty() ? std::wstring() : JoinPath(dir, L"settings.json");
}

bool LoadSettings(AppSettings& out) {
    const std::wstring path = SettingsPath();
    if (path.empty()) return false;

    std::string json;
    if (!ReadFileUtf8(path, json)) return false;

    std::string raw;
    double number = 0.0;
    if (FindMember(json, "zoomStep", raw) && ParseNumber(raw, number)) {
        if (number >= 1.01 && number <= 3.0) out.zoomStep = number;
    }
    if (FindMember(json, "backgroundColor", raw)) {
        std::string text;
        D2D1_COLOR_F color{};
        if (ParseString(raw, text) && ParseColor(text, color)) out.background = color;
    }
    if (FindMember(json, "cacheBudgetMB", raw)) {
        if (ParseNumber(raw, number) && number >= 0.0 && number <= 65536.0) {
            out.cacheBudgetMB = static_cast<int>(number);
        }
    }
    if (FindMember(json, "initialSizing", raw)) {
        std::string text;
        if (ParseString(raw, text)) {
            if (text == "actual" || text == "Actual") out.initialSizing = AppSettings::InitialSizing::Actual;
            else if (text == "fit" || text == "Fit") out.initialSizing = AppSettings::InitialSizing::Fit;
        }
    }
    return true;
}

bool SaveSettings(const AppSettings& settings) {
    const std::wstring dir = SettingsDirectory();
    if (dir.empty()) return false;
    // Only ever create the one directory we own.
    if (!IsDirectoryExisting(dir) && !CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }

    char number[64];
    std::snprintf(number, sizeof(number), "%.2f", settings.zoomStep);

    std::string text = "{\n";
    text += "  \"zoomStep\": ";
    text += number;
    text += ",\n  \"backgroundColor\": \"";
    text += FormatColor(settings.background);
    text += "\",\n  \"initialSizing\": \"";
    text += settings.initialSizing == AppSettings::InitialSizing::Fit ? "fit" : "actual";
    text += "\"\n}\n";

    const std::wstring path = SettingsPath();
    const std::wstring temp = path + L".tmp";
    if (!WriteFileUtf8(temp, text)) return false;
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) == TRUE;
}
