// settings.h — %LOCALAPPDATA%\PlainView\settings.json
#pragma once

#include "common.h"

#include <d2d1.h>

struct AppSettings {
    enum class InitialSizing { Fit, Actual };

    double zoomStep = 1.15;
    D2D1_COLOR_F background = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
    InitialSizing initialSizing = InitialSizing::Fit;
};

std::wstring SettingsDirectory();  // %LOCALAPPDATA%\PlainView
std::wstring SettingsPath();       // %LOCALAPPDATA%\PlainView\settings.json

// Loads settings; returns false when the file is absent or unreadable, leaving
// `out` at its defaults. Missing members keep their default value.
bool LoadSettings(AppSettings& out);
bool SaveSettings(const AppSettings& settings);
