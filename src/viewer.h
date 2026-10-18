// viewer.h — window-independent viewer state and drawing.
//
// Everything is measured in physical pixels: the render target is created at
// 96 DPI, so "zoom = 1.0" means one image pixel per screen pixel, regardless of
// the monitor scaling in use.
#pragma once

#include "common.h"
#include "cache.h"
#include "decode.h"
#include "folder.h"
#include "settings.h"

#include <d2d1.h>

#include <d2d1.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>
#include <utility>

class Viewer {
public:
    Viewer();
    ~Viewer();

    // Window mode is split so decoding can start before the GPU device is created
    // (the two then overlap instead of running back to back).
    bool InitWorkers(HWND hwnd);
    bool InitGraphics(HWND hwnd);
    bool InitForWindow(HWND hwnd);
    // Capture mode: draws into a caller-owned target (a WIC bitmap target), and
    // decodes synchronously.
    void AttachOffscreenTarget(ID2D1RenderTarget* target, UINT forceTextureEdge = 0);
    void Shutdown();

    void SetSettings(const AppSettings& settings);
    const AppSettings& Settings() const { return settings_; }
    void RefreshBackground();

    void OpenFile(const std::wstring& path);
    bool LoadBlocking(const std::wstring& path);

    void OnPaint();
    void OnSize(UINT width, UINT height);
    void OnDpiChanged();
    void OnMouseWheel(int wheelDelta, POINT clientPoint);
    void OnLButtonDown(POINT clientPoint);
    void OnMouseMove(POINT clientPoint);
    void OnLButtonUp(POINT clientPoint);
    void OnKeyDown(UINT vk);
    void OnDropFiles(HDROP drop);
    // Returns true when the timer belonged to the viewer.
    bool OnTimer(UINT_PTR timerId);
    void OnDecodeResult(DecodeResult* result);
    void OnFolderList(FolderList* list);

    HRESULT Draw(ID2D1RenderTarget* target, D2D1_SIZE_F size);
    void SetZoom(double zoom);

    void Navigate(int delta);
    bool CanNavigate() const { return folder_.names.size() > 1; }

    const std::wstring& CurrentPath() const { return path_; }
    void SetFirstFrameCallback(std::function<void()> callback);
    void SetImageReadyCallback(std::function<void()> callback);

    // Bench support: timings of the most recent successful/failed open.
    struct Timing {
        double openMs = 0.0;
        double decodeMs = 0.0;
        double totalMs = 0.0;
        double firstFrameMs = 0.0;  // process-relative time of the first painted frame
        double readyMs = 0.0;       // process-relative time the final bitmap was applied
        double requestMs = 0.0;     // process-relative time the open was requested
        double uploadMs = 0.0;      // CreateBitmap() time on the UI thread
        bool fromCache = false;     // shown instantly from the session cache
        bool success = false;
        UINT width = 0;
        UINT height = 0;
        bool tiled = false;
        bool colorManaged = false;
        UINT orientation = 1;
    };
    const Timing& LastTiming() const { return lastTiming_; }

private:
    HRESULT CreateHwndTarget(HWND hwnd, UINT width, UINT height);
    void DiscardTarget();
    void DiscardImage();
    void ReopenCurrent();

    bool CreateBitmap(const PixelBuffer& pixels, ComPtr<ID2D1Bitmap>& out);
    void ApplyInitialView(D2D1_SIZE_F size);
    void ClampCenter(D2D1_SIZE_F size);
    void ZoomAt(D2D1_POINT_2F cursor, double factor);
    void EnsureFolderScan();
    void EnsureTile(UINT tileX, UINT tileY, D2D1_SIZE_F size);
    void DrawImage(ID2D1RenderTarget* target, D2D1_SIZE_F size);
    void DrawFileIcon(ID2D1RenderTarget* target, D2D1_SIZE_F size);
    void LoadFileIcon();
    UINT PreviewEdge() const;
    size_t CacheBudgetBytes() const;
    void ArmPrefetchTimer();
    void RequestFullResolution();
    void SchedulePrefetch();
    void DrawTiled(ID2D1RenderTarget* target, D2D1_SIZE_F size);
    UINT MaxTextureEdge() const;
    D2D1_SIZE_F WindowSize() const;
    void RequestTileIfNeeded(UINT tileX, UINT tileY, D2D1_SIZE_F size);

    HWND hwnd_ = nullptr;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1HwndRenderTarget> hwndTarget_;
    ID2D1RenderTarget* target_ = nullptr;
    bool offscreen_ = false;
    UINT forcedTextureEdge_ = 0;

    AppSettings settings_;
    D2D1_COLOR_F background_ = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);

    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<FolderScanner> scanner_;
    std::unique_ptr<class SyncImage> syncImage_;

    std::wstring path_;
    UINT generation_ = 0;
    ImageMeta meta_;
    UINT imageWidth_ = 0;
    UINT imageHeight_ = 0;
    bool hasImage_ = false;
    bool tiled_ = false;
    // Showing a screen-resolution copy from the session cache; full resolution is
    // decoded lazily, when the user zooms past what the cached copy can show.
    bool previewOnly_ = false;
    double cachedScale_ = 1.0;
    bool fullRequested_ = false;
    bool prefetchScheduled_ = false;
    std::wstring prefetchDirectory_;
    std::unordered_set<std::wstring> prefetchRequested_;
    PreviewCache cache_;
    double overviewScale_ = 1.0;

    ComPtr<ID2D1Bitmap> image_;
    ComPtr<ID2D1Bitmap> fileIcon_;  // shown when the file is not a decodable image
    ComPtr<ID2D1Bitmap> overview_;
    std::map<std::pair<UINT, UINT>, ComPtr<ID2D1Bitmap>> tiles_;
    std::set<std::pair<UINT, UINT>> tilesRequested_;
    UINT tileSize_ = 1024;
    static constexpr UINT_PTR kPrefetchTimerId = 2;
    static constexpr UINT kPrefetchDelayMs = 700;
    static constexpr size_t kMaxTiles = 24;
    static constexpr size_t kMaxTileRequests = 6;

    bool fitMode_ = true;
    double zoom_ = 1.0;
    D2D1_POINT_2F center_ = D2D1::Point2F(0.0f, 0.0f);

    bool dragging_ = false;
    POINT dragLast_ = {0, 0};

    FolderList folder_;
    UINT folderGeneration_ = 0;
    bool folderScanStarted_ = false;

    Timing lastTiming_;
    std::function<void()> firstFrameCallback_;
    std::function<void()> imageReadyCallback_;
    bool firstFrameReported_ = false;
    double openStartMs_ = 0.0;
};
