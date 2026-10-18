// decode.cpp — WIC pipeline: decode, EXIF orientation, color management, tiles.
#include "decode.h"

#include <propidl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// EXIF orientations, verified empirically with tests/data/orientation{1..8}.jpg:
// WIC applies the flip first and the rotation afterwards, which is why 5 and 7
// look "swapped" compared with the naive rotate-then-flip derivation.
UINT OrientationToWicOptions(UINT orientation) {
    switch (orientation) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal;
        case 6: return WICBitmapTransformRotate90;
        case 7: return WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal;
        case 8: return WICBitmapTransformRotate270;
        default: return 0;
    }
}

UINT ReadExifOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader)) || !reader) return 1;

    const wchar_t* kPaths[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}",
                               L"/app1/ifd/exif/{ushort=274}"};
    for (const wchar_t* path : kPaths) {
        PROPVARIANT value;
        PropVariantInit(&value);
        const HRESULT hr = reader->GetMetadataByName(path, &value);
        UINT orientation = 1;
        if (SUCCEEDED(hr)) {
            if (value.vt == VT_UI2) orientation = value.uiVal;
            else if (value.vt == VT_UI4) orientation = value.ulVal;
        }
        PropVariantClear(&value);
        if (orientation >= 1 && orientation <= 8) return orientation;
    }
    return 1;
}

DWORD ReadBigEndian32(const BYTE* bytes) {
    return (static_cast<DWORD>(bytes[0]) << 24) | (static_cast<DWORD>(bytes[1]) << 16) |
           (static_cast<DWORD>(bytes[2]) << 8) | static_cast<DWORD>(bytes[3]);
}

// Reads the ICC 'desc' tag (v2 'desc' or v4 'mluc') and looks for "sRGB". Other
// profiles - Adobe RGB, Display P3, camera-specific - still get the transform; a
// profile without a readable description is treated conservatively.
bool IsSrgbProfile(IWICColorContext* context) {
    UINT size = 0;
    if (FAILED(context->GetProfileBytes(0, nullptr, &size)) || size < 132) return false;
    std::vector<BYTE> profile(size);
    if (FAILED(context->GetProfileBytes(size, profile.data(), &size)) || size < 132) return false;
    if (std::memcmp(profile.data() + 16, "RGB ", 4) != 0) return false;

    const DWORD tagCount = ReadBigEndian32(profile.data() + 128);
    for (DWORD i = 0; i < tagCount; ++i) {
        const size_t entry = 132 + static_cast<size_t>(i) * 12;
        if (entry + 12 > size) break;
        if (std::memcmp(profile.data() + entry, "desc", 4) != 0) continue;

        const DWORD offset = ReadBigEndian32(profile.data() + entry + 4);
        const DWORD length = ReadBigEndian32(profile.data() + entry + 8);
        if (offset > size || length > size - offset || length < 12) return false;
        const BYTE* tag = profile.data() + offset;

        std::string text;
        if (std::memcmp(tag, "desc", 4) == 0) {
            const DWORD count = ReadBigEndian32(tag + 8);
            if (count > 0 && count <= length - 12) {
                text.assign(reinterpret_cast<const char*>(tag + 12), count - 1);
            }
        } else if (std::memcmp(tag, "mluc", 4) == 0) {
            const DWORD records = ReadBigEndian32(tag + 8);
            if (records > 0) {
                const DWORD count = ReadBigEndian32(tag + 20);
                const DWORD charsOffset = ReadBigEndian32(tag + 24);
                if (charsOffset <= length && count <= length - charsOffset) {
                    for (DWORD k = 0; k + 1 < count; k += 2) {
                        const unsigned c = (static_cast<unsigned>(tag[charsOffset + k]) << 8) |
                                           tag[charsOffset + k + 1];
                        if (c > 0 && c < 128) text.push_back(static_cast<char>(c));
                    }
                }
            }
        }

        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text.find("srgb") != std::string::npos;
    }
    return false;
}

