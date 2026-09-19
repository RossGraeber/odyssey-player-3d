#include "AudioOutput.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace odyssey {

AudioOutput::~AudioOutput() {
    releaseResources();
}

HRESULT AudioOutput::initialize() noexcept {
    if (initialized_) {
        return AUDCLNT_E_ALREADY_INITIALIZED;
    }

    ownerThreadId_ = GetCurrentThreadId();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        uninitializeCom_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        ownerThreadId_ = 0;
        return hr;
    }

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> device;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    }
    if (SUCCEEDED(hr)) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(audioClient_.GetAddressOf()));
    }
    if (SUCCEEDED(hr)) {
        hr = audioClient_->GetMixFormat(&mixFormat_);
    }
    if (SUCCEEDED(hr) &&
        (!mixFormat_ || mixFormat_->nBlockAlign == 0 || mixFormat_->nSamplesPerSec == 0)) {
        hr = E_UNEXPECTED;
    }
    if (SUCCEEDED(hr)) {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }
    if (SUCCEEDED(hr)) {
        // 100 ms buffer (headroom against feeder-thread stalls); media clock reads
        // IAudioClock so playback position is unaffected.
        hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                      1'000'000, 0, mixFormat_, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = audioClient_->SetEventHandle(event_);
    }
    if (SUCCEEDED(hr)) {
        hr = audioClient_->GetBufferSize(&bufferFrameCapacity_);
    }
    if (SUCCEEDED(hr)) {
        hr = audioClient_->GetService(IID_PPV_ARGS(&renderClient_));
    }
    if (SUCCEEDED(hr)) {
        hr = audioClient_->GetService(IID_PPV_ARGS(&audioClock_));
    }
    if (SUCCEEDED(hr)) {
        hr = audioClient_->GetService(IID_PPV_ARGS(&volume_));
    }
    if (SUCCEEDED(hr)) {
        hr = audioClock_->GetFrequency(&clockFrequency_);
        if (SUCCEEDED(hr) && clockFrequency_ == 0) {
            hr = E_UNEXPECTED;
        }
    }

    if (FAILED(hr)) {
        device.Reset();
        enumerator.Reset();
        releaseResources();
        return hr;
    }

    initialized_ = true;
    return S_OK;
}

HRESULT AudioOutput::availableFrames(UINT32* frames) const noexcept {
    if (!frames) {
        return E_POINTER;
    }
    const HRESULT state = checkThreadAndInitialized();
    if (FAILED(state)) {
        return state;
    }

    UINT32 padding = 0;
    const HRESULT hr = audioClient_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        return hr;
    }
    if (padding > bufferFrameCapacity_) {
        return E_UNEXPECTED;
    }
    *frames = bufferFrameCapacity_ - padding;
    return S_OK;
}

HRESULT AudioOutput::write(const void* interleavedPcm, size_t byteCount,
                           int64_t firstFramePtsNanoseconds) noexcept {
    const HRESULT state = checkThreadAndInitialized();
    if (FAILED(state)) {
        return state;
    }
    if (byteCount == 0) {
        return S_OK;
    }
    if (!interleavedPcm) {
        return E_POINTER;
    }

    const size_t blockAlign = mixFormat_->nBlockAlign;
    if (byteCount % blockAlign != 0) {
        return E_INVALIDARG;
    }
    const size_t frameCount = byteCount / blockAlign;
    if (frameCount > (std::numeric_limits<UINT32>::max)()) {
        return E_INVALIDARG;
    }

    UINT32 available = 0;
    HRESULT hr = availableFrames(&available);
    if (FAILED(hr)) {
        return hr;
    }
    if (frameCount > available) {
        return AUDCLNT_E_BUFFER_TOO_LARGE;
    }

    UINT64 anchorPosition = 0;
    if (!anchored_) {
        hr = audioClock_->GetPosition(&anchorPosition, nullptr);
        if (FAILED(hr)) {
            return hr;
        }
    }

    BYTE* destination = nullptr;
    hr = renderClient_->GetBuffer(static_cast<UINT32>(frameCount), &destination);
    if (FAILED(hr)) {
        return hr;
    }
    std::memcpy(destination, interleavedPcm, byteCount);
    hr = renderClient_->ReleaseBuffer(static_cast<UINT32>(frameCount), 0);
    if (FAILED(hr)) {
        return hr;
    }

    if (!anchored_) {
        anchorClockPosition_ = anchorPosition;
        anchorPtsNanoseconds_ = firstFramePtsNanoseconds;
        anchored_ = true;
    }
    return S_OK;
}

