#pragma once

#include "SubtitleCue.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <d3d11.h>

extern "C" {
struct AVFrame;
}

namespace odyssey {

class VideoPipeline {
public:
    enum class Status {
        Running,
        Drained,
        Stopped,
        Failed,
    };

    struct Options {
        bool audioEnabled{false};
        bool forceSoftware{false};
        bool enableAudioDiagnostics{false};
        bool startPaused{false};
        bool muted{false};
        float volume{1.0f};
        std::size_t videoFrameCapacity{8};
        std::size_t videoPacketCapacity{64};
        std::size_t audioPacketCapacity{256};
        // The caller keeps this event valid until pipeline destruction. A
        // signaled event interrupts both construction-time and runtime I/O.
        HANDLE cancellationEvent{nullptr};
    };

    struct AudioTrack {
        int streamIndex{-1};
        std::string language;
        std::string title;
        int channels{0};
        int sampleRate{0};
    };

    struct SubtitleTrack {
        int streamIndex{-1};
        std::string language;
        std::string title;
        std::string codecName;
        bool supported{false};
    };

    struct AudioDiagnostics {
        uint64_t contentFramesSubmitted{0};
        uint64_t silenceFramesSubmitted{0};
        uint64_t gapSilenceFramesSubmitted{0};
        uint64_t leadingSilenceFramesSubmitted{0};
        uint64_t trailingSilenceFramesSubmitted{0};
        uint64_t contentFramesDecoded{0};
        uint64_t prerollFramesTrimmed{0};
        uint64_t lateFramesTrimmed{0};
        int64_t maximumSubmissionErrorNs{0};
        double lowestObservedFrequencyHz{0.0};
        double highestObservedFrequencyHz{0.0};
        double currentTrackFrequencyHz{0.0};
        int outputSampleRate{0};
        int64_t currentContentStartNs{0};
        int64_t currentContentEndNs{0};
    };

    VideoPipeline(ID3D11Device* device, const std::wstring& path);
    VideoPipeline(ID3D11Device* device, const std::wstring& path, const Options& options);
    ~VideoPipeline();

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // Returns the next decoded frame in presentation order. The caller owns
    // the frame and must release it with av_frame_free().
    AVFrame* pollLatest();
    void clear();

    bool pause();
    bool resume();
    void stop();
    bool paused() const;
    bool seekSeconds(double seconds);

    std::vector<AudioTrack> audioTracks() const;
    int selectedAudioTrack() const;
    bool selectAudioTrack(int streamIndex);
    bool setVolume(float scalar);
    void setMuted(bool muted);
    bool muted() const;
    float volume() const;
    AudioDiagnostics audioDiagnostics() const;

    std::vector<SubtitleTrack> subtitleTracks() const;
    std::optional<int> selectedSubtitleTrack() const;
    bool selectSubtitleTrack(int streamIndex);
    void disableSubtitles();
    bool loadExternalSubRip(const std::wstring& path, std::string* error = nullptr);
    bool externalSubtitlesSelected() const;
    bool setSubtitleOffsetSeconds(double seconds);
    std::vector<SubtitleCue> subtitleCuesAt(int64_t mediaNanoseconds) const;
    std::string subtitleError() const;

    double positionSeconds() const;
    double durationSeconds() const;
    // Absolute source-timeline time used for direct comparison with decoded
    // frame PTS after applying timebaseNum()/timebaseDen().
    int64_t mediaTimeNanoseconds() const;
    // Changes immediately for every seek or audio-track switch so a host can
    // discard a renderer-owned pending frame before polling the new timeline.
    uint64_t generation() const;

    // True once demux and all enabled decoders have drained or a terminal
    // failure has stopped them. Queued video frames may still be polled.
    bool finished() const;
    Status status() const;
    std::string error() const;

    int width() const;
    int height() const;
    int timebaseNum() const;
    int timebaseDen() const;
    unsigned decodedFrameCount() const;
    unsigned droppedFrameCount() const;

    std::recursive_mutex& contextMutex();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace odyssey
