#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/client.h>

#include "IMediaSample3D.h"
#include "LAVSplitterSettings.h"
#include "LAVVideoSettings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr GUID kClsidLavVideo = {0xee30215d, 0x164f, 0x4a92, {0xa4, 0xeb, 0x9d, 0x4c, 0x13, 0x39, 0x0f, 0x9f}};
constexpr GUID kClsidLavSplitterSource = {0xb98d13e7, 0x55db, 0x4385,
                                          {0xa3, 0x3d, 0x09, 0xfd, 0x1b, 0xa2, 0x63, 0x38}};
constexpr GUID kClsidProbeSink = {0x8ba75392, 0xba23, 0x41ad,
                                  {0xb2, 0x1c, 0xd9, 0xb4, 0xcf, 0xd3, 0x8b, 0xf1}};
constexpr REFERENCE_TIME kTicksPerSecond = 10'000'000;

std::wstring hrText(HRESULT hr) {
    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wostringstream out;
    out << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    if (message) {
        out << L" (" << message << L")";
        LocalFree(message);
    }
    return out.str();
}

std::wstring guidText(REFGUID guid) {
    wchar_t text[64]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    return text;
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

struct PhaseStats {
    uint64_t samples{0};
    uint64_t mvcPairs{0};
    uint64_t distinctPairs{0};
    uint64_t discontinuities{0};
    uint64_t missingTimestamps{0};
    uint64_t timestampRegressions{0};
    uint64_t timestampGaps{0};
    uint64_t malformedPairs{0};
    uint64_t baseDigest{1469598103934665603ULL};
    uint64_t dependentDigest{1469598103934665603ULL};
    REFERENCE_TIME firstPts{0};
    REFERENCE_TIME lastPts{0};
    REFERENCE_TIME maxGap{0};
    bool havePts{false};
    bool eos{false};
    long segments{0};
    VideoFormat format{};
    double wallSeconds{0.0};
};

class ProbeState {
public:
    void setFormat(const AM_MEDIA_TYPE& type) {
        VideoFormat format{};
        format.subtype = type.subtype;
        format.formatType = type.formattype;
        format.formatBytes = type.cbFormat;
        const RECT* source = nullptr;
        const BITMAPINFOHEADER* bitmap = nullptr;
        if (type.formattype == FORMAT_VideoInfo2 && type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
            const auto* info = reinterpret_cast<const VIDEOINFOHEADER2*>(type.pbFormat);
            source = &info->rcSource;
            bitmap = &info->bmiHeader;
            format.frameDuration = info->AvgTimePerFrame;
        } else if (type.formattype == FORMAT_VideoInfo && type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
            const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat);
            source = &info->rcSource;
            bitmap = &info->bmiHeader;
            format.frameDuration = info->AvgTimePerFrame;
        }
        if (!bitmap || (source && (source->left != 0 || source->top != 0))) {
            std::lock_guard<std::mutex> lock(mutex_);
            format_ = {};
            return;
        }
        format.stride = bitmap->biWidth;
        format.storageHeight = std::abs(bitmap->biHeight);
        format.activeWidth = source && source->right > source->left
                                 ? source->right - source->left
                                 : bitmap->biWidth;
        format.activeHeight = source && source->bottom > source->top
                                  ? source->bottom - source->top
                                  : std::abs(bitmap->biHeight);
        std::lock_guard<std::mutex> lock(mutex_);
        format_ = format;
    }

    void begin(double seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_ = {};
        stats_.format = format_;
        target_ = static_cast<REFERENCE_TIME>(seconds * static_cast<double>(kTicksPerSecond));
        started_ = std::chrono::steady_clock::now();
        done_ = false;
    }

    HRESULT receive(IMediaSample* sample) {
        if (!sample) return E_POINTER;
        AM_MEDIA_TYPE* changed = nullptr;
        if (sample->GetMediaType(&changed) == S_OK && changed) {
            setFormat(*changed);
            freeMediaType(*changed);
            CoTaskMemFree(changed);
        }

        BYTE* primary = nullptr;
        const HRESULT pointerHr = sample->GetPointer(&primary);
        ComPtr<IMediaSample3D> sample3d;
        sample->QueryInterface(IID_PPV_ARGS(&sample3d));
        auto* owned = sample3d ? dynamic_cast<StereoSample*>(sample3d.Get()) : nullptr;

        REFERENCE_TIME start = 0;
        REFERENCE_TIME stop = 0;
        const HRESULT timeHr = sample->GetTime(&start, &stop);

        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.samples;
        stats_.format = format_;
        if (sample->IsDiscontinuity() == S_OK) ++stats_.discontinuities;
        recordTimestamp(timeHr, start);

        if (SUCCEEDED(pointerHr) && primary && owned && owned->stereoEnabled() && validFormat(format_)) {
            BYTE* dependent = nullptr;
            if (SUCCEEDED(sample3d->GetPointer3D(&dependent)) && dependent &&
                buffersFit(sample->GetSize(), format_)) {
                const uint64_t baseHash = hashActiveNv12(primary, format_);
                const uint64_t dependentHash = hashActiveNv12(dependent, format_);
                stats_.baseDigest = mix(stats_.baseDigest, baseHash);
                stats_.dependentDigest = mix(stats_.dependentDigest, dependentHash);
                ++stats_.mvcPairs;
                if (baseHash != dependentHash) ++stats_.distinctPairs;
            } else {
                ++stats_.malformedPairs;
            }
        }

        const REFERENCE_TIME span = stats_.havePts ? stats_.lastPts - stats_.firstPts : 0;
        if ((stats_.mvcPairs == 0 && span >= 2 * kTicksPerSecond) ||
            (stats_.mvcPairs > 0 && span >= target_)) {
            done_ = true;
            completed_.notify_all();
        }
        return S_OK;
    }

    void segment() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.segments;
    }
    void eos() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.eos = true;
        done_ = true;
        completed_.notify_all();
    }
    bool wait(std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return completed_.wait_for(lock, timeout, [&] { return done_; });
    }
    PhaseStats snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        PhaseStats copy = stats_;
        copy.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        return copy;
    }

