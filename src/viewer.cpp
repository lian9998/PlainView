// viewer.cpp
#include "viewer.h"

#include "shellicon.h"

#include <d2d1helper.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr double kMinZoom = 0.02;
constexpr double kMaxZoom = 32.0;

// ID2D1RenderTarget only offers nearest-neighbour and linear. Crisp pixels when
// zoomed in, smooth sampling when scaling down.
D2D1_BITMAP_INTERPOLATION_MODE InterpolationFor(double scale) {
    return scale >= 2.0 ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                        : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
}

bool OrientationSwapsAxes(UINT orientation) { return orientation >= 5 && orientation <= 8; }


}  // namespace

Viewer::Viewer() = default;

Viewer::~Viewer() { Shutdown(); }

bool Viewer::InitWorkers(HWND hwnd) {
    hwnd_ = hwnd;
    decoder_ = std::make_unique<Decoder>(hwnd, WM_APP_DECODE);
    decoder_->Start();
    scanner_ = std::make_unique<FolderScanner>(hwnd, WM_APP_FOLDER);
    scanner_->Start();
    return true;
}

bool Viewer::InitGraphics(HWND hwnd) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory_)))) {
        return false;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (FAILED(CreateHwndTarget(hwnd, static_cast<UINT>(rc.right - rc.left),
                                static_cast<UINT>(rc.bottom - rc.top)))) {
        return false;
    }
    // Session cache: screen-sized entries, never on disk. Bounded by default
    // (256 MB); "unlimited" is an explicit opt-in.
    cache_.Configure(PreviewEdge(), CacheBudgetBytes());
    // If the image arrived while the device was still being created, make sure it
    // fits this device's texture limit.
    if (hasImage_ && std::max(imageWidth_, imageHeight_) > MaxTextureEdge()) ReopenCurrent();
    return true;
}

bool Viewer::InitForWindow(HWND hwnd) { return InitWorkers(hwnd) && InitGraphics(hwnd); }

void Viewer::AttachOffscreenTarget(ID2D1RenderTarget* target, UINT forceTextureEdge) {
    target_ = target;
    offscreen_ = true;
    forcedTextureEdge_ = forceTextureEdge;
}

void Viewer::Shutdown() {
    if (decoder_) {
        decoder_->Stop();
        decoder_.reset();
    }
    if (scanner_) {
        scanner_->Stop();
        scanner_.reset();
    }
    DiscardImage();
    DiscardTarget();
    target_ = nullptr;
    syncImage_.reset();
}

void Viewer::SetSettings(const AppSettings& settings) {
    settings_ = settings;
    background_ = settings.background;
    if (target_) cache_.Configure(PreviewEdge(), CacheBudgetBytes());
    prefetchScheduled_ = false;
    if (target_ && hwnd_) ArmPrefetchTimer();
}

void Viewer::RefreshBackground() {
    background_ = settings_.background;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

HRESULT Viewer::CreateHwndTarget(HWND hwnd, UINT width, UINT height) {
    if (!factory_) return E_FAIL;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd, D2D1::SizeU(std::max(1u, width), std::max(1u, height)));

    ComPtr<ID2D1HwndRenderTarget> target;
    const HRESULT hr = factory_->CreateHwndRenderTarget(props, hwndProps, &target);
    if (FAILED(hr)) return hr;

    hwndTarget_ = target;
    target_ = hwndTarget_.Get();
    return S_OK;
}

void Viewer::DiscardTarget() {
    hwndTarget_.Reset();
    if (!offscreen_) target_ = nullptr;
}

void Viewer::DiscardImage() {
    image_.Reset();
    fileIcon_.Reset();
    overview_.Reset();
    tiles_.clear();
    tilesRequested_.clear();
    hasImage_ = false;
    tiled_ = false;
    previewOnly_ = false;
    cachedScale_ = 1.0;
    fullRequested_ = false;
    overviewScale_ = 1.0;
    imageWidth_ = imageHeight_ = 0;
    meta_ = ImageMeta{};
}

UINT Viewer::MaxTextureEdge() const {
    if (forcedTextureEdge_) return forcedTextureEdge_;
    UINT edge = target_ ? target_->GetMaximumBitmapSize() : 0;
    if (edge == 0 || edge > 16384) edge = 16384;
    return edge;
}

