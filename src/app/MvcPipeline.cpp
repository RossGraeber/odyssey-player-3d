#include "MvcPipeline.h"
#include "BluRayTitles.h"
#include "IsoMount.h"
#include "MvcSubtitleTiming.h"
#include "SubtitleDecoder.h"

#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/client.h>

#include "IMediaSample3D.h"
#include "LAVSplitterSettings.h"
#include "LAVVideoSettings.h"
#include "LAVAudioSettings.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <deque>
#include <filesystem>
#include <limits>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

constexpr GUID kClsidLavVideo = {0xee30215d, 0x164f, 0x4a92, {0xa4, 0xeb, 0x9d, 0x4c, 0x13, 0x39, 0x0f, 0x9f}};
constexpr GUID kClsidLavSplitterSource = {0xb98d13e7, 0x55db, 0x4385,
                                          {0xa3, 0x3d, 0x09, 0xfd, 0x1b, 0xa2, 0x63, 0x38}};
constexpr GUID kClsidLavAudio = {0xe8e73b6b, 0x4cb3, 0x44a4,
                                 {0xbe, 0x99, 0x4f, 0x7b, 0xcb, 0x96, 0xe4, 0x91}};
constexpr GUID kClsidFrameSink = {0x8ba75392, 0xba23, 0x41ad,
                                  {0xb2, 0x1c, 0xd9, 0xb4, 0xcf, 0xd3, 0x8b, 0xf1}};
constexpr GUID kClsidSubtitleSink = {0x19f82647, 0x4b7d, 0x4180,
                                     {0xa7, 0x8d, 0xe2, 0x1b, 0x49, 0xc9, 0xd3, 0x17}};
constexpr GUID kMediaTypeSubtitle = {0xe487eb08, 0x6b26, 0x4be9,
                                     {0x9d, 0xd3, 0x99, 0x34, 0x34, 0xd3, 0x13, 0xfd}};
constexpr GUID kMediaSubtypeHdmvSub = {0x04eba53e, 0x9330, 0x436c,
                                       {0x91, 0x33, 0x55, 0x3e, 0xc8, 0x70, 0x31, 0xdc}};
constexpr GUID kFormatSubtitleInfo = {0xa33d2f7d, 0x96bc, 0x4337,
                                      {0xb2, 0x3b, 0xa8, 0xb9, 0xfb, 0xc2, 0x95, 0xe9}};
constexpr REFERENCE_TIME kTicksPerSecond = 10'000'000;
constexpr std::size_t kMaximumSubtitleCues = 1'024;
constexpr std::size_t kMaximumSubtitleBytes = 64 * 1024 * 1024;
constexpr int64_t kMaximumSubtitleOffsetNs = 60'000'000'000;

#pragma pack(push, 1)
struct SubtitleInfoFormat {
    DWORD offset;
    CHAR isoLanguage[4];
    WCHAR trackName[256];
};
#pragma pack(pop)

bool checkedAdd(REFERENCE_TIME left, REFERENCE_TIME right, REFERENCE_TIME* result) {
    if (!result || (right > 0 && left > std::numeric_limits<REFERENCE_TIME>::max() - right) ||
        (right < 0 && left < std::numeric_limits<REFERENCE_TIME>::min() - right)) {
        return false;
    }
    *result = left + right;
    return true;
}

bool checkedSubtract(REFERENCE_TIME left, REFERENCE_TIME right, REFERENCE_TIME* result) {
    if (!result || (right < 0 && left > std::numeric_limits<REFERENCE_TIME>::max() + right) ||
        (right > 0 && left < std::numeric_limits<REFERENCE_TIME>::min() + right)) {
        return false;
    }
    *result = left - right;
    return true;
}

int64_t saturatingAddNanoseconds(int64_t left, int64_t right) {
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return std::numeric_limits<int64_t>::min();
    return left + right;
}

std::size_t subtitleCueBytes(const odyssey::SubtitleCue& cue) {
    std::size_t result = cue.textUtf8.size();
    for (const odyssey::SubtitleBitmap& bitmap : cue.bitmaps) {
        if (bitmap.pixels.size() > std::numeric_limits<std::size_t>::max() - result)
            return std::numeric_limits<std::size_t>::max();
        result += bitmap.pixels.size();
    }
    return result;
}

void freeMediaType(AM_MEDIA_TYPE& type) {
    CoTaskMemFree(type.pbFormat);
    type.pbFormat = nullptr;
    type.cbFormat = 0;
    if (type.pUnk) {
        type.pUnk->Release();
        type.pUnk = nullptr;
    }
}

HRESULT copyMediaType(AM_MEDIA_TYPE& destination, const AM_MEDIA_TYPE& source) {
    destination = source;
    destination.pbFormat = nullptr;
    destination.pUnk = nullptr;
    if (source.cbFormat != 0) {
        destination.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(source.cbFormat));
        if (!destination.pbFormat) return E_OUTOFMEMORY;
        std::memcpy(destination.pbFormat, source.pbFormat, source.cbFormat);
    }
    if (source.pUnk) {
        source.pUnk->AddRef();
        destination.pUnk = source.pUnk;
    }
    return S_OK;
}

bool sourceRectStartsAtOrigin(const AM_MEDIA_TYPE& type) {
    const RECT* source = nullptr;
    if (type.formattype == FORMAT_VideoInfo2 && type.pbFormat &&
        type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        source = &reinterpret_cast<const VIDEOINFOHEADER2*>(type.pbFormat)->rcSource;
    } else if (type.formattype == FORMAT_VideoInfo && type.pbFormat &&
               type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        source = &reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat)->rcSource;
    }
    return source && source->left == 0 && source->top == 0;
}

class StereoAllocator;

class StereoSample final : public IMediaSample3D {
public:
    StereoSample(StereoAllocator* allocator, long size, long alignment, long prefix)
        : allocator_(allocator), size_(size) {
        const size_t padding = static_cast<size_t>(std::max<long>(alignment, 1));
        primaryStorage_.resize(static_cast<size_t>(size + prefix) + padding);
        stereoStorage_.resize(static_cast<size_t>(size + prefix) + padding);
        primary_ = alignedBuffer(primaryStorage_, alignment, prefix);
        stereo_ = alignedBuffer(stereoStorage_, alignment, prefix);
        reset();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IMediaSample) ||
            riid == __uuidof(IMediaSample2) || riid == __uuidof(IMediaSample3D)) {
            *object = static_cast<IMediaSample3D*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetPointer(BYTE** buffer) override {
        if (!buffer) return E_POINTER;
        *buffer = primary_;
        return S_OK;
    }
    STDMETHODIMP_(long) GetSize() override { return size_; }
    STDMETHODIMP GetTime(REFERENCE_TIME* start, REFERENCE_TIME* stop) override {
        if (!(properties_.dwSampleFlags & AM_SAMPLE_TIMEVALID)) return VFW_E_SAMPLE_TIME_NOT_SET;
        if (start) *start = properties_.tStart;
        if (stop) *stop = properties_.tStop;
        return (properties_.dwSampleFlags & AM_SAMPLE_STOPVALID) ? S_OK : VFW_S_NO_STOP_TIME;
    }
    STDMETHODIMP SetTime(REFERENCE_TIME* start, REFERENCE_TIME* stop) override {
        properties_.dwSampleFlags &= ~(AM_SAMPLE_TIMEVALID | AM_SAMPLE_STOPVALID);
        if (start) {
            properties_.tStart = *start;
            properties_.dwSampleFlags |= AM_SAMPLE_TIMEVALID;
        }
        if (stop) {
            properties_.tStop = *stop;
            properties_.dwSampleFlags |= AM_SAMPLE_STOPVALID;
        }
        return S_OK;
    }
    STDMETHODIMP IsSyncPoint() override {
        return (properties_.dwSampleFlags & AM_SAMPLE_SPLICEPOINT) ? S_OK : S_FALSE;
    }
    STDMETHODIMP SetSyncPoint(BOOL sync) override {
        setFlag(AM_SAMPLE_SPLICEPOINT, sync);
        return S_OK;
    }
    STDMETHODIMP IsPreroll() override {
        return (properties_.dwSampleFlags & AM_SAMPLE_PREROLL) ? S_OK : S_FALSE;
    }
    STDMETHODIMP SetPreroll(BOOL preroll) override {
        setFlag(AM_SAMPLE_PREROLL, preroll);
        return S_OK;
    }
    STDMETHODIMP_(long) GetActualDataLength() override { return properties_.lActual; }
    STDMETHODIMP SetActualDataLength(long length) override {
        if (length < 0 || length > size_) return VFW_E_BUFFER_OVERFLOW;
        properties_.lActual = length;
        return S_OK;
    }
    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE** type) override {
        if (!type) return E_POINTER;
        *type = nullptr;
        if (!mediaType_) return S_FALSE;
        auto* copy = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!copy) return E_OUTOFMEMORY;
        std::memset(copy, 0, sizeof(*copy));
        const HRESULT hr = copyMediaType(*copy, *mediaType_);
        if (FAILED(hr)) {
            CoTaskMemFree(copy);
            return hr;
        }
        *type = copy;
        return S_OK;
    }
    STDMETHODIMP SetMediaType(AM_MEDIA_TYPE* type) override {
        clearMediaType();
        if (!type) {
            properties_.dwSampleFlags &= ~AM_SAMPLE_TYPECHANGED;
            return S_OK;
        }
        mediaType_ = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!mediaType_) return E_OUTOFMEMORY;
        std::memset(mediaType_, 0, sizeof(*mediaType_));
        const HRESULT hr = copyMediaType(*mediaType_, *type);
        if (FAILED(hr)) {
            CoTaskMemFree(mediaType_);
            mediaType_ = nullptr;
            return hr;
        }
        properties_.dwSampleFlags |= AM_SAMPLE_TYPECHANGED;
        properties_.pMediaType = mediaType_;
        return S_OK;
    }
    STDMETHODIMP IsDiscontinuity() override {
        return (properties_.dwSampleFlags & AM_SAMPLE_DATADISCONTINUITY) ? S_OK : S_FALSE;
    }
    STDMETHODIMP SetDiscontinuity(BOOL discontinuity) override {
        setFlag(AM_SAMPLE_DATADISCONTINUITY, discontinuity);
        return S_OK;
    }
    STDMETHODIMP GetMediaTime(LONGLONG* start, LONGLONG* stop) override {
        if (!mediaTimeValid_) return VFW_E_MEDIA_TIME_NOT_SET;
        if (start) *start = mediaStart_;
        if (stop) *stop = mediaStop_;
        return S_OK;
    }
    STDMETHODIMP SetMediaTime(LONGLONG* start, LONGLONG* stop) override {
        mediaTimeValid_ = false;
        if (start) {
            mediaStart_ = *start;
            mediaStop_ = stop ? *stop : *start + 1;
            mediaTimeValid_ = true;
        }
        return S_OK;
    }
    STDMETHODIMP GetProperties(DWORD length, BYTE* properties) override {
        if (!properties || length < sizeof(AM_SAMPLE2_PROPERTIES)) return E_INVALIDARG;
        properties_.pMediaType = mediaType_;
        std::memcpy(properties, &properties_, sizeof(properties_));
        return S_OK;
    }
    STDMETHODIMP SetProperties(DWORD length, const BYTE* properties) override {
        if (!properties || length < sizeof(AM_SAMPLE2_PROPERTIES)) return E_INVALIDARG;
        const auto* incoming = reinterpret_cast<const AM_SAMPLE2_PROPERTIES*>(properties);
        const HRESULT hr = SetMediaType(incoming->pMediaType);
        if (FAILED(hr)) return hr;
        properties_.dwTypeSpecificFlags = incoming->dwTypeSpecificFlags;
        properties_.dwSampleFlags = incoming->dwSampleFlags;
        properties_.lActual = incoming->lActual;
        properties_.tStart = incoming->tStart;
        properties_.tStop = incoming->tStop;
        properties_.dwStreamId = incoming->dwStreamId;
        properties_.pMediaType = mediaType_;
        properties_.pbBuffer = primary_;
        properties_.cbBuffer = size_;
        return S_OK;
    }
    STDMETHODIMP Enable3D() override {
        stereoEnabled_ = true;
        return S_OK;
    }
    STDMETHODIMP GetPointer3D(BYTE** buffer) override {
        if (!buffer) return E_POINTER;
        *buffer = nullptr;
        if (!stereoEnabled_) return VFW_E_WRONG_STATE;
        *buffer = stereo_;
        return S_OK;
    }

    bool stereoEnabled() const { return stereoEnabled_; }
    BYTE* stereoBuffer() const { return stereo_; }

    void reset() {
        clearMediaType();
        std::memset(&properties_, 0, sizeof(properties_));
        properties_.cbData = sizeof(properties_);
        properties_.dwStreamId = AM_STREAM_MEDIA;
        properties_.pbBuffer = primary_;
        properties_.cbBuffer = size_;
        stereoEnabled_ = false;
        mediaStart_ = 0;
        mediaStop_ = 0;
        mediaTimeValid_ = false;
    }

    ~StereoSample() { clearMediaType(); }