private:
    static bool validFormat(const VideoFormat& format) {
        return format.subtype == MEDIASUBTYPE_NV12 && format.activeWidth > 0 &&
               format.activeHeight > 0 && format.stride >= format.activeWidth &&
               format.storageHeight >= format.activeHeight;
    }
    static bool buffersFit(long size, const VideoFormat& format) {
        const int64_t required = static_cast<int64_t>(format.stride) * format.storageHeight * 3 / 2;
        return size >= 0 && required <= size;
    }
    static uint64_t hashActiveNv12(const BYTE* data, const VideoFormat& format) {
        uint64_t hash = 1469598103934665603ULL;
        const auto hashRows = [&](const BYTE* plane, long rows) {
            for (long y = 0; y < rows; ++y) {
                const BYTE* row = plane + static_cast<size_t>(y) * format.stride;
                for (long x = 0; x < format.activeWidth; ++x) {
                    hash ^= row[x];
                    hash *= 1099511628211ULL;
                }
            }
        };
        hashRows(data, format.activeHeight);
        hashRows(data + static_cast<size_t>(format.stride) * format.storageHeight,
                 format.activeHeight / 2);
        return hash;
    }
    static uint64_t mix(uint64_t aggregate, uint64_t frame) {
        aggregate ^= frame;
        aggregate *= 1099511628211ULL;
        return aggregate;
    }
    void recordTimestamp(HRESULT timeHr, REFERENCE_TIME start) {
        if (FAILED(timeHr)) {
            ++stats_.missingTimestamps;
            return;
        }
        if (!stats_.havePts) {
            stats_.havePts = true;
            stats_.firstPts = start;
            stats_.lastPts = start;
            return;
        }
        const REFERENCE_TIME gap = start - stats_.lastPts;
        if (gap < 0) ++stats_.timestampRegressions;
        else {
            stats_.maxGap = std::max(stats_.maxGap, gap);
            if (format_.frameDuration > 0 && gap > format_.frameDuration * 2) ++stats_.timestampGaps;
        }
        stats_.lastPts = start;
    }

    mutable std::mutex mutex_;
    std::condition_variable completed_;
    VideoFormat format_{};
    PhaseStats stats_{};
    REFERENCE_TIME target_{0};
    std::chrono::steady_clock::time_point started_{};
    bool done_{false};
};

class ProbeFilter;

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

