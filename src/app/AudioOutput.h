#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>

namespace odyssey {

// Event-driven WASAPI output. Initialize, use, and destroy this object on one
// caller-owned audio thread. The media clock is available only after the first
// PCM write and measures consumed endpoint time; it does not include queued
// samples or compensate for endpoint latency.
class AudioOutput {
public:
    AudioOutput() = default;
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    HRESULT initialize() noexcept;

    const WAVEFORMATEX* mixFormat() const noexcept { return mixFormat_; }
    UINT32 bufferFrameCapacity() const noexcept { return bufferFrameCapacity_; }
    HANDLE eventHandle() const noexcept { return event_; }

    HRESULT availableFrames(UINT32* frames) const noexcept;
    // The first successful write establishes the clock anchor and must precede
    // start(). Subsequent writes retain that anchor until reset().
    HRESULT write(const void* interleavedPcm, size_t byteCount,
                  int64_t firstFramePtsNanoseconds) noexcept;

    HRESULT start() noexcept;
    HRESULT pause() noexcept;
    HRESULT reset() noexcept;

    HRESULT setVolume(float scalar) noexcept;
    HRESULT setMuted(bool muted) noexcept;

    // Returns S_FALSE until write() establishes the first PCM PTS anchor.
    HRESULT mediaTimeNanoseconds(int64_t* timeNanoseconds) const noexcept;

private:
    HRESULT checkThreadAndInitialized() const noexcept;
    void releaseResources() noexcept;

    DWORD ownerThreadId_{0};
    bool uninitializeCom_{false};
    bool initialized_{false};
    bool started_{false};
    bool anchored_{false};

    HANDLE event_{nullptr};
    WAVEFORMATEX* mixFormat_{nullptr};
    UINT32 bufferFrameCapacity_{0};
    UINT64 clockFrequency_{0};
    UINT64 anchorClockPosition_{0};
    int64_t anchorPtsNanoseconds_{0};

    Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient_;
    Microsoft::WRL::ComPtr<IAudioClock> audioClock_;
    Microsoft::WRL::ComPtr<ISimpleAudioVolume> volume_;
};

} // namespace odyssey