private:
    static BYTE* alignedBuffer(std::vector<BYTE>& storage, long alignment, long prefix) {
        const uintptr_t candidate = reinterpret_cast<uintptr_t>(storage.data()) +
                                    static_cast<uintptr_t>(std::max<long>(prefix, 0));
        const uintptr_t align = static_cast<uintptr_t>(std::max<long>(alignment, 1));
        return reinterpret_cast<BYTE*>(((candidate + align - 1) / align) * align);
    }
    void setFlag(DWORD flag, BOOL enabled) {
        if (enabled) properties_.dwSampleFlags |= flag;
        else properties_.dwSampleFlags &= ~flag;
    }
    void clearMediaType() {
        if (mediaType_) {
            freeMediaType(*mediaType_);
            CoTaskMemFree(mediaType_);
            mediaType_ = nullptr;
        }
    }

    StereoAllocator* allocator_;
    std::atomic<ULONG> references_{0};
    long size_;
    std::vector<BYTE> primaryStorage_;
    std::vector<BYTE> stereoStorage_;
    BYTE* primary_{nullptr};
    BYTE* stereo_{nullptr};
    bool stereoEnabled_{false};
    AM_SAMPLE2_PROPERTIES properties_{};
    AM_MEDIA_TYPE* mediaType_{nullptr};
    LONGLONG mediaStart_{0};
    LONGLONG mediaStop_{0};
    bool mediaTimeValid_{false};
};

class StereoAllocator final : public IMemAllocator {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IMemAllocator)) {
            *object = static_cast<IMemAllocator*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    STDMETHODIMP SetProperties(ALLOCATOR_PROPERTIES* request, ALLOCATOR_PROPERTIES* actual) override {
        if (!request || !actual) return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        if (committed_) return VFW_E_ALREADY_COMMITTED;
        if (request->cbBuffer <= 0) return E_INVALIDARG;
        properties_ = *request;
        properties_.cBuffers = std::clamp<long>(request->cBuffers, 4, 32);
        properties_.cbAlign = std::max<long>(request->cbAlign, 1);
        properties_.cbPrefix = std::max<long>(request->cbPrefix, 0);
        *actual = properties_;
        return S_OK;
    }
    STDMETHODIMP GetProperties(ALLOCATOR_PROPERTIES* properties) override {
        if (!properties) return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        *properties = properties_;
        return S_OK;
    }
    STDMETHODIMP Commit() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (committed_) return S_OK;
        if (properties_.cbBuffer <= 0) return VFW_E_SIZENOTSET;
        if (free_.size() != samples_.size()) return VFW_E_BUFFERS_OUTSTANDING;
        samples_.clear();
        free_.clear();
        try {
            for (long i = 0; i < properties_.cBuffers; ++i) {
                samples_.emplace_back(new StereoSample(this, properties_.cbBuffer,
                                                       properties_.cbAlign, properties_.cbPrefix));
                free_.push_back(samples_.back().get());
            }
        } catch (const std::bad_alloc&) {
            samples_.clear();
            free_.clear();
            return E_OUTOFMEMORY;
        }
        committed_ = true;
        return S_OK;
    }
    STDMETHODIMP Decommit() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            committed_ = false;
        }
        available_.notify_all();
        return S_OK;
    }
    STDMETHODIMP GetBuffer(IMediaSample** sample, REFERENCE_TIME* start, REFERENCE_TIME* stop,
                           DWORD flags) override {
        if (!sample) return E_POINTER;
        *sample = nullptr;
        std::unique_lock<std::mutex> lock(mutex_);
        while (committed_ && free_.empty()) {
            if (flags & AM_GBF_NOWAIT) return VFW_E_TIMEOUT;
            available_.wait(lock);
        }
        if (!committed_) return VFW_E_NOT_COMMITTED;
        StereoSample* next = free_.back();
        free_.pop_back();
        next->reset();
        next->AddRef();
        if (start || stop) next->SetTime(start, stop);
        *sample = next;
        return S_OK;
    }
    STDMETHODIMP ReleaseBuffer(IMediaSample* sample) override {
        if (!sample) return E_POINTER;
        auto* stereo = dynamic_cast<StereoSample*>(sample);
        if (!stereo) return E_INVALIDARG;
        stereo->reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            free_.push_back(stereo);
        }
        available_.notify_one();
        return S_OK;
    }

private:
    ~StereoAllocator() = default;
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    std::condition_variable available_;
    ALLOCATOR_PROPERTIES properties_{4, 0, 1, 0};
    bool committed_{false};
    std::vector<std::unique_ptr<StereoSample>> samples_;
    std::vector<StereoSample*> free_;
};

STDMETHODIMP_(ULONG) StereoSample::Release() {
    const ULONG left = --references_;
    if (left == 0) allocator_->ReleaseBuffer(this);
    return left;
}

struct VideoFormat {
    long activeWidth{0};
    long activeHeight{0};
    long stride{0};
    long storageHeight{0};
    REFERENCE_TIME frameDuration{0};
    GUID subtype{};
    GUID formatType{};
    ULONG formatBytes{0};
};

class FrameState {
public:
    explicit FrameState(std::size_t capacity)
        : capacity_(std::max<std::size_t>(capacity, 1)) {}

    ~FrameState() { stop(); }

    HRESULT setFormat(const AM_MEDIA_TYPE& type) {
        VideoFormat format{};
        format.subtype = type.subtype;
        format.formatType = type.formattype;
        const RECT* source = nullptr;
        const BITMAPINFOHEADER* bitmap = nullptr;
        if (type.formattype == FORMAT_VideoInfo2 && type.pbFormat &&
            type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
            const auto* info = reinterpret_cast<const VIDEOINFOHEADER2*>(type.pbFormat);
            source = &info->rcSource;
            bitmap = &info->bmiHeader;
            format.frameDuration = info->AvgTimePerFrame;
        } else if (type.formattype == FORMAT_VideoInfo && type.pbFormat &&
                   type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
            const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat);
            source = &info->rcSource;
            bitmap = &info->bmiHeader;
            format.frameDuration = info->AvgTimePerFrame;
        }
        if (!bitmap || !source || source->left != 0 || source->top != 0) {
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        format.stride = bitmap->biWidth;
        format.storageHeight = std::abs(bitmap->biHeight);
        format.activeWidth = source->right > 0 ? source->right : bitmap->biWidth;
        format.activeHeight = source->bottom > 0 ? source->bottom : format.storageHeight;
        if (!validFormat(format) || format.activeWidth > std::numeric_limits<int>::max() / 2) {
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        format_ = format;
        width_ = format.activeWidth * 2;
        height_ = format.activeHeight;
        return S_OK;
    }

    HRESULT receive(IMediaSample* sample) {
        if (!sample) return E_POINTER;
        AM_MEDIA_TYPE* changed = nullptr;
        if (sample->GetMediaType(&changed) == S_OK && changed) {
            const HRESULT formatHr = setFormat(*changed);
            freeMediaType(*changed);
            CoTaskMemFree(changed);
            if (FAILED(formatHr)) return formatHr;
        }

        BYTE* primary = nullptr;
        ComPtr<IMediaSample3D> sample3d;
        const HRESULT pointerHr = sample->GetPointer(&primary);
        sample->QueryInterface(IID_PPV_ARGS(&sample3d));
        auto* owned = sample3d ? dynamic_cast<StereoSample*>(sample3d.Get()) : nullptr;
        BYTE* dependent = nullptr;
        REFERENCE_TIME start = 0;
        REFERENCE_TIME stop = 0;
        const HRESULT timeHr = sample->GetTime(&start, &stop);

        VideoFormat format{};
        REFERENCE_TIME segmentStart = 0;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || flushing_) return S_FALSE;
            format = format_;
            segmentStart = segmentStart_;
            generation = generation_;
        }
        if (FAILED(pointerHr) || !primary || !owned || !owned->stereoEnabled() ||
            FAILED(sample3d->GetPointer3D(&dependent)) || !dependent) {
            return fail("LAV Video did not deliver an enabled MVC view pair");
        }
        if (FAILED(timeHr)) return fail("MVC sample is missing its presentation timestamp");
        if (!validFormat(format) || !buffersFit(sample->GetSize(), format)) {
            return fail("LAV Video delivered a malformed NV12 view pair");
        }

        REFERENCE_TIME absolutePts = 0;
        if (!checkedAdd(segmentStart, start, &absolutePts)) {
            return fail("MVC presentation timestamp overflowed");
        }
        REFERENCE_TIME sampleDuration = format.frameDuration;
        if (stop > start) {
            if (!checkedSubtract(stop, start, &sampleDuration)) {
                return fail("MVC sample duration overflowed");
            }
        }

        struct FrameDeleter {
            void operator()(AVFrame* frame) const { av_frame_free(&frame); }
        };
        std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
        if (!frame) return E_OUTOFMEMORY;
        frame->format = AV_PIX_FMT_NV12;
        frame->width = format.activeWidth * 2;
        frame->height = format.activeHeight;
        frame->pts = absolutePts;
        frame->best_effort_timestamp = absolutePts;
        frame->duration = sampleDuration;
        frame->time_base = AVRational{1, static_cast<int>(kTicksPerSecond)};
        frame->sample_aspect_ratio = AVRational{1, 1};
        frame->color_primaries = AVCOL_PRI_BT709;
        frame->color_trc = AVCOL_TRC_BT709;
        frame->colorspace = AVCOL_SPC_BT709;
        frame->color_range = AVCOL_RANGE_MPEG;
        if (av_frame_get_buffer(frame.get(), 32) < 0) {
            return E_OUTOFMEMORY;
        }
        copyPair(primary, dependent, format, frame.get());

        std::unique_lock<std::mutex> lock(mutex_);
        available_.wait(lock, [&] {
            return stopping_ || flushing_ || generation != generation_ ||
                   frames_.size() < capacity_;
        });
        if (stopping_ || flushing_ || generation != generation_) {
            return S_FALSE;
        }
        try {
            frames_.push_back(frame.get());
        } catch (const std::bad_alloc&) {
            return failLocked("MVC frame queue allocation failed", E_OUTOFMEMORY);
        } catch (...) {
            return failLocked("MVC frame queue failed", E_FAIL);
        }
        frame.release();
        decodedFrames_++;
        return S_OK;
    }

    void beginFlush() {
        std::lock_guard<std::mutex> lock(mutex_);
        flushing_ = true;
        ++generation_;
        clearLocked();
        available_.notify_all();
    }

    void endFlush() {
        std::lock_guard<std::mutex> lock(mutex_);
        flushing_ = false;
        videoEos_ = false;
        available_.notify_all();
    }

    HRESULT newSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) {
        if (start < 0 || stop < start || !std::isfinite(rate) || rate <= 0.0) {
            return callbackFailure("MVC segment timing is invalid", E_INVALIDARG);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        segmentStart_ = start;
        ++segmentCount_;
        videoEos_ = false;
        return S_OK;
    }

    void run(REFERENCE_TIME referenceStart) {
        std::lock_guard<std::mutex> lock(mutex_);
        runReferenceStart_ = referenceStart;
        haveRunReference_ = true;
        ++runCount_;
    }

    bool mediaPosition(REFERENCE_TIME referenceNow, REFERENCE_TIME* position) const {
        if (!position) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!haveRunReference_) return false;
        REFERENCE_TIME elapsed = 0;
        if (referenceNow > runReferenceStart_ &&
            !checkedSubtract(referenceNow, runReferenceStart_, &elapsed)) {
            return false;
        }
        return checkedAdd(segmentStart_, elapsed, position);
    }

    void endOfStream() {
        std::lock_guard<std::mutex> lock(mutex_);
        videoEos_ = true;
    }

    AVFrame* poll() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) return nullptr;
        AVFrame* frame = frames_.front();
        frames_.pop_front();
        available_.notify_one();
        return frame;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        clearLocked();
        available_.notify_all();
    }

    void cancelForCommand() {
        std::lock_guard<std::mutex> lock(mutex_);
        flushing_ = true;
        ++generation_;
        clearLocked();
        available_.notify_all();
    }

    void resumeAfterCommand() {
        std::lock_guard<std::mutex> lock(mutex_);
        flushing_ = false;
        videoEos_ = false;
        available_.notify_all();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        clearLocked();
        available_.notify_all();
    }

    uint64_t generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }
    int width() const { std::lock_guard<std::mutex> lock(mutex_); return width_; }
    int height() const { std::lock_guard<std::mutex> lock(mutex_); return height_; }
    bool empty() const { std::lock_guard<std::mutex> lock(mutex_); return frames_.empty(); }
    bool videoEos() const { std::lock_guard<std::mutex> lock(mutex_); return videoEos_; }
    odyssey::MvcPipeline::TimingDiagnostics timingDiagnostics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {segmentStart_, runReferenceStart_, segmentCount_, runCount_};
    }
    std::string error() const { std::lock_guard<std::mutex> lock(mutex_); return error_; }
    HRESULT callbackFailure(const char* message, HRESULT result) {
        std::lock_guard<std::mutex> lock(mutex_);
        return failLocked(message, result);
    }