// True when the frame carries a real ICC profile that we should transform from.
// An EXIF colorspace of sRGB (1) needs no transform, and neither does an embedded
// sRGB profile: the transform is the identity, but WIC charges for it (about 60% on
// top of a 10 MP JPEG decode on this machine).
bool GetSourceColorContext(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                           IWICColorContext** out) {
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) return false;

    std::vector<ComPtr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw(count, nullptr);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(factory->CreateColorContext(&contexts[i]))) return false;
        raw[i] = contexts[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count))) return false;

    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type = WICColorContextUninitialized;
        if (FAILED(contexts[i]->GetType(&type))) continue;
        if (type == WICColorContextProfile) {
            if (IsSrgbProfile(contexts[i].Get())) return false;  // identity transform
            contexts[i].CopyTo(out);
            return true;
        }
        if (type == WICColorContextExifColorSpace) {
            UINT space = 0;
            if (SUCCEEDED(contexts[i]->GetExifColorSpace(&space)) && space == 1) {
                return false;  // already sRGB
            }
        }
    }
    return false;
}

struct Cancel {
    std::function<bool()> fn;

    bool operator()() const { return fn && fn(); }
};

bool BufferSizeFits(UINT64 bytes) { return bytes <= 0xFFFFFFFFull; }

UINT BytesPerPixelFor(const WICPixelFormatGUID& format) {
    if (IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) return 3;
    if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGR)) return 4;
    if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) return 4;
    if (IsEqualGUID(format, GUID_WICPixelFormat32bppPBGRA)) return 4;
    if (IsEqualGUID(format, GUID_WICPixelFormat48bppRGB)) return 6;
    if (IsEqualGUID(format, GUID_WICPixelFormat64bppRGBA)) return 8;
    return 0;
}

bool IsQuarterTurn(WICBitmapTransformOptions options) {
    return (options & (WICBitmapTransformRotate90 | WICBitmapTransformRotate270)) != 0;
}

}  // namespace

// Declared in decode.h; defined here at global scope so the unique_ptr member
// works with an incomplete type.
// A WIC session that keeps the decoder, the color transform and the orientation
// transform open so that tile requests do not re-open or re-decode the file.
class WicSession {
public:
    HRESULT Open(const std::wstring& path, const Cancel& cancel);
    const ImageMeta& Meta() const { return meta_; }
    bool LastOverviewWasNative() const { return lastOverviewNative_; }

    HRESULT DecodeFull(const Cancel& cancel, PixelBuffer& out, double* decodeMs);
    HRESULT DecodeOverview(UINT maxEdge, const Cancel& cancel, PixelBuffer& out,
                           double* pixelsPerImagePixel);
    HRESULT DecodeRect(UINT x, UINT y, UINT w, UINT h, const Cancel& cancel, PixelBuffer& out);

private:
    HRESULT MakeConverter(IWICBitmapSource* source, IWICFormatConverter** out);
    HRESULT CopyToBuffer(IWICBitmapSource* source, UINT w, UINT h, PixelBuffer& out);
    // Applies colour management and EXIF orientation to `input`, then converts to
    // premultiplied BGRA. Sizes reported are after the orientation transform.
    HRESULT ConvertSource(IWICBitmapSource* input, PixelBuffer& out, UINT* outW, UINT* outH);

    ComPtr<IWICImagingFactory> factory_;
    ComPtr<IWICBitmapDecoder> decoder_;
    ComPtr<IWICBitmapFrameDecode> frame_;
    ComPtr<IWICBitmapSource> oriented_;
    ComPtr<IWICColorContext> sourceColorContext_;
    WICBitmapTransformOptions orientationOptions_ = WICBitmapTransformRotate0;
    ImageMeta meta_;
    bool lastOverviewNative_ = false;
    UINT orientedWidth_ = 0;
    UINT orientedHeight_ = 0;
};

