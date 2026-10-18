// decode.h — WIC decoding on a worker thread.
//
// The UI thread never blocks on a file: it posts an "open" request and receives a
// DecodeResult through WM_APP_DECODE. Only the newest request is honoured; older
// work observes the generation counter and aborts between stages.
#pragma once

#include "common.h"

#include <wincodec.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

struct ImageMeta {
    UINT srcWidth = 0;
    UINT srcHeight = 0;
    UINT orientation = 1;
    bool colorManaged = false;
};

// 32bpp PBGRA, top-down.
struct PixelBuffer {
    std::vector<BYTE> pixels;
    UINT width = 0;
    UINT height = 0;

    UINT Stride() const { return width * 4; }
    bool Empty() const { return pixels.empty() || width == 0 || height == 0; }
    void Reset() {
        pixels.clear();
        width = height = 0;
    }
};

enum class DecodeKind {
    Full,      // whole image, fits in one GPU texture
    Overview,  // downscaled stand-in for an image too large for one texture
    Tile,      // one detail tile of such an image
    Prefetch,  // background screen-sized copy for the session cache (not shown)
};

struct DecodeResult {
    DecodeKind kind = DecodeKind::Full;
    UINT generation = 0;
    std::wstring path;

    // Tile coordinates (only meaningful for DecodeKind::Tile).
    UINT tileX = 0;
    UINT tileY = 0;
    UINT tileSize = 0;

    HRESULT hr = E_FAIL;
    PixelBuffer pixels;
    ImageMeta meta;

    // Screen-sized copy of the image for the session cache (may be empty).
    PixelBuffer cachePreview;

    // For Overview: overview pixels per source pixel.
    double overviewPixelsPerImagePixel = 1.0;
    // True when the decoder itself produced the scaled preview (fast path).
    bool nativeScaled = false;

    double openMs = 0.0;
    double decodeMs = 0.0;
    double totalMs = 0.0;
};

HRESULT CreateWicFactory(IWICImagingFactory** factory);

// Box-filter downscale into a new buffer (used for cache previews).
void DownscaleToFit(const PixelBuffer& source, UINT maxEdge, PixelBuffer& out);

// Synchronous decode helper for offscreen capture and tests: decodes on the
// calling thread and keeps the decoder open so tiles can be produced on demand.
class SyncImage {
public:
    SyncImage();
    ~SyncImage();
    SyncImage(const SyncImage&) = delete;
    SyncImage& operator=(const SyncImage&) = delete;

    HRESULT Open(const std::wstring& path);
    const ImageMeta& Meta() const;
    bool IsOpen() const;
    void Close();

    HRESULT DecodeFull(PixelBuffer& out);
    HRESULT DecodeOverview(UINT maxEdge, PixelBuffer& out, double* pixelsPerImagePixel);
    HRESULT DecodeRect(UINT x, UINT y, UINT w, UINT h, PixelBuffer& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A decode job for one generation. Cancellation is cooperative: the callbacks run
// between decode stages and return true when the work is obsolete.
class Decoder {
public:
    Decoder(HWND notify, UINT message);
    ~Decoder();

    void Start();
    void Stop();

    // Supersedes all queued/in-flight work for older generations.
    void Reset(UINT generation);

    // Decodes the whole image when it fits in `maxTextureEdge`, otherwise only an
    // overview (the UI then asks for tiles). `cacheEdge` is the longest edge of the
    // screen-sized copy handed back for the preview cache.
    void RequestOpen(const std::wstring& path, UINT generation, UINT maxTextureEdge,
                     UINT cacheEdge);
    void RequestTile(const std::wstring& path, UINT generation, UINT tileX, UINT tileY,
                     UINT tileSize);

    // Background, lowest priority: decodes a screen-sized copy of another file so
    // that navigating to it later needs no disk work.
    void RequestPrefetch(const std::wstring& path, UINT generation, UINT maxEdge);

private:
    struct Job {
        enum class Type { Open, Tile, Prefetch } type = Type::Open;
        std::wstring path;
        UINT generation = 0;
        UINT maxTextureEdge = 0;
        UINT tileX = 0;
        UINT tileY = 0;
        UINT tileSize = 0;
        UINT cacheEdge = 0;
        UINT maxEdge = 0;
    };

    void ThreadMain();
    void HandleOpen(const Job& job);
    void HandleTile(const Job& job);
    void HandlePrefetch(const Job& job);
    void DropQueuedPrefetches();
    bool IsCancelled(UINT generation) const;
    void PostResult(DecodeResult&& result);

    HWND notify_ = nullptr;
    UINT message_ = 0;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool stopping_ = false;

    std::atomic<UINT> currentGeneration_{0};

    // Owned by the worker thread.
    std::unique_ptr<class WicSession> session_;
};