private:
    static bool validFormat(const VideoFormat& format) {
        // Blu-ray 3D AVC/MVC delivers one HD view per IMediaSample3D buffer.
        // A wider decoded buffer is an already-packed 2D/SBS stream.
        return format.subtype == MEDIASUBTYPE_NV12 && format.activeWidth > 0 &&
               format.activeWidth <= 1920 &&
               format.activeHeight > 0 && (format.activeWidth % 2) == 0 &&
               (format.activeHeight % 2) == 0 && format.stride >= format.activeWidth &&
               format.storageHeight >= format.activeHeight;
    }
    static bool buffersFit(long size, const VideoFormat& format) {
        const int64_t required = static_cast<int64_t>(format.stride) *
                                 format.storageHeight * 3 / 2;
        return size >= 0 && required <= size;
    }
    static void copyPair(const BYTE* left, const BYTE* right,
                         const VideoFormat& format, AVFrame* frame) {
        for (long y = 0; y < format.activeHeight; ++y) {
            BYTE* destination = frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0];
            const size_t offset = static_cast<size_t>(y) * format.stride;
            std::memcpy(destination, left + offset, static_cast<size_t>(format.activeWidth));
            std::memcpy(destination + format.activeWidth, right + offset,
                        static_cast<size_t>(format.activeWidth));
        }
        const size_t sourceUv = static_cast<size_t>(format.stride) * format.storageHeight;
        for (long y = 0; y < format.activeHeight / 2; ++y) {
            BYTE* destination = frame->data[1] + static_cast<ptrdiff_t>(y) * frame->linesize[1];
            const size_t offset = sourceUv + static_cast<size_t>(y) * format.stride;
            std::memcpy(destination, left + offset, static_cast<size_t>(format.activeWidth));
            std::memcpy(destination + format.activeWidth, right + offset,
                        static_cast<size_t>(format.activeWidth));
        }
    }
    HRESULT fail(const char* message) {
        std::lock_guard<std::mutex> lock(mutex_);
        return failLocked(message, E_FAIL);
    }
    HRESULT failLocked(const char* message, HRESULT result) {
        try {
            error_ = message;
        } catch (...) {
            error_.clear();
        }
        return result;
    }
    void clearLocked() {
        for (AVFrame* frame : frames_) av_frame_free(&frame);
        frames_.clear();
    }

    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<AVFrame*> frames_;
    const std::size_t capacity_;
    VideoFormat format_{};
    REFERENCE_TIME segmentStart_{0};
    REFERENCE_TIME runReferenceStart_{0};
    uint64_t generation_{0};
    uint64_t decodedFrames_{0};
    uint64_t segmentCount_{0};
    uint64_t runCount_{0};
    int width_{0};
    int height_{0};
    bool flushing_{false};
    bool stopping_{false};
    bool videoEos_{false};
    bool haveRunReference_{false};
    std::string error_;
};

class SubtitleState {
public:
    explicit SubtitleState(FrameState* frames) : frames_(frames) {}

    HRESULT setFormat(const AM_MEDIA_TYPE& type) {
        if (type.majortype != kMediaTypeSubtitle ||
            type.subtype != kMediaSubtypeHdmvSub ||
            type.formattype != kFormatSubtitleInfo || !type.pbFormat ||
            type.cbFormat < sizeof(SubtitleInfoFormat)) {
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        const auto* info = reinterpret_cast<const SubtitleInfoFormat*>(type.pbFormat);
        if (info->offset < sizeof(SubtitleInfoFormat) || info->offset > type.cbFormat) {
            return VFW_E_TYPE_NOT_ACCEPTED;
        }

        AVCodecParameters* parameters = avcodec_parameters_alloc();
        if (!parameters) return E_OUTOFMEMORY;
        struct ParametersGuard {
            AVCodecParameters* value;
            ~ParametersGuard() { avcodec_parameters_free(&value); }
        } parametersGuard{parameters};
        parameters->codec_type = AVMEDIA_TYPE_SUBTITLE;
        parameters->codec_id = AV_CODEC_ID_HDMV_PGS_SUBTITLE;
        parameters->width = frames_->width() > 0 ? frames_->width() / 2 : 1920;
        parameters->height = frames_->height() > 0 ? frames_->height() : 1080;
        std::string decoderError;
        std::lock_guard<std::mutex> lock(mutex_);
        const bool opened = decoder_.open(parameters, 1, static_cast<int>(kTicksPerSecond),
                                          parameters->width, parameters->height,
                                          decoderError);
        if (!opened) {
            error_ = std::move(decoderError);
            decoderReady_ = false;
            accepting_ = false;
            return S_OK;
        }
        decoderReady_ = true;
        error_.clear();
        return S_OK;
    }

    HRESULT receive(IMediaSample* sample) {
        if (!sample) return E_POINTER;
        BYTE* bytes = nullptr;
        REFERENCE_TIME sampleStart = 0;
        REFERENCE_TIME sampleStop = 0;
        if (FAILED(sample->GetPointer(&bytes)) || !bytes ||
            FAILED(sample->GetTime(&sampleStart, &sampleStop)) ||
            sample->GetActualDataLength() < 0) {
            setCaptionError("PGS sample is malformed");
            return S_OK;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || flushing_ || !decoderReady_) return S_OK;
        int64_t timestamp = 0;
        if (!odyssey::mvcSubtitleTimestamp100ns(
                segmentStart_, sampleStart, segmentRate_, timestamp)) {
            setCaptionErrorLocked("PGS sample timestamp is invalid");
            return S_OK;
        }
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            setCaptionErrorLocked("PGS packet allocation failed");
            return S_OK;
        }
        struct PacketGuard {
            AVPacket** packet;
            ~PacketGuard() { av_packet_free(packet); }
        } guard{&packet};
        const int length = sample->GetActualDataLength();
        if (av_new_packet(packet, length) < 0) {
            setCaptionErrorLocked("PGS packet buffer allocation failed");
            return S_OK;
        }
        if (length > 0) std::memcpy(packet->data, bytes, static_cast<std::size_t>(length));
        packet->pts = timestamp;
        packet->dts = timestamp;
        if (sampleStop > sampleStart) packet->duration = sampleStop - sampleStart;

        odyssey::SubtitleDecodeOutput output;
        std::string decoderError;
        if (!decoder_.decode(packet, output, decoderError)) {
            setCaptionErrorLocked(std::move(decoderError));
            return S_OK;
        }
        if (output.clearAtNanoseconds) {
            for (odyssey::SubtitleCue& cue : cues_) {
                if (!cue.endNanoseconds &&
                    cue.startNanoseconds <= *output.clearAtNanoseconds) {
                    cue.endNanoseconds = *output.clearAtNanoseconds;
                }
            }
        }
        const int64_t safeExpiration = saturatingAddNanoseconds(
            mediaNanoseconds_, -kMaximumSubtitleOffsetNs);
        cues_.erase(std::remove_if(cues_.begin(), cues_.end(),
                     [safeExpiration](const odyssey::SubtitleCue& cue) {
                         return cue.endNanoseconds &&
                                *cue.endNanoseconds <= safeExpiration;
                     }), cues_.end());

        std::size_t totalBytes = 0;
        bool overflow = output.cues.size() > kMaximumSubtitleCues - cues_.size();
        for (const odyssey::SubtitleCue& cue : cues_) {
            const std::size_t bytesInCue = subtitleCueBytes(cue);
            if (bytesInCue > kMaximumSubtitleBytes - totalBytes) {
                overflow = true;
                break;
            }
            totalBytes += bytesInCue;
        }
        for (const odyssey::SubtitleCue& cue : output.cues) {
            const std::size_t bytesInCue = subtitleCueBytes(cue);
            if (bytesInCue > kMaximumSubtitleBytes - totalBytes) {
                overflow = true;
                break;
            }
            totalBytes += bytesInCue;
        }
        if (overflow) {
            setCaptionErrorLocked("PGS cue buffer capacity exceeded");
            return S_OK;
        }
        cues_.insert(cues_.end(),
                     std::make_move_iterator(output.cues.begin()),
                     std::make_move_iterator(output.cues.end()));
        return S_OK;
    }

    void beginFlush() {
        std::lock_guard<std::mutex> lock(mutex_);
        flushing_ = true;
        decoder_.flush();
        cues_.clear();
    }
    void endFlush() {
        std::lock_guard<std::mutex> lock(mutex_);
        flushing_ = false;
    }
    HRESULT newSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) {
        if (start < 0 || stop < start || rate != 1.0) {
            setCaptionError("PGS segment timing is unsupported");
            return E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        segmentStart_ = start;
        segmentRate_ = rate;
        decoder_.flush();
        cues_.clear();
        return S_OK;
    }
    void prepareEmbedded(long streamIndex) {
        std::lock_guard<std::mutex> lock(mutex_);
        selectedStream_ = streamIndex;
        externalSelected_ = false;
        accepting_ = false;
        cues_.clear();
        externalCues_.clear();
        error_.clear();
    }
    void activateEmbedded() {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = decoderReady_;
        if (!decoderReady_ && error_.empty()) error_ = "PGS decoder is not connected";
    }
    void selectionFailed(std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        setCaptionErrorLocked(std::move(message));
    }
    void disable() {
        std::lock_guard<std::mutex> lock(mutex_);
        selectedStream_ = -1;
        externalSelected_ = false;
        accepting_ = false;
        cues_.clear();
        externalCues_.clear();
        error_.clear();
    }
    bool loadExternal(const std::wstring& path, std::string* error) {
        std::vector<odyssey::SubtitleCue> parsed;
        std::string parseError;
        if (!odyssey::SubtitleDecoder::parseSrtFile(path, parsed, parseError)) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = parseError;
            if (error) *error = std::move(parseError);
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        selectedStream_ = -1;
        externalSelected_ = true;
        accepting_ = false;
        cues_.clear();
        externalCues_ = std::move(parsed);
        error_.clear();
        if (error) error->clear();
        return true;
    }
    bool setOffset(double seconds) {
        if (!std::isfinite(seconds) ||
            std::abs(seconds) > static_cast<double>(kMaximumSubtitleOffsetNs) / 1'000'000'000.0) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        offsetNanoseconds_ = static_cast<int64_t>(seconds * 1'000'000'000.0);
        return true;
    }
    void updateMediaTime(int64_t nanoseconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        mediaNanoseconds_ = nanoseconds;
    }
    std::vector<odyssey::SubtitleCue> activeCues(int64_t mediaNanoseconds) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (externalSelected_) {
            return odyssey::SubtitleDecoder::activeCues(
                externalCues_, mediaNanoseconds, offsetNanoseconds_);
        }
        if (selectedStream_ < 0) return {};
        return odyssey::SubtitleDecoder::activeCues(
            cues_, mediaNanoseconds, offsetNanoseconds_);
    }
    long selectedStream() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return selectedStream_;
    }
    bool externalSelected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return externalSelected_;
    }
    std::string error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }
    void callbackFailure(std::string message) noexcept {
        try {
            setCaptionError(std::move(message));
        } catch (...) {
            // Caption diagnostics must not turn an allocator/decoder exception into A/V failure.
        }
    }