HRESULT WicSession::Open(const std::wstring& path, const Cancel& cancel) {
    if (factory_) return E_UNEXPECTED;

    HRESULT hr = CreateWicFactory(&factory_);
    if (FAILED(hr)) return hr;

    hr = factory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder_);
    if (FAILED(hr)) return hr;
    if (cancel()) return E_ABORT;

    hr = decoder_->GetFrame(0, &frame_);
    if (FAILED(hr)) return hr;

    UINT w = 0, h = 0;
    hr = frame_->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) return FAILED(hr) ? hr : WINCODEC_ERR_WRONGSTATE;
    meta_.srcWidth = w;
    meta_.srcHeight = h;
    meta_.orientation = ReadExifOrientation(frame_.Get());
    if (cancel()) return E_ABORT;

    ComPtr<IWICBitmapSource> source = frame_;

    if (GetSourceColorContext(factory_.Get(), frame_.Get(), &sourceColorContext_)) {
        ComPtr<IWICFormatConverter> toBgra;
        hr = factory_->CreateFormatConverter(&toBgra);
        if (SUCCEEDED(hr)) {
            hr = toBgra->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom);
        }
        ComPtr<IWICColorContext> destContext;
        ComPtr<IWICColorTransform> transform;
        if (SUCCEEDED(hr)) hr = factory_->CreateColorContext(&destContext);
        if (SUCCEEDED(hr)) hr = destContext->InitializeFromExifColorSpace(1);  // sRGB
        if (SUCCEEDED(hr)) hr = factory_->CreateColorTransformer(&transform);
        if (SUCCEEDED(hr)) {
            hr = transform->Initialize(toBgra.Get(), sourceColorContext_.Get(), destContext.Get(),
                                       GUID_WICPixelFormat32bppBGRA);
        }
        if (SUCCEEDED(hr)) {
            source = transform;
            meta_.colorManaged = true;
        }
    }
    if (cancel()) return E_ABORT;

    orientationOptions_ =
        static_cast<WICBitmapTransformOptions>(OrientationToWicOptions(meta_.orientation));
    if (orientationOptions_ != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        hr = factory_->CreateBitmapFlipRotator(&rotator);
        if (SUCCEEDED(hr)) hr = rotator->Initialize(source.Get(), orientationOptions_);
        if (FAILED(hr)) return hr;
        source = rotator;
    }

    hr = source->GetSize(&orientedWidth_, &orientedHeight_);
    if (FAILED(hr)) return hr;
    oriented_ = source;
    return S_OK;
}

HRESULT WicSession::MakeConverter(IWICBitmapSource* source, IWICFormatConverter** out) {
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = factory_->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;
    hr = converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                               nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    *out = converter.Detach();
    return S_OK;
}

HRESULT WicSession::CopyToBuffer(IWICBitmapSource* source, UINT w, UINT h, PixelBuffer& out) {
    const UINT64 bytes = static_cast<UINT64>(w) * h * 4;
    if (!BufferSizeFits(bytes)) return E_OUTOFMEMORY;
    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(bytes));
    const WICRect rect = {0, 0, static_cast<INT>(w), static_cast<INT>(h)};
    return source->CopyPixels(&rect, w * 4, static_cast<UINT>(bytes), out.pixels.data());
}

HRESULT WicSession::ConvertSource(IWICBitmapSource* input, PixelBuffer& out, UINT* outW,
                                  UINT* outH) {
    ComPtr<IWICBitmapSource> source = input;

    if (meta_.colorManaged && sourceColorContext_) {
        ComPtr<IWICFormatConverter> toBgra;
        HRESULT hr = factory_->CreateFormatConverter(&toBgra);
        if (FAILED(hr)) return hr;
        hr = toBgra->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return hr;

        ComPtr<IWICColorContext> destContext;
        ComPtr<IWICColorTransform> transform;
        hr = factory_->CreateColorContext(&destContext);
        if (SUCCEEDED(hr)) hr = destContext->InitializeFromExifColorSpace(1);
        if (SUCCEEDED(hr)) hr = factory_->CreateColorTransformer(&transform);
        if (SUCCEEDED(hr)) {
            hr = transform->Initialize(toBgra.Get(), sourceColorContext_.Get(), destContext.Get(),
                                       GUID_WICPixelFormat32bppBGRA);
        }
        if (FAILED(hr)) return hr;
        source = transform;
    }

    if (orientationOptions_ != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        HRESULT hr = factory_->CreateBitmapFlipRotator(&rotator);
        if (FAILED(hr)) return hr;
        hr = rotator->Initialize(source.Get(), orientationOptions_);
        if (FAILED(hr)) return hr;
        source = rotator;
    }

    UINT w = 0, h = 0;
    HRESULT hr = source->GetSize(&w, &h);
    if (FAILED(hr)) return hr;

    ComPtr<IWICFormatConverter> converter;
    hr = MakeConverter(source.Get(), &converter);
    if (SUCCEEDED(hr)) hr = CopyToBuffer(converter.Get(), w, h, out);
    if (SUCCEEDED(hr)) {
        if (outW) *outW = w;
        if (outH) *outH = h;
    }
    return hr;
}

