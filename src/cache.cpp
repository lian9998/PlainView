// cache.cpp
#include "cache.h"

#include <algorithm>
#include <cstdio>

namespace {
constexpr size_t kMaxEntries = 32;
}

std::wstring MakeCacheKey(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return path;

    const ULARGE_INTEGER size{{data.nFileSizeLow, data.nFileSizeHigh}};
    const ULARGE_INTEGER written{{data.ftLastWriteTime.dwLowDateTime,
                                  data.ftLastWriteTime.dwHighDateTime}};
    wchar_t suffix[64];
    swprintf_s(suffix, L"|%llu|%llu", size.QuadPart, written.QuadPart);
    return path + suffix;
}

void PreviewCache::Configure(UINT maxEdge, size_t budgetBytes) {
    maxEdge_ = std::max<UINT>(256, maxEdge);
    budget_ = budgetBytes;
    EvictUntil(0);
}

bool PreviewCache::Contains(const std::wstring& key) const {
    return index_.find(key) != index_.end();
}

bool PreviewCache::Lookup(const std::wstring& key, PixelBuffer& pixels, ImageMeta& meta) {
    const auto found = index_.find(key);
    if (found == index_.end()) return false;

    items_.splice(items_.begin(), items_, found->second);
    pixels = found->second->pixels;  // copy: the caller uploads it to the GPU
    meta = found->second->meta;
    return true;
}

void PreviewCache::Store(const std::wstring& key, const PixelBuffer& pixels, const ImageMeta& meta) {
    if (pixels.Empty()) return;
    const size_t incoming = pixels.pixels.size();
    // With a finite budget, one entry may never take more than a third of it.
    if (!Unlimited() && incoming > budget_ / 3) return;

    const auto found = index_.find(key);
    if (found != index_.end()) {
        bytes_ -= found->second->pixels.pixels.size();
        items_.erase(found->second);
        index_.erase(found);
    }

    if (!Unlimited()) {
        EvictUntil(incoming);
        if (bytes_ + incoming > budget_) return;
    }

    items_.push_front(Entry{key, pixels, meta});
    index_[key] = items_.begin();
    bytes_ += incoming;
}

void PreviewCache::Clear() {
    items_.clear();
    index_.clear();
    bytes_ = 0;
}

void PreviewCache::EvictUntil(size_t incomingBytes) {
    if (Unlimited()) return;  // the OS decides what to page out
    while (!items_.empty() &&
           (bytes_ + incomingBytes > budget_ || items_.size() >= kMaxEntries)) {
        bytes_ -= items_.back().pixels.pixels.size();
        index_.erase(items_.back().key);
        items_.pop_back();
    }
}