private:
    void setCaptionError(std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        setCaptionErrorLocked(std::move(message));
    }
    void setCaptionErrorLocked(std::string message) {
        error_ = std::move(message);
        selectedStream_ = -1;
        accepting_ = false;
        cues_.clear();
    }

    FrameState* frames_;
    mutable std::mutex mutex_;
    odyssey::SubtitleDecoder decoder_;
    std::vector<odyssey::SubtitleCue> cues_;
    std::vector<odyssey::SubtitleCue> externalCues_;
    REFERENCE_TIME segmentStart_{0};
    double segmentRate_{1.0};
    int64_t mediaNanoseconds_{0};
    int64_t offsetNanoseconds_{0};
    long selectedStream_{-1};
    bool externalSelected_{false};
    bool decoderReady_{false};
    bool accepting_{false};
    bool flushing_{false};
    std::string error_;
};


class FrameSinkFilter;

class PinEnumerator final : public IEnumPins {
public:
    explicit PinEnumerator(IPin* pin) : pin_(pin) { pin_->AddRef(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IEnumPins)) {
            *object = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override {
        if (!pins || (count != 1 && !fetched)) return E_POINTER;
        ULONG actual = 0;
        if (!returned_ && count > 0) {
            pin_->AddRef();
            pins[0] = pin_;
            returned_ = true;
            actual = 1;
        }
        if (fetched) *fetched = actual;
        return actual == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        if (count == 0) return S_OK;
        if (!returned_) returned_ = true;
        return count == 1 ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        returned_ = false;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** clone) override {
        if (!clone) return E_POINTER;
        auto* copy = new (std::nothrow) PinEnumerator(pin_);
        if (!copy) return E_OUTOFMEMORY;
        copy->returned_ = returned_;
        *clone = copy;
        return S_OK;
    }

private:
    ~PinEnumerator() { pin_->Release(); }
    std::atomic<ULONG> references_{1};
    IPin* pin_;
    bool returned_{false};
};

class FrameSinkPin final : public IPin, public IMemInputPin {
public:
    FrameSinkPin(FrameSinkFilter* owner, FrameState* state)
        : owner_(owner), state_(state) {
        allocator_.Attach(new StereoAllocator());
    }
    ~FrameSinkPin() {
        Disconnect();
        freeMediaType(mediaType_);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }

    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* type) override {
        if (!connector || !type) return E_POINTER;
        if (QueryAccept(type) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;
        if (connected_ && connected_ != connector) return VFW_E_ALREADY_CONNECTED;
        if (!connected_) {
            connector->AddRef();
            connected_ = connector;
        }
        freeMediaType(mediaType_);
        std::memset(&mediaType_, 0, sizeof(mediaType_));
        HRESULT hr = copyMediaType(mediaType_, *type);
        if (SUCCEEDED(hr)) hr = state_->setFormat(*type);
        if (FAILED(hr)) {
            freeMediaType(mediaType_);
            std::memset(&mediaType_, 0, sizeof(mediaType_));
            connected_->Release();
            connected_ = nullptr;
        }
        return hr;
    }
    STDMETHODIMP Disconnect() override {
        if (!connected_) return S_FALSE;
        connected_->Release();
        connected_ = nullptr;
        freeMediaType(mediaType_);
        std::memset(&mediaType_, 0, sizeof(mediaType_));
        return S_OK;
    }
    STDMETHODIMP ConnectedTo(IPin** pin) override {
        if (!pin) return E_POINTER;
        *pin = nullptr;
        if (!connected_) return VFW_E_NOT_CONNECTED;
        connected_->AddRef();
        *pin = connected_;
        return S_OK;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* type) override {
        if (!type) return E_POINTER;
        if (!connected_) return VFW_E_NOT_CONNECTED;
        std::memset(type, 0, sizeof(*type));
        return copyMediaType(*type, mediaType_);
    }
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* direction) override {
        if (!direction) return E_POINTER;
        *direction = PINDIR_INPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR* id) override {
        if (!id) return E_POINTER;
        constexpr wchar_t name[] = L"Input";
        *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(name)));
        if (!*id) return E_OUTOFMEMORY;
        std::memcpy(*id, name, sizeof(name));
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* type) override {
        if (!type) return E_POINTER;
        const bool formatOk = type->formattype == FORMAT_VideoInfo || type->formattype == FORMAT_VideoInfo2;
        return type->majortype == MEDIATYPE_Video && type->subtype == MEDIASUBTYPE_NV12 &&
                       formatOk && sourceRectStartsAtOrigin(*type)
                   ? S_OK
                   : S_FALSE;
    }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override {
        state_->endOfStream();
        return S_OK;
    }
    STDMETHODIMP BeginFlush() override {
        flushing_ = true;
        state_->beginFlush();
        return S_OK;
    }
    STDMETHODIMP EndFlush() override {
        state_->endFlush();
        flushing_ = false;
        return S_OK;
    }
    STDMETHODIMP NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override {
        return state_->newSegment(start, stop, rate);
    }

    STDMETHODIMP GetAllocator(IMemAllocator** allocator) override {
        if (!allocator) return E_POINTER;
        allocator_->AddRef();
        *allocator = allocator_.Get();
        return S_OK;
    }
    STDMETHODIMP NotifyAllocator(IMemAllocator* allocator, BOOL) override {
        return allocator == allocator_.Get() ? S_OK : VFW_E_NO_ALLOCATOR;
    }
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES* properties) override {
        if (!properties) return E_POINTER;
        properties->cBuffers = 8;
        properties->cbBuffer = 0;
        properties->cbAlign = 16;
        properties->cbPrefix = 0;
        return S_OK;
    }
    STDMETHODIMP Receive(IMediaSample* sample) override {
        if (flushing_) return S_FALSE;
        try {
            return state_->receive(sample);
        } catch (const std::bad_alloc&) {
            return state_->callbackFailure("MVC receive allocation failed", E_OUTOFMEMORY);
        } catch (...) {
            return state_->callbackFailure("MVC receive failed unexpectedly", E_FAIL);
        }
    }
    STDMETHODIMP ReceiveMultiple(IMediaSample** samples, long count, long* processed) override {
        if (!samples || !processed) return E_POINTER;
        *processed = 0;
        for (long i = 0; i < count; ++i) {
            const HRESULT hr = Receive(samples[i]);
            if (FAILED(hr)) return hr;
            ++*processed;
        }
        return S_OK;
    }
    STDMETHODIMP ReceiveCanBlock() override { return S_OK; }

private:
    std::atomic<ULONG> references_{1};
    FrameSinkFilter* owner_;
    FrameState* state_;
    ComPtr<StereoAllocator> allocator_;
    IPin* connected_{nullptr};
    AM_MEDIA_TYPE mediaType_{};
    std::atomic<bool> flushing_{false};
};

class FrameSinkFilter final : public IBaseFilter {
public:
    explicit FrameSinkFilter(FrameState* state)
        : pin_(new FrameSinkPin(this, state)), frameState_(state) {}
    ~FrameSinkFilter() {
        if (clock_) clock_->Release();
        pin_->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IPersist) ||
            riid == __uuidof(IMediaFilter) || riid == __uuidof(IBaseFilter)) {
            *object = static_cast<IBaseFilter*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    STDMETHODIMP GetClassID(CLSID* clsid) override {
        if (!clsid) return E_POINTER;
        *clsid = kClsidFrameSink;
        return S_OK;
    }
    STDMETHODIMP Stop() override {
        state_ = State_Stopped;
        return S_OK;
    }
    STDMETHODIMP Pause() override {
        state_ = State_Paused;
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME referenceStart) override {
        frameState_->run(referenceStart);
        state_ = State_Running;
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* state) override {
        if (!state) return E_POINTER;
        *state = state_;
        return S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* clock) override {
        if (clock) clock->AddRef();
        if (clock_) clock_->Release();
        clock_ = clock;
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** clock) override {
        if (!clock) return E_POINTER;
        *clock = clock_;
        if (clock_) clock_->AddRef();
        return S_OK;
    }
    STDMETHODIMP EnumPins(IEnumPins** pins) override {
        if (!pins) return E_POINTER;
        *pins = new (std::nothrow) PinEnumerator(pin_);
        return *pins ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override {
        if (!id || !pin) return E_POINTER;
        *pin = nullptr;
        if (std::wcscmp(id, L"Input") != 0) return VFW_E_NOT_FOUND;
        pin_->AddRef();
        *pin = pin_;
        return S_OK;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override {
        if (!info) return E_POINTER;
        wcscpy_s(info->achName, L"Odyssey MVC Frame Sink");
        info->pGraph = graph_;
        if (graph_) graph_->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override {
        graph_ = graph;
        try {
            name_ = name ? name : L"";
            return S_OK;
        } catch (const std::bad_alloc&) {
            name_.clear();
            return E_OUTOFMEMORY;
        } catch (...) {
            name_.clear();
            return E_FAIL;
        }
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR* vendor) override {
        if (!vendor) return E_POINTER;
        constexpr wchar_t name[] = L"Odyssey";
        *vendor = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(name)));
        if (!*vendor) return E_OUTOFMEMORY;
        std::memcpy(*vendor, name, sizeof(name));
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    FrameSinkPin* pin_;
    FrameState* frameState_;
    FILTER_STATE state_{State_Stopped};
    IReferenceClock* clock_{nullptr};
    IFilterGraph* graph_{nullptr};
    std::wstring name_;
};

STDMETHODIMP FrameSinkPin::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IPin)) *object = static_cast<IPin*>(this);
    else if (riid == __uuidof(IMemInputPin)) *object = static_cast<IMemInputPin*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP FrameSinkPin::QueryPinInfo(PIN_INFO* info) {
    if (!info) return E_POINTER;
    info->pFilter = owner_;
    owner_->AddRef();
    info->dir = PINDIR_INPUT;
    wcscpy_s(info->achName, L"Input");
    return S_OK;
}

class SubtitleSinkFilter;

class SubtitleSinkPin final : public IPin, public IMemInputPin {
public:
    SubtitleSinkPin(SubtitleSinkFilter* owner, SubtitleState* state)
        : owner_(owner), state_(state) {}
    ~SubtitleSinkPin() {
        Disconnect();
        freeMediaType(mediaType_);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* type) override {
        if (!connector || !type) return E_POINTER;
        if (QueryAccept(type) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;
        if (connected_ && connected_ != connector) return VFW_E_ALREADY_CONNECTED;
        if (!connected_) {
            connector->AddRef();
            connected_ = connector;
        }
        freeMediaType(mediaType_);
        std::memset(&mediaType_, 0, sizeof(mediaType_));
        HRESULT hr = S_OK;
        try {
            hr = copyMediaType(mediaType_, *type);
            if (SUCCEEDED(hr)) hr = state_->setFormat(*type);
        } catch (const std::bad_alloc&) {
            state_->callbackFailure("PGS format allocation failed");
            hr = E_OUTOFMEMORY;
        } catch (...) {
            state_->callbackFailure("PGS format setup failed unexpectedly");
            hr = E_FAIL;
        }
        if (FAILED(hr)) Disconnect();
        return hr;
    }
    STDMETHODIMP Disconnect() override {
        if (!connected_) return S_FALSE;
        connected_->Release();
        connected_ = nullptr;
        freeMediaType(mediaType_);
        std::memset(&mediaType_, 0, sizeof(mediaType_));
        return S_OK;
    }
    STDMETHODIMP ConnectedTo(IPin** pin) override {
        if (!pin) return E_POINTER;
        *pin = nullptr;
        if (!connected_) return VFW_E_NOT_CONNECTED;
        connected_->AddRef();
        *pin = connected_;
        return S_OK;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* type) override {
        if (!type) return E_POINTER;
        if (!connected_) return VFW_E_NOT_CONNECTED;
        std::memset(type, 0, sizeof(*type));
        return copyMediaType(*type, mediaType_);
    }
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* direction) override {
        if (!direction) return E_POINTER;
        *direction = PINDIR_INPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR* id) override {
        if (!id) return E_POINTER;
        constexpr wchar_t name[] = L"Input";
        *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(name)));
        if (!*id) return E_OUTOFMEMORY;
        std::memcpy(*id, name, sizeof(name));
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* type) override {
        if (!type) return E_POINTER;
        return type->majortype == kMediaTypeSubtitle &&
                       type->subtype == kMediaSubtypeHdmvSub &&
                       type->formattype == kFormatSubtitleInfo &&
                       type->pbFormat && type->cbFormat >= sizeof(SubtitleInfoFormat)
                   ? S_OK : S_FALSE;
    }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override {
        flushing_ = true;
        state_->beginFlush();
        return S_OK;
    }
    STDMETHODIMP EndFlush() override {
        state_->endFlush();
        flushing_ = false;
        return S_OK;
    }
    STDMETHODIMP NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override {
        return state_->newSegment(start, stop, rate);
    }
    STDMETHODIMP GetAllocator(IMemAllocator** allocator) override {
        if (!allocator) return E_POINTER;
        if (!allocator_) {
            const HRESULT hr = CoCreateInstance(
                CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&allocator_));
            if (FAILED(hr)) return hr;
        }
        allocator_->AddRef();
        *allocator = allocator_.Get();
        return S_OK;
    }
    STDMETHODIMP NotifyAllocator(IMemAllocator* allocator, BOOL) override {
        if (!allocator) return E_POINTER;
        allocator_ = allocator;
        return S_OK;
    }
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES* properties) override {
        if (!properties) return E_POINTER;
        properties->cBuffers = 4;
        properties->cbBuffer = 0;
        properties->cbAlign = 1;
        properties->cbPrefix = 0;
        return S_OK;
    }
    STDMETHODIMP Receive(IMediaSample* sample) override {
        if (flushing_) return S_FALSE;
        try {
            return state_->receive(sample);
        } catch (const std::bad_alloc&) {
            state_->callbackFailure("PGS callback allocation failed");
            return S_OK;
        } catch (...) {
            state_->callbackFailure("PGS callback failed unexpectedly");
            return S_OK;
        }
    }
    STDMETHODIMP ReceiveMultiple(IMediaSample** samples, long count, long* processed) override {
        if (!samples || !processed) return E_POINTER;
        *processed = 0;
        for (long i = 0; i < count; ++i) {
            const HRESULT hr = Receive(samples[i]);
            if (FAILED(hr)) return hr;
            ++*processed;
        }
        return S_OK;
    }
    STDMETHODIMP ReceiveCanBlock() override { return S_OK; }