HRESULT WicSession::DecodeFull(const Cancel& cancel, PixelBuffer& out, double* decodeMs) {
    const double start = NowMs();
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = MakeConverter(oriented_.Get(), &converter);
    if (SUCCEEDED(hr) && !cancel()) hr = CopyToBuffer(converter.Get(), orientedWidth_,
                                                      orientedHeight_, out);
    if (decodeMs) *decodeMs = NowMs() - start;
    return hr;
}

HRESULT WicSession::DecodeOverview(UINT maxEdge, const Cancel& cancel, PixelBuffer& out,
                                   double* pixelsPerImagePixel) {
    const UINT longest = std::max(orientedWidth_, orientedHeight_);
    if (longest == 0) return E_UNEXPECTED;

    double scale = 1.0;
    if (maxEdge > 0 && longest > maxEdge) scale = static_cast<double>(maxEdge) / longest;
    const UINT targetW = std::max<UINT>(1, static_cast<UINT>(orientedWidth_ * scale + 0.5));
    const UINT targetH = std::max<UINT>(1, static_cast<UINT>(orientedHeight_ * scale + 0.5));

    // Fast path: let the decoder produce a smaller image (JPEG supports DCT
    // scaling). This avoids decoding and touching every source pixel.
    ComPtr<IWICBitmapSourceTransform> transform;
    if (SUCCEEDED(frame_.As(&transform)) && !cancel()) {
        // The decoder works in pre-orientation space; swap the request back.
        const bool swapped = IsQuarterTurn(orientationOptions_);
        UINT wantW = swapped ? targetH : targetW;
        UINT wantH = swapped ? targetW : targetH;
        UINT gotW = wantW, gotH = wantH;
        if (SUCCEEDED(transform->GetClosestSize(&gotW, &gotH)) && gotW > 0 && gotH > 0) {
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
            if (FAILED(transform->GetClosestPixelFormat(&format))) {
                format = GUID_WICPixelFormat32bppBGR;
            }
            const UINT bytesPerPixel = BytesPerPixelFor(format);
            const UINT64 bytes = static_cast<UINT64>(gotW) * gotH * bytesPerPixel;
            if (bytesPerPixel != 0 && BufferSizeFits(bytes)) {
                std::vector<BYTE> buffer(static_cast<size_t>(bytes));
                HRESULT hr = transform->CopyPixels(nullptr, gotW, gotH, &format,
                                                   WICBitmapTransformRotate0, gotW * bytesPerPixel,
                                                   static_cast<UINT>(bytes), buffer.data());
                if (SUCCEEDED(hr) && !cancel()) {
                    ComPtr<IWICBitmap> scaled;
                    hr = factory_->CreateBitmapFromMemory(gotW, gotH, format, gotW * bytesPerPixel,
                                                          static_cast<UINT>(bytes), buffer.data(),
                                                          &scaled);
                    if (SUCCEEDED(hr)) {
                        UINT outW = 0, outH = 0;
                        hr = ConvertSource(scaled.Get(), out, &outW, &outH);
                        if (SUCCEEDED(hr)) {
                            if (pixelsPerImagePixel) {
                                *pixelsPerImagePixel =
                                    static_cast<double>(outW) / orientedWidth_;
                            }
                            lastOverviewNative_ = true;
                            return S_OK;
                        }
                    }
                }
            }
        }
    }
    if (cancel()) return E_ABORT;

    // Fallback: scale the decoded image.
    ComPtr<IWICBitmapScaler> scaler;
    HRESULT hr = factory_->CreateBitmapScaler(&scaler);
    if (FAILED(hr)) return hr;
    hr = scaler->Initialize(oriented_.Get(), targetW, targetH, WICBitmapInterpolationModeFant);
    if (FAILED(hr) || cancel()) return FAILED(hr) ? hr : E_ABORT;

    ComPtr<IWICFormatConverter> converter;
    hr = MakeConverter(scaler.Get(), &converter);
    if (SUCCEEDED(hr)) hr = CopyToBuffer(converter.Get(), targetW, targetH, out);
    if (SUCCEEDED(hr) && pixelsPerImagePixel) {
        *pixelsPerImagePixel = static_cast<double>(targetW) / orientedWidth_;
    }
    return hr;
}