bool Viewer::CreateBitmap(const PixelBuffer& pixels, ComPtr<ID2D1Bitmap>& out) {
    if (!target_ || pixels.Empty()) return false;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
        96.0f);
    return SUCCEEDED(target_->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height),
                                           pixels.pixels.data(), pixels.Stride(), props, &out));
}

void Viewer::OpenFile(const std::wstring& path) {
    if (path.empty()) return;

    path_ = path;
    ++generation_;
    if (hwnd_) KillTimer(hwnd_, kPrefetchTimerId);
    openStartMs_ = NowMs();
    lastTiming_ = Timing{};
    firstFrameReported_ = false;
    prefetchScheduled_ = false;
    folderScanStarted_ = false;
    fitMode_ = true;
    DiscardImage();

    if (decoder_) decoder_->Reset(generation_);
    if (hwnd_) SetWindowTextW(hwnd_, path_.c_str());

    // Navigation inside one folder reuses the list that is already in memory.
    const std::wstring dir = DirectoryOf(path_);
    if (_wcsicmp(dir.c_str(), prefetchDirectory_.c_str()) != 0) {
        prefetchDirectory_ = dir;
        prefetchRequested_.clear();
        if (decoder_) decoder_->ClearPrefetchQueue();
    }
    if (!folder_.names.empty() && _wcsicmp(folder_.directory.c_str(), dir.c_str()) == 0) {
        const std::wstring name = FileNameOf(path_);
        folder_.index = -1;
        for (size_t i = 0; i < folder_.names.size(); ++i) {
            if (_wcsicmp(folder_.names[i].c_str(), name.c_str()) == 0) {
                folder_.index = static_cast<int>(i);
                break;
            }
        }
    } else {
        folder_ = FolderList{};
    }

    // Session cache first: a file seen before appears with no disk work at all.
    bool shownFromCache = false;
    if (target_) {
        PixelBuffer cached;
        ImageMeta cachedMeta;
        if (cache_.Lookup(MakeCacheKey(path_), cached, cachedMeta) && !cached.Empty()) {
            const bool swapped = OrientationSwapsAxes(cachedMeta.orientation);
            const UINT fullWidth = swapped ? cachedMeta.srcHeight : cachedMeta.srcWidth;
            const UINT fullHeight = swapped ? cachedMeta.srcWidth : cachedMeta.srcHeight;
            if (CreateBitmap(cached, image_)) {
                meta_ = cachedMeta;
                imageWidth_ = fullWidth;
                imageHeight_ = fullHeight;
                hasImage_ = true;
                tiled_ = false;
                cachedScale_ = static_cast<double>(cached.width) / fullWidth;
                previewOnly_ = cached.width < fullWidth || cached.height < fullHeight;
                fullRequested_ = !previewOnly_;
                lastTiming_.fromCache = true;
                center_ = D2D1::Point2F(static_cast<float>(imageWidth_) / 2.0f,
                                        static_cast<float>(imageHeight_) / 2.0f);
                ApplyInitialView(WindowSize());
                shownFromCache = true;
            }
        }
    }

    if (!shownFromCache && decoder_) {
        decoder_->RequestOpen(path_, generation_, MaxTextureEdge(), PreviewEdge());
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (shownFromCache) ArmPrefetchTimer();
}

bool Viewer::LoadBlocking(const std::wstring& path) {
    if (!target_ || path.empty()) return false;

    path_ = path;
    DiscardImage();
    lastTiming_ = Timing{};

    syncImage_ = std::make_unique<SyncImage>();
    if (FAILED(syncImage_->Open(path))) {
        syncImage_.reset();
        return false;
    }
    meta_ = syncImage_->Meta();
    const bool swapped = OrientationSwapsAxes(meta_.orientation);
    imageWidth_ = swapped ? meta_.srcHeight : meta_.srcWidth;
    imageHeight_ = swapped ? meta_.srcWidth : meta_.srcHeight;

    const UINT edge = MaxTextureEdge();
    if (std::max(imageWidth_, imageHeight_) > edge) {
        PixelBuffer pixels;
        double scale = 1.0;
        if (FAILED(syncImage_->DecodeOverview(std::min<UINT>(2048, edge), pixels, &scale))) {
            return false;
        }
        if (!CreateBitmap(pixels, overview_)) return false;
        tiled_ = true;
        overviewScale_ = scale;
    } else {
        PixelBuffer pixels;
        if (FAILED(syncImage_->DecodeFull(pixels))) return false;
        if (!CreateBitmap(pixels, image_)) return false;
        tiled_ = false;
    }

    hasImage_ = true;
    lastTiming_.success = true;
    lastTiming_.width = imageWidth_;
    lastTiming_.height = imageHeight_;
    lastTiming_.tiled = tiled_;
    lastTiming_.colorManaged = meta_.colorManaged;
    lastTiming_.orientation = meta_.orientation;

    center_ = D2D1::Point2F(static_cast<float>(imageWidth_) / 2.0f,
                            static_cast<float>(imageHeight_) / 2.0f);
    ApplyInitialView(WindowSize());
    return true;
}

void Viewer::ReopenCurrent() {
    if (path_.empty() || !decoder_) return;
    ++generation_;
    decoder_->Reset(generation_);
    decoder_->RequestOpen(path_, generation_, MaxTextureEdge(), PreviewEdge());
}

D2D1_SIZE_F Viewer::WindowSize() const { return target_ ? target_->GetSize() : D2D1::SizeF(0, 0); }

void Viewer::ApplyInitialView(D2D1_SIZE_F size) {
    if (!hasImage_ || size.width <= 0.0f || size.height <= 0.0f) return;
    if (settings_.initialSizing == AppSettings::InitialSizing::Fit) {
        zoom_ = std::min(static_cast<double>(size.width) / imageWidth_,
                         static_cast<double>(size.height) / imageHeight_);
        fitMode_ = true;
    } else {
        zoom_ = 1.0;
        fitMode_ = false;
    }
    center_ = D2D1::Point2F(static_cast<float>(imageWidth_) / 2.0f,
                            static_cast<float>(imageHeight_) / 2.0f);
    ClampCenter(size);
}

UINT Viewer::PreviewEdge() const {
    const D2D1_SIZE_F size = WindowSize();
    return PreviewEdgeForWindow(static_cast<UINT>(std::max(0.0f, size.width)),
                                static_cast<UINT>(std::max(0.0f, size.height)));
}

size_t Viewer::CacheBudgetBytes() const {
    if (settings_.cacheBudgetMB == kCacheBudgetUnlimitedMB) return PreviewCache::kUnlimited;
    const int megabytes =
        std::clamp(settings_.cacheBudgetMB, kCacheBudgetMinMB, kCacheBudgetMaxMB);
    return static_cast<size_t>(megabytes) * 1024 * 1024;
}

void Viewer::ArmPrefetchTimer() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kPrefetchTimerId);
    SetTimer(hwnd_, kPrefetchTimerId, kPrefetchDelayMs, nullptr);
}