private:
    std::atomic<ULONG> references_{1};
    SubtitleSinkFilter* owner_;
    SubtitleState* state_;
    ComPtr<IMemAllocator> allocator_;
    IPin* connected_{nullptr};
    AM_MEDIA_TYPE mediaType_{};
    std::atomic<bool> flushing_{false};
};

class SubtitleSinkFilter final : public IBaseFilter {
public:
    explicit SubtitleSinkFilter(SubtitleState* state)
        : pin_(new SubtitleSinkPin(this, state)) {}
    ~SubtitleSinkFilter() {
        if (clock_) clock_->Release();
        pin_->Release();
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IPersist) ||
            riid == __uuidof(IMediaFilter) || riid == __uuidof(IBaseFilter)) {
            *object = static_cast<IBaseFilter*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    STDMETHODIMP GetClassID(CLSID* clsid) override {
        if (!clsid) return E_POINTER;
        *clsid = kClsidSubtitleSink;
        return S_OK;
    }
    STDMETHODIMP Stop() override { state_ = State_Stopped; return S_OK; }
    STDMETHODIMP Pause() override { state_ = State_Paused; return S_OK; }
    STDMETHODIMP Run(REFERENCE_TIME) override { state_ = State_Running; return S_OK; }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* state) override {
        if (!state) return E_POINTER;
        *state = state_;
        return S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* clock) override {
        if (clock) clock->AddRef();
        if (clock_) clock_->Release();
        clock_ = clock;
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** clock) override {
        if (!clock) return E_POINTER;
        *clock = clock_;
        if (clock_) clock_->AddRef();
        return S_OK;
    }
    STDMETHODIMP EnumPins(IEnumPins** pins) override {
        if (!pins) return E_POINTER;
        *pins = new (std::nothrow) PinEnumerator(pin_);
        return *pins ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override {
        if (!id || !pin) return E_POINTER;
        *pin = nullptr;
        if (std::wcscmp(id, L"Input") != 0) return VFW_E_NOT_FOUND;
        pin_->AddRef();
        *pin = pin_;
        return S_OK;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override {
        if (!info) return E_POINTER;
        wcscpy_s(info->achName, L"Odyssey MVC Subtitle Sink");
        info->pGraph = graph_;
        if (graph_) graph_->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override {
        graph_ = graph;
        try {
            name_ = name ? name : L"";
            return S_OK;
        } catch (...) {
            name_.clear();
            return E_OUTOFMEMORY;
        }
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR* vendor) override {
        if (!vendor) return E_POINTER;
        constexpr wchar_t name[] = L"Odyssey";
        *vendor = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(name)));
        if (!*vendor) return E_OUTOFMEMORY;
        std::memcpy(*vendor, name, sizeof(name));
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    SubtitleSinkPin* pin_;
    FILTER_STATE state_{State_Stopped};
    IReferenceClock* clock_{nullptr};
    IFilterGraph* graph_{nullptr};
    std::wstring name_;
};

STDMETHODIMP SubtitleSinkPin::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IPin)) *object = static_cast<IPin*>(this);
    else if (riid == __uuidof(IMemInputPin)) *object = static_cast<IMemInputPin*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP SubtitleSinkPin::QueryPinInfo(PIN_INFO* info) {
    if (!info) return E_POINTER;
    info->pFilter = owner_;
    owner_->AddRef();
    info->dir = PINDIR_INPUT;
    wcscpy_s(info->achName, L"Input");
    return S_OK;
}

class Module {
public:
    explicit Module(const std::wstring& path)
        : handle_(LoadLibraryExW(path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {}
    ~Module() { if (handle_) FreeLibrary(handle_); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    explicit operator bool() const { return handle_ != nullptr; }
    HMODULE get() const { return handle_; }
private:
    HMODULE handle_{nullptr};
};

HRESULT createPrivateFilter(HMODULE module, REFCLSID clsid, IBaseFilter** filter) {
    if (!filter) return E_POINTER;
    *filter = nullptr;
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
    if (!getClassObject) return HRESULT_FROM_WIN32(GetLastError());
    ComPtr<IClassFactory> factory;
    HRESULT hr = getClassObject(clsid, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    return factory->CreateInstance(nullptr, IID_PPV_ARGS(filter));
}

std::vector<ComPtr<IPin>> pins(IBaseFilter* filter, PIN_DIRECTION direction) {
    std::vector<ComPtr<IPin>> result;
    ComPtr<IEnumPins> enumerator;
    if (FAILED(filter->EnumPins(&enumerator))) return result;
    while (true) {
        ComPtr<IPin> pin;
        ULONG fetched = 0;
        if (enumerator->Next(1, &pin, &fetched) != S_OK) break;
        PIN_DIRECTION actual{};
        if (SUCCEEDED(pin->QueryDirection(&actual)) && actual == direction) result.push_back(pin);
    }
    return result;
}

HRESULT connectSourceVideo(IGraphBuilder* graph, IBaseFilter* source, IBaseFilter* decoder,
                           ComPtr<IPin>& connectedSource, AM_MEDIA_TYPE& inputType) {
    auto decoderInputs = pins(decoder, PINDIR_INPUT);
    if (decoderInputs.empty()) return VFW_E_NOT_FOUND;
    for (const auto& sourcePin : pins(source, PINDIR_OUTPUT)) {
        const HRESULT hr = graph->ConnectDirect(sourcePin.Get(), decoderInputs[0].Get(), nullptr);
        if (SUCCEEDED(hr)) {
            connectedSource = sourcePin;
            std::memset(&inputType, 0, sizeof(inputType));
            decoderInputs[0]->ConnectionMediaType(&inputType);
            return S_OK;
        }
    }
    return VFW_E_CANNOT_CONNECT;
}

HRESULT connectSourceToDecoder(IGraphBuilder* graph, IBaseFilter* source,
                               IBaseFilter* decoder, ComPtr<IPin>* connectedSource) {
    auto decoderInputs = pins(decoder, PINDIR_INPUT);
    if (decoderInputs.empty()) return VFW_E_NOT_FOUND;
    for (const auto& sourcePin : pins(source, PINDIR_OUTPUT)) {
        const HRESULT hr = graph->ConnectDirect(sourcePin.Get(), decoderInputs[0].Get(), nullptr);
        if (SUCCEEDED(hr)) {
            if (connectedSource) *connectedSource = sourcePin;
            return S_OK;
        }
    }
    return VFW_E_CANNOT_CONNECT;
}

HRESULT connectFirstOutput(IGraphBuilder* graph, IBaseFilter* source, IBaseFilter* sink) {
    const auto outputs = pins(source, PINDIR_OUTPUT);
    const auto inputs = pins(sink, PINDIR_INPUT);
    if (outputs.empty() || inputs.empty()) return VFW_E_NOT_FOUND;
    for (const auto& output : outputs) {
        const HRESULT hr = graph->ConnectDirect(output.Get(), inputs[0].Get(), nullptr);
        if (SUCCEEDED(hr)) return S_OK;
    }
    return VFW_E_CANNOT_CONNECT;
}

std::string mediaTypeSummary(const AM_MEDIA_TYPE& type) {
    std::ostringstream stream;
    stream << "major=0x" << std::hex << type.majortype.Data1
           << " subtype=0x" << type.subtype.Data1
           << " format=0x" << type.formattype.Data1 << std::dec
           << " cbFormat=" << type.cbFormat;
    if (type.formattype == kFormatSubtitleInfo && type.pbFormat &&
        type.cbFormat >= sizeof(SubtitleInfoFormat)) {
        stream << " offset=" << reinterpret_cast<const SubtitleInfoFormat*>(type.pbFormat)->offset;
    }
    return stream.str();
}

HRESULT ensureSubtitleConnection(IGraphBuilder* graph, IBaseFilter* source,
                                 IBaseFilter* sink, std::string* diagnostic) {
    const auto inputs = pins(sink, PINDIR_INPUT);
    if (inputs.empty()) return VFW_E_NOT_FOUND;
    ComPtr<IPin> connected;
    if (inputs[0]->ConnectedTo(&connected) == S_OK) return S_OK;
    HRESULT lastError = VFW_E_CANNOT_CONNECT;
    std::ostringstream details;
    std::vector<ComPtr<IPin>> untypedOutputs;
    for (const auto& output : pins(source, PINDIR_OUTPUT)) {
        bool attempted = false;
        bool typeKnown = false;
        AM_MEDIA_TYPE type{};
        HRESULT typeHr = output->ConnectionMediaType(&type);
        if (SUCCEEDED(typeHr)) {
            typeKnown = true;
            const bool subtitle = type.majortype == kMediaTypeSubtitle &&
                                  type.subtype == kMediaSubtypeHdmvSub;
            if (subtitle) {
                attempted = true;
                const HRESULT hr = graph->ConnectDirect(output.Get(), inputs[0].Get(), nullptr);
                if (SUCCEEDED(hr)) return S_OK;
                lastError = hr;
                details << " connected(" << mediaTypeSummary(type) << ") hr=0x"
                        << std::hex << static_cast<unsigned long>(hr) << std::dec;
            }
            freeMediaType(type);
        } else {
            ComPtr<IEnumMediaTypes> mediaTypes;
            if (SUCCEEDED(output->EnumMediaTypes(&mediaTypes))) {
                while (true) {
                    AM_MEDIA_TYPE* candidate = nullptr;
                    ULONG fetched = 0;
                    const HRESULT nextHr = mediaTypes->Next(1, &candidate, &fetched);
                    if (nextHr != S_OK || fetched != 1 || !candidate) break;
                    typeKnown = true;
                    const bool subtitle = candidate->majortype == kMediaTypeSubtitle &&
                                          candidate->subtype == kMediaSubtypeHdmvSub;
                    if (subtitle) {
                        attempted = true;
                        const HRESULT hr = graph->ConnectDirect(output.Get(), inputs[0].Get(), nullptr);
                        if (SUCCEEDED(hr)) {
                            freeMediaType(*candidate);
                            CoTaskMemFree(candidate);
                            return S_OK;
                        }
                        lastError = hr;
                        details << " advertised(" << mediaTypeSummary(*candidate) << ") hr=0x"
                                << std::hex << static_cast<unsigned long>(hr) << std::dec;
                    }
                    freeMediaType(*candidate);
                    CoTaskMemFree(candidate);
                }
            }
        }
        if (!attempted) {
            details << " untyped-output(type_hr=0x" << std::hex
                    << static_cast<unsigned long>(typeHr) << std::dec << ')';
        }
        if (!typeKnown) untypedOutputs.push_back(output);
    }
    for (const auto& output : untypedOutputs) {
        const HRESULT hr = graph->ConnectDirect(output.Get(), inputs[0].Get(), nullptr);
        if (SUCCEEDED(hr)) return S_OK;
        lastError = hr;
        details << " fallback hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec;
    }
    if (diagnostic) *diagnostic = details.str();
    return lastError;
}

bool sameComObject(IUnknown* left, IUnknown* right) {
    if (!left || !right) return false;
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity))) &&
           SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity))) &&
           leftIdentity.Get() == rightIdentity.Get();
}