HRESULT WicSession::DecodeRect(UINT x, UINT y, UINT w, UINT h, const Cancel& cancel,
                               PixelBuffer& out) {
    if (x >= orientedWidth_ || y >= orientedHeight_) return E_INVALIDARG;
    const UINT cw = std::min(w, orientedWidth_ - x);
    const UINT ch = std::min(h, orientedHeight_ - y);
    if (cw == 0 || ch == 0) return E_INVALIDARG;

    ComPtr<IWICBitmapClipper> clipper;
    HRESULT hr = factory_->CreateBitmapClipper(&clipper);
    if (FAILED(hr)) return hr;
    const WICRect rect = {static_cast<INT>(x), static_cast<INT>(y), static_cast<INT>(cw),
                          static_cast<INT>(ch)};
    hr = clipper->Initialize(oriented_.Get(), &rect);
    if (FAILED(hr) || cancel()) return FAILED(hr) ? hr : E_ABORT;

    ComPtr<IWICFormatConverter> converter;
    hr = MakeConverter(clipper.Get(), &converter);
    if (SUCCEEDED(hr)) hr = CopyToBuffer(converter.Get(), cw, ch, out);
    return hr;
}

void DownscaleToFit(const PixelBuffer& source, UINT maxEdge, PixelBuffer& out) {
    out.Reset();
    if (source.Empty() || maxEdge == 0) return;

    const UINT longest = std::max(source.width, source.height);
    if (longest <= maxEdge) {
        out = source;
        return;
    }

    const double scale = static_cast<double>(maxEdge) / longest;
    const UINT width = std::max<UINT>(1, static_cast<UINT>(source.width * scale + 0.5));
    const UINT height = std::max<UINT>(1, static_cast<UINT>(source.height * scale + 0.5));

    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height * 4);

    const size_t sourceStride = source.Stride();
    for (UINT y = 0; y < height; ++y) {
        const UINT y0 = static_cast<UINT>(static_cast<UINT64>(y) * source.height / height);
        const UINT y1 = std::max<UINT>(y0 + 1,
                                       static_cast<UINT>(static_cast<UINT64>(y + 1) *
                                                         source.height / height));
        for (UINT x = 0; x < width; ++x) {
            const UINT x0 = static_cast<UINT>(static_cast<UINT64>(x) * source.width / width);
            const UINT x1 = std::max<UINT>(x0 + 1,
                                           static_cast<UINT>(static_cast<UINT64>(x + 1) *
                                                             source.width / width));
            UINT64 b = 0, g = 0, r = 0, a = 0;
            UINT count = 0;
            for (UINT sy = y0; sy < y1; ++sy) {
                const BYTE* row = source.pixels.data() + static_cast<size_t>(sy) * sourceStride;
                for (UINT sx = x0; sx < x1; ++sx) {
                    const BYTE* pixel = row + static_cast<size_t>(sx) * 4;
                    b += pixel[0];
                    g += pixel[1];
                    r += pixel[2];
                    a += pixel[3];
                    ++count;
                }
            }
            BYTE* dest = out.pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            dest[0] = static_cast<BYTE>(b / count);
            dest[1] = static_cast<BYTE>(g / count);
            dest[2] = static_cast<BYTE>(r / count);
            dest[3] = static_cast<BYTE>(a / count);
        }
    }
}

HRESULT CreateWicFactory(IWICImagingFactory** factory) {
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(factory));
}

Decoder::Decoder(HWND notify, UINT message) : notify_(notify), message_(message) {}

Decoder::~Decoder() { Stop(); }

void Decoder::Start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread(&Decoder::ThreadMain, this);
    prefetchThreads_[0] = std::thread(&Decoder::PrefetchThreadMain, this,
                                      PrefetchDirection::Forward);
    prefetchThreads_[1] = std::thread(&Decoder::PrefetchThreadMain, this,
                                      PrefetchDirection::Backward);
}