class ProbePin final : public IPin, public IMemInputPin {
public:
    ProbePin(ProbeFilter* owner, ProbeState* state)
        : owner_(owner), state_(state) {
        allocator_.Attach(new StereoAllocator());
    }
    ~ProbePin() {
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
        const HRESULT hr = copyMediaType(mediaType_, *type);
        if (SUCCEEDED(hr)) state_->setFormat(*type);
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
        state_->eos();
        return S_OK;
    }
    STDMETHODIMP BeginFlush() override {
        flushing_ = true;
        return S_OK;
    }
    STDMETHODIMP EndFlush() override {
        flushing_ = false;
        return S_OK;
    }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override {
        state_->segment();
        return S_OK;
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
        return flushing_ ? S_FALSE : state_->receive(sample);
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
    STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

private:
    std::atomic<ULONG> references_{1};
    ProbeFilter* owner_;
    ProbeState* state_;
    ComPtr<StereoAllocator> allocator_;
    IPin* connected_{nullptr};
    AM_MEDIA_TYPE mediaType_{};
    std::atomic<bool> flushing_{false};
};

class ProbeFilter final : public IBaseFilter {
public:
    explicit ProbeFilter(ProbeState* state) : pin_(new ProbePin(this, state)) {}
    ~ProbeFilter() {
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
        *clsid = kClsidProbeSink;
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
    STDMETHODIMP Run(REFERENCE_TIME) override {
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
        wcscpy_s(info->achName, L"Odyssey MVC Probe Sink");
        info->pGraph = graph_;
        if (graph_) graph_->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override {
        graph_ = graph;
        name_ = name ? name : L"";
        return S_OK;
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
    ProbePin* pin_;
    FILTER_STATE state_{State_Stopped};
    IReferenceClock* clock_{nullptr};
    IFilterGraph* graph_{nullptr};
    std::wstring name_;
};

STDMETHODIMP ProbePin::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IPin)) *object = static_cast<IPin*>(this);
    else if (riid == __uuidof(IMemInputPin)) *object = static_cast<IMemInputPin*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP ProbePin::QueryPinInfo(PIN_INFO* info) {
    if (!info) return E_POINTER;
    info->pFilter = owner_;
    owner_->AddRef();
    info->dir = PINDIR_INPUT;
    wcscpy_s(info->achName, L"Input");
    return S_OK;
}

class Module {
public:
    explicit Module(const std::wstring& path) : handle_(LoadLibraryW(path.c_str())) {}
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

std::wstring pinName(IPin* pin) {
    PIN_INFO info{};
    if (FAILED(pin->QueryPinInfo(&info))) return L"?";
    if (info.pFilter) info.pFilter->Release();
    return info.achName;
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

bool phasePasses(const PhaseStats& stats, double targetSeconds);

void printStats(const wchar_t* name, const PhaseStats& stats, double targetSeconds) {
    const double span = stats.havePts
                            ? static_cast<double>(stats.lastPts - stats.firstPts) / kTicksPerSecond
                            : 0.0;
    const double sourceFps = stats.format.frameDuration > 0
                                 ? static_cast<double>(kTicksPerSecond) / stats.format.frameDuration
                                 : 0.0;
    const double decodeFps = stats.wallSeconds > 0 ? stats.mvcPairs / stats.wallSeconds : 0.0;
    const bool pass = phasePasses(stats, targetSeconds);

    std::wcout << L"PHASE name=" << name << L" result=" << (pass ? L"PASS" : L"FAIL")
               << L" subtype=" << guidText(stats.format.subtype)
               << L" format_type=" << guidText(stats.format.formatType)
               << L" format_bytes=" << stats.format.formatBytes
               << L" dimensions=" << stats.format.activeWidth << L"x" << stats.format.activeHeight
               << L" stride=" << stats.format.stride
               << L" samples=" << stats.samples
               << L" mvc_pairs=" << stats.mvcPairs
               << L" distinct_pairs=" << stats.distinctPairs
               << L" first_pts=" << stats.firstPts
               << L" last_pts=" << stats.lastPts
               << L" span_seconds=" << std::fixed << std::setprecision(3) << span
               << L" wall_seconds=" << stats.wallSeconds
               << L" source_fps=" << sourceFps
               << L" decode_fps=" << decodeFps
               << L" pts_regressions=" << stats.timestampRegressions
               << L" pts_gaps=" << stats.timestampGaps
               << L" max_gap_ms=" << (static_cast<double>(stats.maxGap) * 1000.0 / kTicksPerSecond)
               << L" missing_pts=" << stats.missingTimestamps
               << L" malformed_pairs=" << stats.malformedPairs
               << L" discontinuities=" << stats.discontinuities
               << L" segments=" << stats.segments
               << L" base_digest=0x" << std::hex << stats.baseDigest
               << L" dependent_digest=0x" << stats.dependentDigest << std::dec << L'\n';
}

bool phasePasses(const PhaseStats& stats, double targetSeconds) {
    if (stats.format.frameDuration <= 0 || stats.wallSeconds <= 0) return false;
    const double span = stats.havePts
                            ? static_cast<double>(stats.lastPts - stats.firstPts) / kTicksPerSecond
                            : 0.0;
    const double sourceFps = static_cast<double>(kTicksPerSecond) / stats.format.frameDuration;
    const double decodeFps = stats.mvcPairs / stats.wallSeconds;
    const uint64_t expected = static_cast<uint64_t>(targetSeconds * sourceFps * 0.95);
    return stats.mvcPairs == stats.samples && stats.mvcPairs >= expected &&
           stats.distinctPairs > 0 && span >= targetSeconds && stats.missingTimestamps == 0 &&
           stats.timestampRegressions == 0 && stats.timestampGaps == 0 &&
           stats.malformedPairs == 0 && decodeFps >= sourceFps;
}

struct Arguments {
    std::wstring input;
    std::wstring lavDir;
    double seconds{60};
    double seekSeconds{1800};
};

bool parseArguments(int argc, wchar_t** argv, Arguments& args) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (i + 1 >= argc) return false;
        if (option == L"--input") args.input = argv[++i];
        else if (option == L"--lav-dir") args.lavDir = argv[++i];
        else if (option == L"--seconds") args.seconds = std::stod(argv[++i]);
        else if (option == L"--seek-seconds") args.seekSeconds = std::stod(argv[++i]);
        else return false;
    }
    return !args.input.empty() && !args.lavDir.empty() && args.seconds > 0 && args.seekSeconds >= 0;
}

int runPhase(IMediaControl* control, ProbeState& state, const wchar_t* name, double seconds) {
    state.begin(seconds);
    HRESULT hr = control->Run();
    if (FAILED(hr)) {
        std::wcerr << L"ERROR graph Run failed: " << hrText(hr) << L'\n';
        return 30;
    }
    const auto timeout = std::chrono::seconds(static_cast<long long>(std::max(30.0, seconds * 5.0)));
    const bool completed = state.wait(timeout);
    control->Stop();
    const PhaseStats stats = state.snapshot();
    printStats(name, stats, seconds);
    if (!completed) {
        std::wcerr << L"ERROR phase timed out before enough decoded media was delivered\n";
        return 31;
    }
    if (stats.mvcPairs == 0) {
        std::wcerr << L"ERROR no sample was explicitly enabled as MVC by LAV Video\n";
        return 23;
    }
    return phasePasses(stats, seconds) ? 0 : 24;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Arguments args;
    try {
        if (!parseArguments(argc, argv, args)) {
            std::wcerr << L"usage: mvc_probe --input <path> --lav-dir <dir> --seconds <n> --seek-seconds <n>\n";
            return 2;
        }
    } catch (const std::exception&) {
        std::wcerr << L"ERROR invalid numeric argument\n";
        return 2;
    }

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init)) {
        std::wcerr << L"ERROR COM initialization failed: " << hrText(init) << L'\n';
        return 3;
    }