std::string hrError(const char* operation, HRESULT hr) {
    std::ostringstream stream;
    stream << operation << " failed (0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(hr) << ')';
    return stream.str();
}

bool isIsoPath(const std::wstring& path) {
    std::filesystem::path extension(path);
    std::wstring value = extension.extension().wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value == L".iso";
}

bool safeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == L"..") return false;
    }
    return true;
}

bool pathWithinMountedRoot(const std::wstring& root, const std::filesystem::path& relative,
                           std::wstring* openedPath) {
    if (!openedPath || !safeRelativePath(relative)) return false;
    std::error_code error;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(std::filesystem::path(root), error);
    if (error) return false;
    const std::filesystem::path candidate =
        std::filesystem::weakly_canonical(canonicalRoot / relative, error);
    if (error) return false;
    const std::filesystem::path fromRoot =
        std::filesystem::relative(candidate, canonicalRoot, error);
    if (error || fromRoot.empty() || !safeRelativePath(fromRoot)) return false;
    *openedPath = candidate.wstring();
    return true;
}

bool sameRelativePath(const std::wstring& left, const std::filesystem::path& right) {
    return _wcsicmp(std::filesystem::path(left).lexically_normal().wstring().c_str(),
                    right.lexically_normal().wstring().c_str()) == 0;
}

long volumeToDirectShow(float scalar) {
    if (scalar <= 0.0f) return -10000;
    const double hundredthsDb = 2000.0 * std::log10(static_cast<double>(scalar));
    return static_cast<long>(std::clamp(hundredthsDb, -10000.0, 0.0));
}

class GraphLifetime {
public:
    GraphLifetime(IGraphBuilder* graph, FrameState* frames)
        : graph_(graph), frames_(frames) {}
    ~GraphLifetime() {
        frames_->cancelForCommand();
        ComPtr<IMediaControl> control;
        graph_->QueryInterface(IID_PPV_ARGS(&control));
        if (control) control->Stop();
        frames_->stop();
    }
    GraphLifetime(const GraphLifetime&) = delete;
    GraphLifetime& operator=(const GraphLifetime&) = delete;

private:
    IGraphBuilder* graph_;
    FrameState* frames_;
};

} // namespace

namespace odyssey {

struct MvcPipeline::Impl {
    enum class CommandType {
        Pause, Resume, Seek, SelectAudio, SelectSubtitle, DisableSubtitle, Volume, Mute
    };
    struct Command {
        CommandType type;
        double number{0.0};
        long index{-1};
        bool flag{false};
    };

    Impl(std::wstring sourcePath, Options pipelineOptions)
        : path(std::move(sourcePath)), options(std::move(pipelineOptions)),
          frames(options.frameCapacity), subtitles(&frames) {
        if (path.empty()) throw std::invalid_argument("MvcPipeline requires an input path");
        if (options.lavDirectory.empty() ||
            !std::filesystem::path(options.lavDirectory).is_absolute()) {
            throw std::invalid_argument("MvcPipeline requires an absolute private LAV directory");
        }
        if (options.frameCapacity == 0 || !std::isfinite(options.volume) ||
            options.volume < 0.0f || options.volume > 1.0f) {
            throw std::invalid_argument("MvcPipeline options are invalid");
        }
        volumeScalar = options.volume;
        mutedValue = options.muted;
        cancellationEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!cancellationEvent) throw std::runtime_error("CreateEventW failed");
        try {
            worker = std::thread([this] {
                try {
                    threadMain();
                } catch (const std::exception& exception) {
                    setFailure(exception.what());
                    frames.stop();
                } catch (...) {
                    setFailure("MVC graph thread failed unexpectedly");
                    frames.stop();
                }
            });
        } catch (...) {
            CloseHandle(cancellationEvent);
            cancellationEvent = nullptr;
            throw;
        }
    }

    ~Impl() {
        stopAndJoin();
        CloseHandle(cancellationEvent);
    }

    bool enqueue(Command command) {
        if (status() == Status::Stopped || status() == Status::Failed) return false;
        std::lock_guard<std::mutex> lock(commandMutex);
        if (stopping) return false;
        commands.push_back(command);
        commandAvailable.notify_one();
        return true;
    }