void Decoder::Stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(prefetchMutex_);
        prefetchQueues_[0].clear();
        prefetchQueues_[1].clear();
    }
    cv_.notify_all();
    prefetchCv_.notify_all();
    thread_.join();
    for (auto& prefetchThread : prefetchThreads_) {
        if (prefetchThread.joinable()) prefetchThread.join();
    }
}

void Decoder::Reset(UINT generation) {
    currentGeneration_.store(generation);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    cv_.notify_all();
}

void Decoder::RequestOpen(const std::wstring& path, UINT generation, UINT maxTextureEdge,
                          UINT cacheEdge) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (queue_.size() >= 4) queue_.pop_front();
        Job job;
        job.type = Job::Type::Open;
        job.path = path;
        job.generation = generation;
        job.maxTextureEdge = maxTextureEdge;
        job.cacheEdge = cacheEdge;
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void Decoder::RequestPrefetch(const std::wstring& path, UINT maxEdge,
                              PrefetchDirection direction) {
    const size_t index = direction == PrefetchDirection::Forward ? 0 : 1;
    {
        std::lock_guard<std::mutex> lock(prefetchMutex_);
        if (prefetchQueues_[index].size() >= 20) return;
        Job job;
        job.type = Job::Type::Prefetch;
        job.path = path;
        job.maxEdge = maxEdge;
        prefetchQueues_[index].push_back(std::move(job));
    }
    prefetchCv_.notify_all();
}

void Decoder::ClearPrefetchQueue() {
    std::lock_guard<std::mutex> lock(prefetchMutex_);
    prefetchQueues_[0].clear();
    prefetchQueues_[1].clear();
}

void Decoder::RequestTile(const std::wstring& path, UINT generation, UINT tileX, UINT tileY,
                          UINT tileSize) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (queue_.size() >= 16) queue_.pop_front();
        Job job;
        job.type = Job::Type::Tile;
        job.path = path;
        job.generation = generation;
        job.tileX = tileX;
        job.tileY = tileY;
        job.tileSize = tileSize;
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

bool Decoder::IsCancelled(UINT generation) const {
    return stopping_.load() || currentGeneration_.load() != generation;
}

void Decoder::ThreadMain() {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) break;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        if (IsCancelled(job.generation)) continue;

        if (job.type == Job::Type::Open) HandleOpen(job);
        else HandleTile(job);
    }

    session_.reset();
    if (SUCCEEDED(hrCom)) CoUninitialize();
}

void Decoder::PrefetchThreadMain(PrefetchDirection direction) {
    const size_t index = direction == PrefetchDirection::Forward ? 0 : 1;
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(prefetchMutex_);
            prefetchCv_.wait(lock, [this, index] {
                return stopping_.load() || !prefetchQueues_[index].empty();
            });
            if (stopping_) break;
            job = std::move(prefetchQueues_[index].front());
            prefetchQueues_[index].pop_front();
        }
        HandlePrefetch(job);
    }

    if (SUCCEEDED(hrCom)) CoUninitialize();
}