HRESULT AudioOutput::start() noexcept {
    const HRESULT state = checkThreadAndInitialized();
    if (FAILED(state)) {
        return state;
    }
    if (started_) {
        return S_FALSE;
    }
    if (!anchored_) {
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }
    const HRESULT hr = audioClient_->Start();
    if (SUCCEEDED(hr)) {
        started_ = true;
    }
    return hr;
}

HRESULT AudioOutput::pause() noexcept {
    const HRESULT state = checkThreadAndInitialized();
    if (FAILED(state)) {
        return state;
    }
    if (!started_) {
        return S_FALSE;
    }
    const HRESULT hr = audioClient_->Stop();
    if (SUCCEEDED(hr)) {
        started_ = false;
    }
    return hr;
}

HRESULT AudioOutput::reset() noexcept {
    const HRESULT state = checkThreadAndInitialized();
    if (FAILED(state)) {
        return state;
    }
    const HRESULT hr = audioClient_->Reset();
    if (SUCCEEDED(hr)) {
        anchored_ = false;
        anchorClockPosition_ = 0;
        anchorPtsNanoseconds_ = 0;
    }
    return hr;
}

HRESULT AudioOutput::setVolume(float scalar) noexcept {
    const HRESULT state = checkThreadAndInitialized();
    if (FAILED(state)) {
        return state;
    }
    if (!std::isfinite(scalar) || scalar < 0.0f || scalar > 1.0f) {
        return E_INVALIDARG;
    }
    return volume_->SetMasterVolume(scalar, nullptr);
}

HRESULT AudioOutput::setMuted(bool muted) noexcept {
    const HRESULT state = checkThreadAndInitialized();
    if (FAILED(state)) {
        return state;
    }
    return volume_->SetMute(muted ? TRUE : FALSE, nullptr);
}

HRESULT AudioOutput::mediaTimeNanoseconds(int64_t* timeNanoseconds) const noexcept {
    if (!timeNanoseconds) {
        return E_POINTER;
    }
    const HRESULT state = checkThreadAndInitialized();
    if (FAILED(state)) {
        return state;
    }
    if (!anchored_) {
        return S_FALSE;
    }

    UINT64 position = 0;
    const HRESULT hr = audioClock_->GetPosition(&position, nullptr);
    if (FAILED(hr)) {
        return hr;
    }
    if (position < anchorClockPosition_) {
        return E_UNEXPECTED;
    }

    const long double elapsedNanoseconds =
        static_cast<long double>(position - anchorClockPosition_) * 1000000000.0L /
        static_cast<long double>(clockFrequency_);
    const long double mediaTime =
        static_cast<long double>(anchorPtsNanoseconds_) + elapsedNanoseconds;
    constexpr long double minimumInt64 = -9223372036854775808.0L;
    constexpr long double onePastMaximumInt64 = 9223372036854775808.0L;
    if (mediaTime < minimumInt64 || mediaTime >= onePastMaximumInt64) {
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    }

    *timeNanoseconds = static_cast<int64_t>(mediaTime);
    return S_OK;
}

HRESULT AudioOutput::checkThreadAndInitialized() const noexcept {
    if (!initialized_) {
        return CO_E_NOTINITIALIZED;
    }
    if (GetCurrentThreadId() != ownerThreadId_) {
        return RPC_E_WRONG_THREAD;
    }
    return S_OK;
}

void AudioOutput::releaseResources() noexcept {
    if (audioClient_ && started_) {
        audioClient_->Stop();
    }
    started_ = false;
    initialized_ = false;
    anchored_ = false;

    volume_.Reset();
    audioClock_.Reset();
    renderClient_.Reset();
    audioClient_.Reset();

    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }

    bufferFrameCapacity_ = 0;
    clockFrequency_ = 0;
    anchorClockPosition_ = 0;
    anchorPtsNanoseconds_ = 0;
    ownerThreadId_ = 0;

    if (uninitializeCom_) {
        CoUninitialize();
        uninitializeCom_ = false;
    }
}

} // namespace odyssey