    void stopAndJoin() {
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            stopping = true;
        }
        SetEvent(cancellationEvent);
        frames.cancelForCommand();
        commandAvailable.notify_all();
        if (worker.joinable()) worker.join();
    }

    void setFailure(const std::string& message) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        errorValue = message;
        statusValue = Status::Failed;
        openingStage = OpeningStage::Failed;
    }

    void setStatus(Status value) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        statusValue = value;
        if (value == Status::Stopped) openingStage = OpeningStage::Stopped;
    }

    void setOpeningStage(OpeningStage value) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        openingStage = value;
    }

    void setIsoMountDiagnostics(bool mounted, bool owned) {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        isoMounted = mounted;
        isoOwned = owned;
    }

    bool shouldStop() const {
        std::lock_guard<std::mutex> lock(commandMutex);
        return stopping;
    }

    void updateClock(IReferenceClock* clock, bool running) {
        if (!running || !clock) return;
        REFERENCE_TIME now = 0;
        if (FAILED(clock->GetTime(&now))) return;
        REFERENCE_TIME position = 0;
        if (!frames.mediaPosition(now, &position)) return;
        std::lock_guard<std::mutex> lock(snapshotMutex);
        positionTicks = std::clamp(position, static_cast<REFERENCE_TIME>(0), durationTicks);
    }

    void updateAudioContent(ILAVAudioStatus* audioStatus) {
        if (!audioStatus) return;
        LPCSTR codec = nullptr;
        LPCSTR format = nullptr;
        int channels = 0;
        int sampleRate = 0;
        DWORD channelMask = 0;
        if (FAILED(audioStatus->GetDecodeDetails(&codec, &format, &channels,
                                                 &sampleRate, &channelMask)) ||
            channels <= 0 || sampleRate <= 0) {
            return;
        }
        for (int channel = 0; channel < channels; ++channel) {
            float decibels = -std::numeric_limits<float>::infinity();
            if (SUCCEEDED(audioStatus->GetChannelVolumeAverage(static_cast<WORD>(channel),
                                                               &decibels)) &&
                std::isfinite(decibels) && decibels > -100.0f) {
                std::lock_guard<std::mutex> lock(snapshotMutex);
                audioContent = true;
                return;
            }
        }
    }

    int64_t snapshotPositionNanoseconds() const {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        if (positionTicks > std::numeric_limits<int64_t>::max() / 100)
            return std::numeric_limits<int64_t>::max();
        return positionTicks * 100;
    }

    void enumerateAudioTracks(IAMStreamSelect* selector) {
        std::vector<AudioTrack> result;
        DWORD count = 0;
        if (!selector || FAILED(selector->Count(&count))) return;
        for (DWORD rawIndex = 0; rawIndex < count; ++rawIndex) {
            const long index = static_cast<long>(rawIndex);
            AM_MEDIA_TYPE* type = nullptr;
            DWORD flags = 0;
            LPWSTR name = nullptr;
            const HRESULT hr = selector->Info(index, &type, &flags, nullptr, nullptr,
                                              &name, nullptr, nullptr);
            if (SUCCEEDED(hr) && type && type->majortype == MEDIATYPE_Audio) {
                result.push_back({index, name ? name : L"Audio",
                                  (flags & (AMSTREAMSELECTINFO_ENABLED |
                                            AMSTREAMSELECTINFO_EXCLUSIVE)) != 0});
            }
            if (name) CoTaskMemFree(name);
            if (type) {
                freeMediaType(*type);
                CoTaskMemFree(type);
            }
        }
        std::lock_guard<std::mutex> lock(snapshotMutex);
        tracks = std::move(result);
        selectedTrack = -1;
        for (const AudioTrack& track : tracks) {
            if (track.selected) selectedTrack = track.streamIndex;
        }
    }

    void enumerateSubtitleTracks(IAMStreamSelect* selector) {
        std::vector<SubtitleTrack> result;
        DWORD count = 0;
        if (!selector || FAILED(selector->Count(&count))) return;
        for (DWORD rawIndex = 0; rawIndex < count; ++rawIndex) {
            const long index = static_cast<long>(rawIndex);
            AM_MEDIA_TYPE* type = nullptr;
            DWORD flags = 0;
            LPWSTR name = nullptr;
            const HRESULT hr = selector->Info(index, &type, &flags, nullptr, nullptr,
                                              &name, nullptr, nullptr);
            if (SUCCEEDED(hr) && type &&
                type->majortype == kMediaTypeSubtitle) {
                SubtitleTrack track;
                track.streamIndex = index;
                track.name = name ? name : L"Subtitle";
                track.codecName = type->subtype == kMediaSubtypeHdmvSub ? "hdmv_pgs_subtitle"
                                                                        : "unsupported";
                track.supported = type->subtype == kMediaSubtypeHdmvSub &&
                                  type->formattype == kFormatSubtitleInfo &&
                                  type->pbFormat &&
                                  type->cbFormat >= sizeof(SubtitleInfoFormat);
                track.selected = (flags & (AMSTREAMSELECTINFO_ENABLED |
                                            AMSTREAMSELECTINFO_EXCLUSIVE)) != 0;
                if (track.supported) {
                    const auto* info = reinterpret_cast<const SubtitleInfoFormat*>(type->pbFormat);
                    if (info->offset < sizeof(SubtitleInfoFormat) || info->offset > type->cbFormat) {
                        track.supported = false;
                    } else {
                        track.language.assign(info->isoLanguage,
                                              std::find(info->isoLanguage,
                                                        info->isoLanguage + 4, '\0'));
                    }
                }
                result.push_back(std::move(track));
            }
            if (name) CoTaskMemFree(name);
            if (type) {
                freeMediaType(*type);
                CoTaskMemFree(type);
            }
        }
        const long activeStream = subtitles.selectedStream();
        const bool externalSelected = subtitles.externalSelected();
        if (!externalSelected && activeStream >= 0) {
            for (SubtitleTrack& track : result) {
                track.selected = track.streamIndex == activeStream;
            }
        } else if (externalSelected || activeStream < 0) {
            for (SubtitleTrack& track : result) track.selected = false;
        }
        std::lock_guard<std::mutex> lock(snapshotMutex);
        subtitleTracksValue = std::move(result);
    }

    bool handleCommand(const Command& command, IMediaControl* control,
                       IMediaSeeking* seeking, IAMStreamSelect* selector,
                       IBasicAudio* audio, IReferenceClock* clock,
                       IGraphBuilder* graph, IBaseFilter* source,
                       IBaseFilter* subtitleSink, ILAVFSettings* splitterSettings,
                       bool& running) {
        HRESULT hr = S_OK;
        switch (command.type) {
        case CommandType::Pause:
            if (running) {
                updateClock(clock, true);
                hr = control->Pause();
                if (SUCCEEDED(hr)) {
                    running = false;
                    std::lock_guard<std::mutex> lock(snapshotMutex);
                    statusValue = Status::Paused;
                }
            }
            break;
        case CommandType::Resume:
            if (!running) {
                hr = control->Run();
                if (SUCCEEDED(hr)) {
                    running = true;
                    {
                        std::lock_guard<std::mutex> lock(snapshotMutex);
                        statusValue = Status::Running;
                    }
                }
            }
            break;
        case CommandType::Seek: {
            const long double scaled = static_cast<long double>(command.number) *
                                       static_cast<long double>(kTicksPerSecond);
            if (scaled < 0.0L || scaled >= 9223372036854775808.0L) {
                hr = E_INVALIDARG;
                break;
            }
            const auto ticks = static_cast<REFERENCE_TIME>(scaled);
            frames.cancelForCommand();
            REFERENCE_TIME requested = ticks;
            hr = seeking->SetPositions(&requested, AM_SEEKING_AbsolutePositioning |
                                                   AM_SEEKING_ReturnTime,
                                       nullptr, AM_SEEKING_NoPositioning);
            frames.resumeAfterCommand();
            if (SUCCEEDED(hr)) {
                std::lock_guard<std::mutex> lock(snapshotMutex);
                positionTicks = requested;
            }
            break;
        }
        case CommandType::SelectAudio:
            frames.cancelForCommand();
            hr = selector ? selector->Enable(command.index, AMSTREAMSELECTENABLE_ENABLE)
                          : E_NOINTERFACE;
            frames.resumeAfterCommand();
            if (SUCCEEDED(hr)) enumerateAudioTracks(selector);
            break;
        case CommandType::SelectSubtitle: {
            const bool wasRunning = running;
            if (wasRunning) updateClock(clock, true);
            REFERENCE_TIME savedPosition = 0;
            {
                std::lock_guard<std::mutex> lock(snapshotMutex);
                savedPosition = positionTicks;
            }
            frames.cancelForCommand();
            std::string subtitleLifecycleDiagnostic;
            auto stageFailure = [&subtitleLifecycleDiagnostic](const char* stage, HRESULT value) {
                if (!subtitleLifecycleDiagnostic.empty()) subtitleLifecycleDiagnostic.append(" ");
                subtitleLifecycleDiagnostic.append(stage);
                subtitleLifecycleDiagnostic.append(" hr=0x");
                std::ostringstream stream;
                stream << std::hex << std::uppercase << static_cast<unsigned long>(value);
                subtitleLifecycleDiagnostic.append(stream.str());
            };
            auto restoreGraph = [&]() {
                REFERENCE_TIME restorePosition = savedPosition;
                const HRESULT seekHr = seeking->SetPositions(
                    &restorePosition, AM_SEEKING_AbsolutePositioning |
                                          AM_SEEKING_ReturnTime,
                    nullptr, AM_SEEKING_NoPositioning);
                if (FAILED(seekHr)) stageFailure("restore-seek", seekHr);
                const HRESULT stateHr = wasRunning ? control->Run() : control->Pause();
                if (FAILED(stateHr)) stageFailure(wasRunning ? "restore-run" : "restore-pause", stateHr);
                running = wasRunning && SUCCEEDED(stateHr);
                if (!wasRunning && SUCCEEDED(stateHr)) running = false;
                frames.resumeAfterCommand();
                return SUCCEEDED(seekHr) && SUCCEEDED(stateHr);
            };
            hr = control->Stop();
            if (FAILED(hr)) stageFailure("stop", hr);
            if (SUCCEEDED(hr)) hr = splitterSettings->SetSubtitleMode(LAVSubtitleMode_Default);
            if (FAILED(hr)) stageFailure("subtitle-mode", hr);
            if (SUCCEEDED(hr)) hr = splitterSettings->SetPGSForcedStream(FALSE);
            if (FAILED(hr)) stageFailure("forced-mode", hr);
            if (SUCCEEDED(hr)) hr = splitterSettings->SetPGSOnlyForced(FALSE);
            if (FAILED(hr)) stageFailure("forced-only-mode", hr);
            if (SUCCEEDED(hr)) hr = selector->Enable(
                command.index, AMSTREAMSELECTENABLE_ENABLE);
            if (FAILED(hr)) stageFailure("stream-enable", hr);
            std::string subtitleConnectionDiagnostic;
            if (SUCCEEDED(hr)) hr = ensureSubtitleConnection(
                graph, source, subtitleSink, &subtitleConnectionDiagnostic);
            if (FAILED(hr)) stageFailure("connect", hr);
            if (SUCCEEDED(hr)) subtitles.activateEmbedded();
            const bool restored = restoreGraph();
            if (SUCCEEDED(hr) && !restored) hr = E_FAIL;
            if (SUCCEEDED(hr)) {
                enumerateSubtitleTracks(selector);
            } else {
                std::string message = hrError("PGS subtitle selection", hr);
                if (!subtitleLifecycleDiagnostic.empty()) {
                    message.append(" ");
                    message.append(subtitleLifecycleDiagnostic);
                }
                if (!subtitleConnectionDiagnostic.empty()) {
                    message.append(" ");
                    message.append(subtitleConnectionDiagnostic);
                }
                if (!restored) {
                    setFailure(message);
                    splitterSettings->SetSubtitleMode(LAVSubtitleMode_NoSubs);
                    enumerateSubtitleTracks(selector);
                    return false;
                }
                subtitles.selectionFailed(std::move(message));
                splitterSettings->SetSubtitleMode(LAVSubtitleMode_NoSubs);
                enumerateSubtitleTracks(selector);
                hr = S_OK;
            }
            break;
        }
        case CommandType::DisableSubtitle: {
            splitterSettings->SetSubtitleMode(LAVSubtitleMode_NoSubs);
            hr = S_OK;
            subtitles.disable();
            enumerateSubtitleTracks(selector);
            break;
        }
        case CommandType::Volume:
            hr = audio->put_Volume(mutedValue ? -10000 : volumeToDirectShow(
                                                        static_cast<float>(command.number)));
            if (SUCCEEDED(hr)) {
                std::lock_guard<std::mutex> lock(snapshotMutex);
                volumeScalar = static_cast<float>(command.number);
            }
            break;
        case CommandType::Mute:
            hr = audio->put_Volume(command.flag ? -10000 : volumeToDirectShow(volumeScalar));
            if (SUCCEEDED(hr)) {
                std::lock_guard<std::mutex> lock(snapshotMutex);
                mutedValue = command.flag;
            }
            break;
        }
        if (FAILED(hr)) {
            bool alreadyFailed = false;
            {
                std::lock_guard<std::mutex> lock(snapshotMutex);
                alreadyFailed = !errorValue.empty();
            }
            if (!alreadyFailed) setFailure(hrError("DirectShow command", hr));
            frames.cancelForCommand();
            control->Stop();
            return false;
        }
        return true;
    }

    void threadMain() {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comHr)) {
            setFailure(hrError("CoInitializeEx", comHr));
            return;
        }
        struct Apartment {
            ~Apartment() { CoUninitialize(); }
        } apartment;
        {
            IsoMount iso;
            std::wstring openedPath = path;
            if (isIsoPath(path)) {
                setOpeningStage(OpeningStage::MountingIso);
                const HRESULT mountHr = iso.open(path, 10000, cancellationEvent);
                if (FAILED(mountHr)) {
                    if (shouldStop() && mountHr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                        setStatus(Status::Stopped);
                    } else {
                        setFailure(hrError("ISO mount", mountHr));
                    }
                    return;
                }
                setIsoMountDiagnostics(true, iso.ownsAttachment());
                setOpeningStage(OpeningStage::ScanningTitles);
                const BluRayTitleScan scan =
                    enumerateBluRayTitles(iso.rootPath(), cancellationEvent);
                {
                    std::lock_guard<std::mutex> lock(snapshotMutex);
                    titleSnapshotValue = scan;
                }
                if (scan.cancelled) {
                    if (shouldStop()) setStatus(Status::Stopped);
                    else setFailure("Blu-ray title scan was cancelled");
                    return;
                }
                std::filesystem::path relative;
                std::wstring selectedLabel;
                if (options.playlistPath.empty() && !scan.titles.empty()) {
                    relative = scan.titles.front().relativePlaylistPath;
                    selectedLabel = scan.titles.front().label;
                } else if (options.playlistPath.empty()) {
                    relative = std::filesystem::path(L"BDMV\\index.bdmv");
                    selectedLabel = L"Automatic disc title";
                } else {
                    relative = std::filesystem::path(options.playlistPath);
                    selectedLabel = L"Explicit playlist";
                    const auto selected = std::find_if(
                        scan.titles.begin(), scan.titles.end(),
                        [&relative](const BluRayTitle& title) {
                            return sameRelativePath(title.relativePlaylistPath, relative);
                        });
                    if (selected != scan.titles.end()) selectedLabel = selected->label;
                }
                std::wstring selectedPath;
                if (!pathWithinMountedRoot(iso.rootPath(), relative, &selectedPath)) {
                    setFailure("MVC playlist path must stay within the mounted ISO");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(snapshotMutex);
                    selectedTitleRelativePlaylistPathValue = relative.wstring();
                    selectedTitleLabelValue = std::move(selectedLabel);
                }
                openedPath = std::move(selectedPath);
            } else if (!options.playlistPath.empty()) {
                setFailure("A relative playlist is valid only for ISO input");
                return;
            }

            Module splitterModule(options.lavDirectory + L"\\LAVSplitter.ax");
            Module videoModule(options.lavDirectory + L"\\LAVVideo.ax");
            Module audioModule(options.lavDirectory + L"\\LAVAudio.ax");
            if (!splitterModule || !videoModule || !audioModule) {
                setFailure(hrError("Private LAV load", HRESULT_FROM_WIN32(GetLastError())));
                return;
            }

            ComPtr<IGraphBuilder> graph;
            ComPtr<IBaseFilter> source;
            ComPtr<IBaseFilter> videoDecoder;
            ComPtr<IBaseFilter> audioDecoder;
            ComPtr<IBaseFilter> audioRenderer;
            ComPtr<IBaseFilter> videoSink;
            ComPtr<IBaseFilter> subtitleSink;
            HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&graph));
            if (FAILED(hr)) {
                setFailure(hrError("DirectShow graph creation", hr));
                return;
            }
            GraphLifetime graphLifetime(graph.Get(), &frames);
            hr = createPrivateFilter(splitterModule.get(), kClsidLavSplitterSource, &source);
            if (SUCCEEDED(hr)) hr = createPrivateFilter(videoModule.get(), kClsidLavVideo, &videoDecoder);
            if (SUCCEEDED(hr)) hr = createPrivateFilter(audioModule.get(), kClsidLavAudio, &audioDecoder);
            if (SUCCEEDED(hr)) {
                hr = CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&audioRenderer));
            }
            if (FAILED(hr)) {
                setFailure(hrError("DirectShow graph creation", hr));
                return;
            }

            ComPtr<ILAVFSettings> splitterSettings;
            ComPtr<ILAVVideoSettings> videoSettings;
            ComPtr<ILAVAudioSettings> audioSettings;
            ComPtr<ILAVAudioStatus> audioStatus;
            source.As(&splitterSettings);
            videoDecoder.As(&videoSettings);
            audioDecoder.As(&audioSettings);
            audioDecoder.As(&audioStatus);
            if (!splitterSettings || !videoSettings || !audioSettings || !audioStatus ||
                FAILED(splitterSettings->SetRuntimeConfig(TRUE)) ||
                FAILED(videoSettings->SetRuntimeConfig(TRUE)) ||
                FAILED(audioSettings->SetRuntimeConfig(TRUE)) ||
                FAILED(splitterSettings->SetSubtitleMode(LAVSubtitleMode_NoSubs)) ||
                FAILED(splitterSettings->SetPGSForcedStream(FALSE)) ||
                FAILED(splitterSettings->SetPGSOnlyForced(FALSE))) {
                setFailure("Private LAV runtime configuration is unavailable");
                return;
            }
            videoSettings->SetFormatConfiguration(Codec_H264MVC, TRUE);
            videoSettings->SetH264MVCDecodingOverride(TRUE);
            videoSettings->SetHWAccelCodec(HWCodec_H264MVC, FALSE);
            for (int format = 0; format < LAVOutPixFmt_NB; ++format) {
                videoSettings->SetPixelFormat(static_cast<LAVOutPixFmts>(format),
                                              format == LAVOutPixFmt_NV12);
            }

            hr = graph->AddFilter(source.Get(), L"Private LAV Splitter Source");
            if (SUCCEEDED(hr)) hr = graph->AddFilter(videoDecoder.Get(), L"Private LAV Video Decoder");
            if (SUCCEEDED(hr)) hr = graph->AddFilter(audioDecoder.Get(), L"Private LAV Audio Decoder");
            if (SUCCEEDED(hr)) hr = graph->AddFilter(audioRenderer.Get(), L"DirectSound Audio Renderer");
            subtitleSink.Attach(new (std::nothrow) SubtitleSinkFilter(&subtitles));
            if (!subtitleSink) hr = E_OUTOFMEMORY;
            if (SUCCEEDED(hr)) hr = graph->AddFilter(
                subtitleSink.Get(), L"Odyssey MVC Subtitle Sink");
            ComPtr<IFileSourceFilter> fileSource;
            if (SUCCEEDED(hr)) hr = source.As(&fileSource);
            if (SUCCEEDED(hr)) {
                setOpeningStage(OpeningStage::LoadingSource);
                hr = fileSource->Load(openedPath.c_str(), nullptr);
                setOpeningStage(OpeningStage::ConnectingGraph);
            }
            videoSink.Attach(new (std::nothrow) FrameSinkFilter(&frames));
            if (!videoSink) hr = E_OUTOFMEMORY;
            if (SUCCEEDED(hr)) hr = graph->AddFilter(videoSink.Get(), L"Odyssey MVC Frame Sink");

            ComPtr<IPin> videoSourcePin;
            AM_MEDIA_TYPE videoInputType{};
            if (SUCCEEDED(hr)) {
                hr = connectSourceVideo(graph.Get(), source.Get(), videoDecoder.Get(),
                                        videoSourcePin, videoInputType);
            }
            if (SUCCEEDED(hr)) freeMediaType(videoInputType);
            if (SUCCEEDED(hr)) hr = connectFirstOutput(graph.Get(), videoDecoder.Get(), videoSink.Get());
            if (FAILED(hr)) {
                setFailure(hrError("DirectShow MVC video connection", hr));
                return;
            }
            ComPtr<IPin> audioSourcePin;
            hr = connectSourceToDecoder(graph.Get(), source.Get(), audioDecoder.Get(), &audioSourcePin);
            if (SUCCEEDED(hr)) hr = connectFirstOutput(graph.Get(), audioDecoder.Get(), audioRenderer.Get());
            if (FAILED(hr)) {
                setFailure(hrError("DirectShow audio connection", hr));
                return;
            }

            graph->SetDefaultSyncSource();
            ComPtr<IMediaFilter> mediaFilter;
            ComPtr<IReferenceClock> graphClock;
            ComPtr<IReferenceClock> rendererClock;
            graph.As(&mediaFilter);
            if (mediaFilter) mediaFilter->GetSyncSource(&graphClock);
            audioRenderer.As(&rendererClock);
            if (!graphClock || !rendererClock || !sameComObject(graphClock.Get(), rendererClock.Get())) {
                setFailure("DirectShow did not select the audio renderer reference clock");
                return;
            }

            ComPtr<IMediaControl> control;
            ComPtr<IMediaSeeking> seeking;
            ComPtr<IMediaEventEx> events;
            ComPtr<IAMStreamSelect> selector;
            ComPtr<IBasicAudio> basicAudio;
            graph.As(&control);
            graph.As(&seeking);
            graph.As(&events);
            source.As(&selector);
            graph.As(&basicAudio);
            if (!control || !seeking || !events || !selector || !basicAudio) {
                setFailure("DirectShow graph is missing required playback interfaces");
                return;
            }
            bool audioDiagnosticsEnabled = false;
            if (options.enableAudioDiagnostics) {
                hr = audioStatus->EnableVolumeStats();
                if (FAILED(hr)) {
                    setFailure(hrError("LAV Audio volume diagnostics", hr));
                    return;
                }
                audioDiagnosticsEnabled = true;
            }
            hr = seeking->SetTimeFormat(&TIME_FORMAT_MEDIA_TIME);
            if (FAILED(hr)) {
                setFailure(hrError("MVC media time format", hr));
                return;
            }
            REFERENCE_TIME duration = 0;
            hr = seeking->GetDuration(&duration);
            if (FAILED(hr) || duration <= 0) {
                setFailure(hrError("MVC duration query", FAILED(hr) ? hr : E_FAIL));
                return;
            }
            {
                std::lock_guard<std::mutex> lock(snapshotMutex);
                durationTicks = duration;
            }
            enumerateAudioTracks(selector.Get());
            enumerateSubtitleTracks(selector.Get());
            hr = basicAudio->put_Volume(mutedValue ? -10000 : volumeToDirectShow(volumeScalar));
            bool running = !options.startPaused;
            if (SUCCEEDED(hr)) hr = running ? control->Run() : control->Pause();
            if (FAILED(hr)) {
                setFailure(hrError("DirectShow playback start", hr));
                return;
            }
            setStatus(running ? Status::Running : Status::Paused);
            setOpeningStage(OpeningStage::Ready);

            bool graphComplete = false;
            bool keepRunning = true;
            while (keepRunning && !shouldStop()) {
                std::deque<Command> pending;
                {
                    std::unique_lock<std::mutex> lock(commandMutex);
                    commandAvailable.wait_for(lock, std::chrono::milliseconds(5),
                                              [&] { return stopping || !commands.empty(); });
                    pending.swap(commands);
                }
                for (const Command& command : pending) {
                    if (!handleCommand(command, control.Get(), seeking.Get(), selector.Get(),
                                       basicAudio.Get(), graphClock.Get(), graph.Get(), source.Get(),
                                       subtitleSink.Get(), splitterSettings.Get(), running)) {
                        keepRunning = false;
                        break;
                    }
                    graphComplete = false;
                }
                updateClock(graphClock.Get(), running);
                subtitles.updateMediaTime(snapshotPositionNanoseconds());
                if (options.enableAudioDiagnostics) updateAudioContent(audioStatus.Get());

                long eventCode = 0;
                LONG_PTR first = 0;
                LONG_PTR second = 0;
                while (events->GetEvent(&eventCode, &first, &second, 0) == S_OK) {
                    events->FreeEventParams(eventCode, first, second);
                    if (eventCode == EC_COMPLETE) graphComplete = true;
                    else if (eventCode == EC_ERRORABORT) {
                        setFailure("DirectShow playback aborted");
                        keepRunning = false;
                    }
                }
                const std::string frameError = frames.error();
                if (!frameError.empty()) {
                    setFailure(frameError);
                    keepRunning = false;
                }
                bool audioNearEnd = false;
                {
                    std::lock_guard<std::mutex> lock(snapshotMutex);
                    const REFERENCE_TIME tolerance = kTicksPerSecond / 2;
                    audioNearEnd = durationTicks <= tolerance ||
                                   positionTicks >= durationTicks - tolerance;
                }
                if (frames.videoEos() && frames.empty() && (graphComplete || audioNearEnd)) {
                    if (running) control->Pause();
                    setStatus(Status::Drained);
                    running = false;
                }
            }
            if (audioDiagnosticsEnabled) audioStatus->DisableVolumeStats();
            if (status() != Status::Failed) setStatus(Status::Stopped);
        }
    }

    Status status() const {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        return statusValue;
    }

    const std::wstring path;
    const Options options;
    FrameState frames;
    SubtitleState subtitles;
    mutable std::mutex commandMutex;
    std::condition_variable commandAvailable;
    std::deque<Command> commands;
    bool stopping{false};
    HANDLE cancellationEvent{nullptr};
    std::thread worker;

    mutable std::mutex snapshotMutex;
    Status statusValue{Status::Opening};
    OpeningStage openingStage{OpeningStage::Starting};
    bool isoMounted{false};
    bool isoOwned{false};
    std::string errorValue;
    std::vector<AudioTrack> tracks;
    std::vector<SubtitleTrack> subtitleTracksValue;
    BluRayTitleScan titleSnapshotValue;
    std::wstring selectedTitleRelativePlaylistPathValue;
    std::wstring selectedTitleLabelValue;
    long selectedTrack{-1};
    REFERENCE_TIME positionTicks{0};
    REFERENCE_TIME durationTicks{0};
    float volumeScalar{1.0f};
    bool mutedValue{false};
    bool audioContent{false};
};