void Decoder::PostResult(DecodeResult&& result) {
    auto* payload = new DecodeResult(std::move(result));
    if (!PostMessageW(notify_, message_, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void Decoder::HandleOpen(const Job& job) {
    const double start = NowMs();
    const UINT generation = job.generation;

    session_.reset();
    session_ = std::make_unique<WicSession>();

    Cancel cancel{[this, generation] { return IsCancelled(generation); }};

    DecodeResult result;
    result.generation = generation;
    result.path = job.path;
    result.kind = DecodeKind::Full;

    const double openStart = NowMs();
    result.hr = session_->Open(job.path, cancel);
    result.openMs = NowMs() - openStart;
    if (IsCancelled(generation)) return;  // silently drop obsolete work

    if (SUCCEEDED(result.hr)) {
        result.meta = session_->Meta();
        const UINT longest = std::max(result.meta.srcWidth, result.meta.srcHeight);
        const UINT edge = job.maxTextureEdge ? job.maxTextureEdge : 16384;

        if (longest > edge) {
            // Too large for one GPU texture: publish a light overview, then tiles.
            const UINT overviewEdge = std::min<UINT>(2048, edge);
            result.kind = DecodeKind::Overview;
            const double decodeStart = NowMs();
            result.hr = session_->DecodeOverview(overviewEdge, cancel, result.pixels,
                                                 &result.overviewPixelsPerImagePixel);
            result.decodeMs = NowMs() - decodeStart;
        } else {
            const double decodeStart = NowMs();
            result.hr = session_->DecodeFull(cancel, result.pixels, &result.decodeMs);
            result.decodeMs = NowMs() - decodeStart;
        }

        // Screen-sized copy for the session cache.
        if (SUCCEEDED(result.hr) && job.cacheEdge > 0 && !result.pixels.Empty()) {
            DownscaleToFit(result.pixels, job.cacheEdge, result.cachePreview);
        }
    }

    if (IsCancelled(generation)) return;
    result.totalMs = NowMs() - start;
    if (FAILED(result.hr)) {
        result.pixels.Reset();
        result.cachePreview.Reset();
        result.kind = DecodeKind::Full;
    }
    PostResult(std::move(result));
}

void Decoder::HandlePrefetch(const Job& job) {
    if (stopping_.load()) return;

    const double start = NowMs();
    WicSession session;
    Cancel cancel{[this] { return stopping_.load(); }};

    DecodeResult result;
    result.generation = currentGeneration_.load();
    result.path = job.path;
    result.kind = DecodeKind::Prefetch;

    result.hr = session.Open(job.path, cancel);
    if (SUCCEEDED(result.hr)) {
        result.meta = session.Meta();
        PixelBuffer scaled;
        double ignored = 1.0;
        result.hr = session.DecodeOverview(job.maxEdge, cancel, scaled, &ignored);
        if (SUCCEEDED(result.hr)) result.cachePreview = std::move(scaled);
    }
    result.totalMs = NowMs() - start;
    if (stopping_.load() || FAILED(result.hr)) return;
    PostResult(std::move(result));
}

void Decoder::HandleTile(const Job& job) {
    if (!session_ || IsCancelled(job.generation)) return;

    const double start = NowMs();
    Cancel cancel{[this, generation = job.generation] { return IsCancelled(generation); }};

    DecodeResult result;
    result.generation = job.generation;
    result.kind = DecodeKind::Tile;
    result.tileX = job.tileX;
    result.tileY = job.tileY;
    result.tileSize = job.tileSize;
    result.meta = session_->Meta();
    result.hr = session_->DecodeRect(job.tileX * job.tileSize, job.tileY * job.tileSize,
                                     job.tileSize, job.tileSize, cancel, result.pixels);
    result.totalMs = NowMs() - start;
    result.decodeMs = result.totalMs;
    if (FAILED(result.hr) || IsCancelled(job.generation)) return;
    PostResult(std::move(result));
}

// ---------------------------------------------------------------------------
// SyncImage: synchronous decoding on the caller's thread (offscreen capture).
// ---------------------------------------------------------------------------

struct SyncImage::Impl {
    WicSession session;
    Cancel cancel;  // never cancelled
};

SyncImage::SyncImage() : impl_(std::make_unique<Impl>()) {}
SyncImage::~SyncImage() = default;

HRESULT SyncImage::Open(const std::wstring& path) { return impl_->session.Open(path, impl_->cancel); }

const ImageMeta& SyncImage::Meta() const { return impl_->session.Meta(); }

bool SyncImage::IsOpen() const { return impl_->session.Meta().srcWidth != 0; }

void SyncImage::Close() { impl_->session = WicSession(); }

HRESULT SyncImage::DecodeFull(PixelBuffer& out) {
    return impl_->session.DecodeFull(impl_->cancel, out, nullptr);
}

HRESULT SyncImage::DecodeOverview(UINT maxEdge, PixelBuffer& out, double* pixelsPerImagePixel) {
    return impl_->session.DecodeOverview(maxEdge, impl_->cancel, out, pixelsPerImagePixel);
}

HRESULT SyncImage::DecodeRect(UINT x, UINT y, UINT w, UINT h, PixelBuffer& out) {
    return impl_->session.DecodeRect(x, y, w, h, impl_->cancel, out);
}