bool Viewer::OnTimer(UINT_PTR timerId) {
    if (timerId != kPrefetchTimerId) return false;
    if (hwnd_) KillTimer(hwnd_, kPrefetchTimerId);
    SchedulePrefetch();
    return true;
}

void Viewer::RequestFullResolution() {
    if (!decoder_ || path_.empty() || fullRequested_ || !previewOnly_) return;
    fullRequested_ = true;
    decoder_->RequestOpen(path_, generation_, MaxTextureEdge(), PreviewEdge());
}

void Viewer::SchedulePrefetch() {
    if (prefetchScheduled_ || !decoder_ || folder_.names.size() < 2 || folder_.index < 0) return;
    prefetchScheduled_ = true;

    const int depth = std::clamp(settings_.prefetchDepth, 0, 20);
    if (depth == 0) return;

    const int count = static_cast<int>(folder_.names.size());
    const UINT edge = PreviewEdge();
    for (int direction : {-1, 1}) {
        for (int step = 1; step <= depth; ++step) {
            const int index = ((folder_.index + direction * step) % count + count) % count;
            if (index == folder_.index) break;
            const std::wstring path =
                JoinPath(folder_.directory, folder_.names[static_cast<size_t>(index)]);
            const std::wstring key = MakeCacheKey(path);
            if (cache_.Contains(key)) continue;
            if (!prefetchRequested_.insert(key).second) continue;
            decoder_->RequestPrefetch(
                path, edge,
                direction > 0 ? PrefetchDirection::Forward : PrefetchDirection::Backward);
        }
    }
}

