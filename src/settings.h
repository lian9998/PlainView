// settings.h — %LOCALAPPDATA%\PlainView\settings.json
#pragma once

#include "common.h"

#include <d2d1.h>

// Session cache budget, in megabytes. The cache holds screen-resolution copies of
// recently viewed images in RAM only, so the budget is the whole story: entries are
// never larger than the window and never touch the disk.
inline constexpr int kCacheBudgetMinMB = 16;
inline constexpr int kCacheBudgetMaxMB = 128 * 1024;  // 128 GB
inline constexpr int kCacheBudgetUnlimitedMB = 0;     // let Windows decide what to page out
inline constexpr int kCacheBudgetDefaultMB = 256;

struct AppSettings {
    enum class InitialSizing { Fit, Actual };

    double zoomStep = 1.15;
    // Session cache budget in megabytes; kCacheBudgetUnlimitedMB means "no limit".
    int cacheBudgetMB = kCacheBudgetDefaultMB;
    // How many images to prefetch in each direction (0 disables prefetching).
    int prefetchDepth = 3;
    D2D1_COLOR_F background = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
    InitialSizing initialSizing = InitialSizing::Fit;
};

std::wstring SettingsDirectory();  // %LOCALAPPDATA%\PlainView
std::wstring SettingsPath();       // %LOCALAPPDATA%\PlainView\settings.json

// Loads settings; returns false when the file is absent or unreadable, leaving
// `out` at its defaults. Missing members keep their default value.
bool LoadSettings(AppSettings& out);
bool SaveSettings(const AppSettings& settings);