    int result = 0;
    {
        SetDllDirectoryW(args.lavDir.c_str());
        Module splitterModule(args.lavDir + L"\\LAVSplitter.ax");
        Module videoModule(args.lavDir + L"\\LAVVideo.ax");
        if (!splitterModule || !videoModule) {
            std::wcerr << L"ERROR loading private LAV filters: " << hrText(HRESULT_FROM_WIN32(GetLastError())) << L'\n';
            result = 4;
        } else {
            ProbeState state;
            ComPtr<IGraphBuilder> graph;
            ComPtr<IBaseFilter> source;
            ComPtr<IBaseFilter> decoder;
            HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&graph));
            if (SUCCEEDED(hr)) hr = createPrivateFilter(splitterModule.get(), kClsidLavSplitterSource, &source);
            if (SUCCEEDED(hr)) hr = createPrivateFilter(videoModule.get(), kClsidLavVideo, &decoder);
            if (FAILED(hr)) {
                std::wcerr << L"ERROR creating private graph/filter: " << hrText(hr) << L'\n';
                result = 5;
            } else {
                ComPtr<ILAVFSettings> splitterSettings;
                ComPtr<ILAVVideoSettings> videoSettings;
                source.As(&splitterSettings);
                decoder.As(&videoSettings);
                if (!splitterSettings || !videoSettings ||
                    FAILED(splitterSettings->SetRuntimeConfig(TRUE)) ||
                    FAILED(videoSettings->SetRuntimeConfig(TRUE))) {
                    std::wcerr << L"ERROR LAV runtime-only configuration is unavailable\n";
                    result = 6;
                } else {
                    videoSettings->SetFormatConfiguration(Codec_H264MVC, TRUE);
                    videoSettings->SetH264MVCDecodingOverride(TRUE);
                    videoSettings->SetHWAccelCodec(HWCodec_H264MVC, FALSE);
                    for (int format = 0; format < LAVOutPixFmt_NB; ++format) {
                        videoSettings->SetPixelFormat(static_cast<LAVOutPixFmts>(format),
                                                      format == LAVOutPixFmt_NV12);
                    }

                    hr = graph->AddFilter(source.Get(), L"Private LAV Splitter Source");
                    if (SUCCEEDED(hr)) hr = graph->AddFilter(decoder.Get(), L"Private LAV Video Decoder");
                    ComPtr<IFileSourceFilter> fileSource;
                    if (SUCCEEDED(hr)) hr = source.As(&fileSource);
                    if (SUCCEEDED(hr)) hr = fileSource->Load(args.input.c_str(), nullptr);
                    if (FAILED(hr)) {
                        std::wcerr << L"ERROR source could not open input: " << hrText(hr) << L'\n';
                        result = 20;
                    } else {
                        ComPtr<IBaseFilter> sink;
                        sink.Attach(new ProbeFilter(&state));
                        hr = graph->AddFilter(sink.Get(), L"Odyssey MVC Probe Sink");
                        ComPtr<IPin> sourceVideo;
                        AM_MEDIA_TYPE inputType{};
                        if (SUCCEEDED(hr)) {
                            hr = connectSourceVideo(graph.Get(), source.Get(), decoder.Get(), sourceVideo, inputType);
                        }
                        if (FAILED(hr)) {
                            std::wcerr << L"ERROR no LAV video path: " << hrText(hr) << L'\n';
                            result = 21;
                        } else {
                            std::wcout << L"INPUT path=" << args.input
                                       << L" pin=" << pinName(sourceVideo.Get())
                                       << L" subtype=" << guidText(inputType.subtype) << L'\n';
                            freeMediaType(inputType);
                            auto decoderOutputs = pins(decoder.Get(), PINDIR_OUTPUT);
                            auto sinkInputs = pins(sink.Get(), PINDIR_INPUT);
                            if (decoderOutputs.empty() || sinkInputs.empty()) hr = VFW_E_NOT_FOUND;
                            else hr = graph->ConnectDirect(decoderOutputs[0].Get(), sinkInputs[0].Get(), nullptr);
                            if (FAILED(hr)) {
                                std::wcerr << L"ERROR decoder-to-MVC-sink connection failed: " << hrText(hr) << L'\n';
                                result = 22;
                            } else {
                                source->SetSyncSource(nullptr);
                                decoder->SetSyncSource(nullptr);
                                sink->SetSyncSource(nullptr);
                                ComPtr<IMediaControl> control;
                                graph.As(&control);
                                result = runPhase(control.Get(), state, L"initial", args.seconds);
                                if (result == 0) {
                                    ComPtr<IMediaSeeking> seeking;
                                    source.As(&seeking);
                                    if (!seeking) graph.As(&seeking);
                                    LONGLONG duration = 0;
                                    LONGLONG position = static_cast<LONGLONG>(args.seekSeconds * kTicksPerSecond);
                                    hr = seeking ? seeking->GetDuration(&duration) : E_NOINTERFACE;
                                    if (SUCCEEDED(hr) && position < duration) {
                                        hr = seeking->SetPositions(&position, AM_SEEKING_AbsolutePositioning,
                                                                   nullptr, AM_SEEKING_NoPositioning);
                                    } else if (SUCCEEDED(hr)) {
                                        hr = E_INVALIDARG;
                                    }
                                    if (FAILED(hr)) {
                                        std::wcerr << L"ERROR seek failed: " << hrText(hr) << L'\n';
                                        result = 25;
                                    } else {
                                        result = runPhase(control.Get(), state, L"seek", args.seconds);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        SetDllDirectoryW(nullptr);
    }
    CoUninitialize();
    return result;
}