void Viewer::SetZoom(double zoom) {
    if (!hasImage_) return;
    zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    fitMode_ = false;
    center_ = D2D1::Point2F(static_cast<float>(imageWidth_) / 2.0f,
                            static_cast<float>(imageHeight_) / 2.0f);
    ClampCenter(WindowSize());
}

void Viewer::ClampCenter(D2D1_SIZE_F size) {
    if (!hasImage_ || size.width <= 0.0f || size.height <= 0.0f) return;
    const double halfW = size.width / (2.0 * zoom_);
    const double halfH = size.height / (2.0 * zoom_);
    if (imageWidth_ * zoom_ <= size.width) {
        center_.x = static_cast<float>(imageWidth_) / 2.0f;
    } else {
        center_.x = static_cast<float>(std::clamp<double>(center_.x, halfW, imageWidth_ - halfW));
    }
    if (imageHeight_ * zoom_ <= size.height) {
        center_.y = static_cast<float>(imageHeight_) / 2.0f;
    } else {
        center_.y = static_cast<float>(std::clamp<double>(center_.y, halfH, imageHeight_ - halfH));
    }
}

void Viewer::ZoomAt(D2D1_POINT_2F cursor, double factor) {
    if (!hasImage_) return;
    const D2D1_SIZE_F size = WindowSize();
    if (size.width <= 0.0f) return;

    const double target = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
    if (target == zoom_) return;

    const double dx = cursor.x - size.width / 2.0;
    const double dy = cursor.y - size.height / 2.0;
    const double imageX = center_.x + dx / zoom_;
    const double imageY = center_.y + dy / zoom_;

    zoom_ = target;
    center_.x = static_cast<float>(imageX - dx / zoom_);
    center_.y = static_cast<float>(imageY - dy / zoom_);
    fitMode_ = false;
    ClampCenter(size);
    if (previewOnly_ && zoom_ > cachedScale_ * 1.02) RequestFullResolution();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Viewer::OnMouseWheel(int wheelDelta, POINT clientPoint) {
    if (!hasImage_) return;
    const double notches = static_cast<double>(wheelDelta) / WHEEL_DELTA;
    if (notches == 0.0) return;
    ZoomAt(D2D1::Point2F(static_cast<float>(clientPoint.x), static_cast<float>(clientPoint.y)),
           std::pow(settings_.zoomStep, notches));
}

void Viewer::OnLButtonDown(POINT clientPoint) {
    if (!hasImage_) return;
    dragging_ = true;
    dragLast_ = clientPoint;
    if (hwnd_) SetCapture(hwnd_);
}

void Viewer::OnMouseMove(POINT clientPoint) {
    if (!dragging_ || !hasImage_) return;
    const double dx = clientPoint.x - dragLast_.x;
    const double dy = clientPoint.y - dragLast_.y;
    dragLast_ = clientPoint;
    center_.x = static_cast<float>(center_.x - dx / zoom_);
    center_.y = static_cast<float>(center_.y - dy / zoom_);
    fitMode_ = false;
    ClampCenter(WindowSize());
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Viewer::OnLButtonUp(POINT) {
    if (!dragging_) return;
    dragging_ = false;
    if (hwnd_) ReleaseCapture();
}

void Viewer::OnKeyDown(UINT vk) {
    if (vk == VK_LEFT) Navigate(-1);
    else if (vk == VK_RIGHT) Navigate(1);
}

void Viewer::OnDropFiles(HDROP drop) {
    wchar_t path[MAX_PATH * 4] = {};
    const UINT length = DragQueryFileW(drop, 0, path, ARRAYSIZE(path));
    if (length > 0) OpenFile(path);
}

void Viewer::Navigate(int delta) {
    if (folder_.names.size() < 2 || folder_.index < 0) return;
    const int count = static_cast<int>(folder_.names.size());
    int index = (folder_.index + delta) % count;
    if (index < 0) index += count;
    OpenFile(JoinPath(folder_.directory, folder_.names[static_cast<size_t>(index)]));
}

void Viewer::EnsureFolderScan() {
    if (folderScanStarted_ || path_.empty()) return;
    folderScanStarted_ = true;
    ++folderGeneration_;
    if (scanner_) scanner_->Scan(DirectoryOf(path_), path_, folderGeneration_);
}

void Viewer::OnFolderList(FolderList* raw) {
    std::unique_ptr<FolderList> list(raw);
    if (!list) return;
    if (list->generation != folderGeneration_) return;
    folder_ = std::move(*list);
    prefetchScheduled_ = false;
    if (hasImage_) ArmPrefetchTimer();
}

void Viewer::OnDecodeResult(DecodeResult* raw) {
    std::unique_ptr<DecodeResult> result(raw);
    if (!result) return;

    // Prefetch results only fill the cache; they never touch what is on screen.
    if (result->kind == DecodeKind::Prefetch) {
        if (SUCCEEDED(result->hr) && !result->cachePreview.Empty()) {
            cache_.Store(MakeCacheKey(result->path), result->cachePreview, result->meta);
        }
        return;
    }

    if (result->generation != generation_) return;  // obsolete work

    if (result->kind == DecodeKind::Tile) {
        const std::pair<UINT, UINT> key{result->tileX, result->tileY};
        tilesRequested_.erase(key);
        if (SUCCEEDED(result->hr) && !result->pixels.Empty()) {
            ComPtr<ID2D1Bitmap> tile;
            if (CreateBitmap(result->pixels, tile)) tiles_[key] = tile;
        }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    lastTiming_.openMs = result->openMs;
    lastTiming_.decodeMs = result->decodeMs;
    lastTiming_.totalMs = result->totalMs;
    lastTiming_.success = SUCCEEDED(result->hr) && !result->pixels.Empty();
    lastTiming_.colorManaged = result->meta.colorManaged;
    lastTiming_.orientation = result->meta.orientation;

    // True when a cached (or overview) image is already on screen; the sharper
    // bitmap then replaces it without disturbing zoom or pan.
    const bool keepView = hasImage_;

    if (lastTiming_.success) {
        meta_ = result->meta;
        const bool swapped = OrientationSwapsAxes(meta_.orientation);
        imageWidth_ = swapped ? meta_.srcHeight : meta_.srcWidth;
        imageHeight_ = swapped ? meta_.srcWidth : meta_.srcHeight;

        if (result->kind == DecodeKind::Full) {
            const double uploadStart = NowMs();
            hasImage_ = CreateBitmap(result->pixels, image_);
            lastTiming_.uploadMs = NowMs() - uploadStart;
            tiled_ = false;
            overviewScale_ = 1.0;
            previewOnly_ = false;
            fullRequested_ = true;
            cachedScale_ = 1.0;
        } else {
            hasImage_ = CreateBitmap(result->pixels, overview_);
            tiled_ = true;
            overviewScale_ = result->overviewPixelsPerImagePixel;
            previewOnly_ = false;
        }
        lastTiming_.width = imageWidth_;
        lastTiming_.height = imageHeight_;
        lastTiming_.tiled = tiled_;

        // Remember a screen-sized copy so navigating back here is instant.
        if (!result->cachePreview.Empty()) {
            cache_.Store(MakeCacheKey(result->path), result->cachePreview, result->meta);
        }

        if (hasImage_ && !keepView) {
            center_ = D2D1::Point2F(static_cast<float>(imageWidth_) / 2.0f,
                                    static_cast<float>(imageHeight_) / 2.0f);
            ApplyInitialView(WindowSize());
        } else if (!hasImage_) {
            lastTiming_.success = false;
        }
    } else if (!previewOnly_) {
        // Not a decodable image: show the icon Explorer would show for this file,
        // at a tenth of the window width. A cached preview, if one is showing, wins.
        hasImage_ = false;
        imageWidth_ = imageHeight_ = 0;
        LoadFileIcon();
    }

    // The folder is enumerated only now: the first image is up (or has failed).
    EnsureFolderScan();

    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);

    lastTiming_.readyMs = NowMs();
    if (imageReadyCallback_) imageReadyCallback_();
    // Prefetch only if the user lingers on this image.
    ArmPrefetchTimer();
}

void Viewer::OnPaint() {
    if (!hwnd_) return;
    if (!hwndTarget_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (FAILED(CreateHwndTarget(hwnd_, static_cast<UINT>(rc.right - rc.left),
                                    static_cast<UINT>(rc.bottom - rc.top)))) {
            return;
        }
        ReopenCurrent();
    }

    const HRESULT hr = Draw(hwndTarget_.Get(), hwndTarget_->GetSize());
    if (hr == D2DERR_RECREATE_TARGET) {
        // The device was lost; all device-dependent bitmaps are invalid now.
        DiscardImage();
        DiscardTarget();
        ReopenCurrent();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (SUCCEEDED(hr) && hasImage_ && !firstFrameReported_) {
        firstFrameReported_ = true;
        lastTiming_.firstFrameMs = NowMs();
        if (firstFrameCallback_) firstFrameCallback_();
    }
}

void Viewer::OnSize(UINT width, UINT height) {
    if (!hwndTarget_) return;
    const HRESULT hr = hwndTarget_->Resize(
        D2D1::SizeU(std::max(1u, width), std::max(1u, height)));
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardImage();
        DiscardTarget();
    }
    if (hasImage_) {
        if (fitMode_) ApplyInitialView(WindowSize());
        else ClampCenter(WindowSize());
    }
    cache_.Configure(PreviewEdge(), CacheBudgetBytes());
    if (previewOnly_ && zoom_ > cachedScale_ * 1.02) RequestFullResolution();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Viewer::OnDpiChanged() {
    if (hasImage_) {
        if (fitMode_) ApplyInitialView(WindowSize());
        else ClampCenter(WindowSize());
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

HRESULT Viewer::Draw(ID2D1RenderTarget* target, D2D1_SIZE_F size) {
    if (!target) return E_POINTER;
    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->Clear(background_);
    if (hasImage_ && (image_ || overview_)) {
        DrawImage(target, size);
    } else if (fileIcon_) {
        DrawFileIcon(target, size);
    }
    return target->EndDraw();
}

void Viewer::LoadFileIcon() {
    fileIcon_.Reset();
    if (!target_ || path_.empty()) return;
    ComPtr<ID2D1Bitmap> icon;
    UINT pixels = 0;
    if (SUCCEEDED(CreateShellIconBitmap(target_, path_, icon, &pixels))) {
        fileIcon_ = icon;
    }
}

void Viewer::DrawFileIcon(ID2D1RenderTarget* target, D2D1_SIZE_F size) {
    if (!fileIcon_ || size.width <= 0.0f) return;
    // One tenth of the window width, centred.
    const float extent = std::max(16.0f, size.width / 10.0f);
    const D2D1_RECT_F destination = D2D1::RectF(
        (size.width - extent) / 2.0f, (size.height - extent) / 2.0f,
        (size.width + extent) / 2.0f, (size.height + extent) / 2.0f);
    target->DrawBitmap(fileIcon_.Get(), &destination, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Viewer::DrawImage(ID2D1RenderTarget* target, D2D1_SIZE_F size) {
    const double scale = zoom_;
    const double originX = size.width / 2.0 - static_cast<double>(center_.x) * scale;
    const double originY = size.height / 2.0 - static_cast<double>(center_.y) * scale;
    const D2D1_RECT_F dest = D2D1::RectF(
        static_cast<float>(originX), static_cast<float>(originY),
        static_cast<float>(originX + imageWidth_ * scale),
        static_cast<float>(originY + imageHeight_ * scale));

    if (!tiled_) {
        if (image_) target->DrawBitmap(image_.Get(), &dest, 1.0f, InterpolationFor(scale));
        return;
    }

    // Coarse full-image stand-in: visible immediately, replaced by tiles where
    // they exist.
    if (overview_) {
        target->DrawBitmap(overview_.Get(), &dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    if (scale <= overviewScale_ * 1.05) return;  // overview resolution is enough
    DrawTiled(target, size);
}

void Viewer::DrawTiled(ID2D1RenderTarget* target, D2D1_SIZE_F size) {
    const double scale = zoom_;
    const double originX = size.width / 2.0 - static_cast<double>(center_.x) * scale;
    const double originY = size.height / 2.0 - static_cast<double>(center_.y) * scale;

    const double left = std::clamp((0.0 - originX) / scale, 0.0, static_cast<double>(imageWidth_));
    const double top = std::clamp((0.0 - originY) / scale, 0.0, static_cast<double>(imageHeight_));
    const double right = std::clamp((size.width - originX) / scale, 0.0,
                                    static_cast<double>(imageWidth_));
    const double bottom = std::clamp((size.height - originY) / scale, 0.0,
                                     static_cast<double>(imageHeight_));
    if (right <= left || bottom <= top) return;

    const UINT tx0 = static_cast<UINT>(left / tileSize_);
    const UINT ty0 = static_cast<UINT>(top / tileSize_);
    const UINT tx1 = std::min<UINT>(static_cast<UINT>((right - 0.5) / tileSize_),
                                    (imageWidth_ - 1) / tileSize_);
    const UINT ty1 = std::min<UINT>(static_cast<UINT>((bottom - 0.5) / tileSize_),
                                    (imageHeight_ - 1) / tileSize_);

    if (tiles_.size() > kMaxTiles) {
        // Drop whatever is off-screen first; if that is not enough, drop it all.
        for (auto it = tiles_.begin(); it != tiles_.end();) {
            const UINT tx = it->first.first, ty = it->first.second;
            if (tx < tx0 || tx > tx1 || ty < ty0 || ty > ty1) it = tiles_.erase(it);
            else ++it;
        }
        if (tiles_.size() > kMaxTiles) tiles_.clear();
    }

    for (UINT ty = ty0; ty <= ty1; ++ty) {
        for (UINT tx = tx0; tx <= tx1; ++tx) {
            const UINT width = std::min(tileSize_, imageWidth_ - tx * tileSize_);
            const UINT height = std::min(tileSize_, imageHeight_ - ty * tileSize_);
            const D2D1_RECT_F dest = D2D1::RectF(
                static_cast<float>(originX + tx * tileSize_ * scale),
                static_cast<float>(originY + ty * tileSize_ * scale),
                static_cast<float>(originX + (tx * tileSize_ + width) * scale),
                static_cast<float>(originY + (ty * tileSize_ + height) * scale));

            const auto found = tiles_.find({tx, ty});
            if (found != tiles_.end()) {
                const D2D1_SIZE_F tileSize = found->second->GetSize();
                const D2D1_RECT_F src = D2D1::RectF(0.0f, 0.0f, tileSize.width, tileSize.height);
                target->DrawBitmap(found->second.Get(), &dest, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
            } else {
                RequestTileIfNeeded(tx, ty, size);
            }
        }
    }
}

void Viewer::RequestTileIfNeeded(UINT tileX, UINT tileY, D2D1_SIZE_F) {
    const std::pair<UINT, UINT> key{tileX, tileY};
    if (tiles_.count(key) || tilesRequested_.count(key)) return;

    if (syncImage_) {  // capture mode: decode on this thread
        PixelBuffer pixels;
        if (SUCCEEDED(syncImage_->DecodeRect(tileX * tileSize_, tileY * tileSize_, tileSize_,
                                             tileSize_, pixels))) {
            ComPtr<ID2D1Bitmap> tile;
            if (CreateBitmap(pixels, tile)) tiles_[key] = tile;
        }
        return;
    }

    if (!decoder_ || tilesRequested_.size() >= kMaxTileRequests) return;
    tilesRequested_.insert(key);
    decoder_->RequestTile(path_, generation_, tileX, tileY, tileSize_);
}

void Viewer::SetFirstFrameCallback(std::function<void()> callback) {
    firstFrameCallback_ = std::move(callback);
}

void Viewer::SetImageReadyCallback(std::function<void()> callback) {
    imageReadyCallback_ = std::move(callback);
}
