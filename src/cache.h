// cache.h — session-only preview cache.
//
// Keeps screen-resolution copies of recently viewed images in RAM so that going
// back and forth in a folder is instant. Nothing is written to disk. Entries are
// bounded both by pixel size (the window's longest edge) and by a byte budget, so
// the cache cannot grow without limit.
#pragma once

#include "common.h"
#include "decode.h"

#include <list>
#include <string>
#include <unordered_map>

// Path plus size plus last-write time, so an edited file is never served stale.
std::wstring MakeCacheKey(const std::wstring& path);

// Longest edge of a cached preview for a window of this size: the window's longest
// edge, kept between 512 and 4096 pixels. Screen resolution, in other words.
UINT PreviewEdgeForWindow(UINT width, UINT height);

class PreviewCache {
public:
    void Configure(UINT maxEdge, size_t budgetBytes);

    UINT MaxEdge() const { return maxEdge_; }
    size_t BudgetBytes() const { return budget_; }
    bool Unlimited() const { return budget_ == kUnlimited; }
    static constexpr size_t kUnlimited = static_cast<size_t>(-1);
    size_t UsedBytes() const { return bytes_; }
    size_t Count() const { return items_.size(); }

    bool Contains(const std::wstring& key) const;
    bool Lookup(const std::wstring& key, PixelBuffer& pixels, ImageMeta& meta);
    void Store(const std::wstring& key, const PixelBuffer& pixels, const ImageMeta& meta);
    void Clear();

private:
    void EvictUntil(size_t incomingBytes);

    struct Entry {
        std::wstring key;
        PixelBuffer pixels;
        ImageMeta meta;
    };

    std::list<Entry> items_;  // front = most recently used
    std::unordered_map<std::wstring, std::list<Entry>::iterator> index_;
    size_t bytes_ = 0;
    size_t budget_ = 256ull * 1024 * 1024;
    UINT maxEdge_ = 2048;
};