MvcPipeline::MvcPipeline(const std::wstring& path, const Options& options)
    : impl_(std::make_unique<Impl>(path, options)) {}

MvcPipeline::~MvcPipeline() = default;

AVFrame* MvcPipeline::pollLatest() { return impl_->frames.poll(); }
void MvcPipeline::clear() { impl_->frames.clear(); }
bool MvcPipeline::pause() { return impl_->enqueue({Impl::CommandType::Pause}); }
bool MvcPipeline::resume() { return impl_->enqueue({Impl::CommandType::Resume}); }
bool MvcPipeline::seekSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) return false;
    const long double scaled = static_cast<long double>(seconds) *
                               static_cast<long double>(kTicksPerSecond);
    if (scaled < 0.0L || scaled >= 9223372036854775808.0L) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
        if (impl_->durationTicks <= 0 || scaled > impl_->durationTicks) return false;
    }
    return impl_->enqueue({Impl::CommandType::Seek, seconds});
}
void MvcPipeline::stop() { impl_->stopAndJoin(); }

std::vector<MvcPipeline::AudioTrack> MvcPipeline::audioTracks() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->tracks;
}
long MvcPipeline::selectedAudioTrack() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->selectedTrack;
}
bool MvcPipeline::selectAudioTrack(long streamIndex) {
    const auto available = audioTracks();
    const auto found = std::find_if(available.begin(), available.end(),
                                    [streamIndex](const AudioTrack& track) {
                                        return track.streamIndex == streamIndex;
                                    });
    return found != available.end() &&
           impl_->enqueue({Impl::CommandType::SelectAudio, 0.0, streamIndex});
}
bool MvcPipeline::setVolume(float scalar) {
    if (!std::isfinite(scalar) || scalar < 0.0f || scalar > 1.0f) return false;
    return impl_->enqueue({Impl::CommandType::Volume, scalar});
}
void MvcPipeline::setMuted(bool muted) {
    impl_->enqueue({Impl::CommandType::Mute, 0.0, -1, muted});
}
std::vector<MvcPipeline::SubtitleTrack> MvcPipeline::subtitleTracks() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->subtitleTracksValue;
}
std::optional<long> MvcPipeline::selectedSubtitleTrack() const {
    if (impl_->subtitles.externalSelected()) return std::nullopt;
    const long selected = impl_->subtitles.selectedStream();
    return selected >= 0 ? std::optional<long>(selected) : std::nullopt;
}
bool MvcPipeline::selectSubtitleTrack(long streamIndex) {
    const auto available = subtitleTracks();
    const auto found = std::find_if(available.begin(), available.end(),
                                    [streamIndex](const SubtitleTrack& track) {
                                        return track.streamIndex == streamIndex &&
                                               track.supported;
                                    });
    if (found == available.end()) return false;
    impl_->subtitles.prepareEmbedded(streamIndex);
    if (!impl_->enqueue({Impl::CommandType::SelectSubtitle, 0.0, streamIndex})) {
        impl_->subtitles.disable();
        return false;
    }
    return true;
}
void MvcPipeline::disableSubtitles() {
    impl_->subtitles.disable();
    impl_->enqueue({Impl::CommandType::DisableSubtitle});
}
bool MvcPipeline::loadExternalSubRip(const std::wstring& path, std::string* error) {
    const Status current = status();
    if (current == Status::Stopped || current == Status::Failed) {
        if (error) *error = "pipeline is stopped";
        return false;
    }
    if (!impl_->subtitles.loadExternal(path, error)) return false;
    impl_->enqueue({Impl::CommandType::DisableSubtitle});
    return true;
}
bool MvcPipeline::externalSubtitlesSelected() const {
    return impl_->subtitles.externalSelected();
}
bool MvcPipeline::setSubtitleOffsetSeconds(double seconds) {
    const Status current = status();
    return current != Status::Stopped && current != Status::Failed &&
           impl_->subtitles.setOffset(seconds);
}
std::vector<SubtitleCue> MvcPipeline::subtitleCuesAt(int64_t mediaNanoseconds) const {
    return impl_->subtitles.activeCues(mediaNanoseconds);
}
std::string MvcPipeline::subtitleError() const { return impl_->subtitles.error(); }
BluRayTitleScan MvcPipeline::titleSnapshot() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->titleSnapshotValue;
}
std::wstring MvcPipeline::selectedTitleRelativePlaylistPath() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->selectedTitleRelativePlaylistPathValue;
}
std::wstring MvcPipeline::selectedTitleLabel() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->selectedTitleLabelValue;
}
double MvcPipeline::positionSeconds() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return static_cast<double>(impl_->positionTicks) / kTicksPerSecond;
}
double MvcPipeline::durationSeconds() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return static_cast<double>(impl_->durationTicks) / kTicksPerSecond;
}
int64_t MvcPipeline::mediaTimeNanoseconds() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    if (impl_->positionTicks > std::numeric_limits<int64_t>::max() / 100) {
        return std::numeric_limits<int64_t>::max();
    }
    return impl_->positionTicks * 100;
}
uint64_t MvcPipeline::generation() const { return impl_->frames.generation(); }
MvcPipeline::Status MvcPipeline::status() const { return impl_->status(); }
std::string MvcPipeline::error() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->errorValue;
}
bool MvcPipeline::paused() const { return status() == Status::Paused; }
bool MvcPipeline::finished() const {
    const Status value = status();
    return value == Status::Drained || value == Status::Stopped || value == Status::Failed;
}
bool MvcPipeline::audioContentObserved() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return impl_->audioContent;
}
MvcPipeline::TimingDiagnostics MvcPipeline::timingDiagnostics() const {
    return impl_->frames.timingDiagnostics();
}
MvcPipeline::OpeningDiagnostics MvcPipeline::openingDiagnostics() const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    return {impl_->openingStage, impl_->isoMounted, impl_->isoOwned};
}
int MvcPipeline::width() const { return impl_->frames.width(); }
int MvcPipeline::height() const { return impl_->frames.height(); }

} // namespace odyssey

