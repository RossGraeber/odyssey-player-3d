#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "SubtitleCue.h"
#include "BluRayTitles.h"
#include "VideoPipeline.h"

struct AVFrame;

namespace odyssey {

class MvcPipeline;
class VideoPipeline;

class PlaybackSession {
public:
    enum class Status { Opening, Running, Paused, Drained, Failed };

    struct AudioTrack {
        int64_t id{-1};
        std::wstring label;
        bool selected{false};
    };
    struct SubtitleTrack {
        int id{-1};
        std::wstring label;
        bool supported{false};
        bool selected{false};
    };
    struct TimingDiagnostics {
        int64_t segmentStart100ns{0};
        int64_t runReference100ns{0};
        uint64_t segmentCount{0};
        uint64_t runCount{0};
    };

    PlaybackSession(ID3D11Device* device, std::wstring path,
                    std::wstring lavDirectory, bool startPaused,
                    bool muted, float volume,
                    std::wstring playlistPath = {});
    ~PlaybackSession();

    PlaybackSession(const PlaybackSession&) = delete;
    PlaybackSession& operator=(const PlaybackSession&) = delete;

    AVFrame* pollFrame();
    void clearFrames();
    bool pause();
    bool resume();
    bool seek(double seconds);
    bool selectAudioTrack(int64_t id);
    bool setVolume(float volume);
    void setMuted(bool muted);
    std::vector<SubtitleTrack> subtitleTracks();
    std::optional<int> selectedSubtitleTrack();
    bool selectSubtitleTrack(int id);
    void disableSubtitles();
    bool loadExternalSubRip(const std::wstring& path, std::wstring& error);
    bool externalSubtitlesSelected();
    bool setSubtitleOffsetSeconds(double seconds);
    double subtitleOffsetSeconds() const noexcept { return subtitleOffsetSeconds_; }
    BluRayTitleScan titleSnapshot();
    std::wstring selectedTitleRelativePlaylistPath();
    std::wstring selectedTitleLabel();
    std::vector<SubtitleCue> subtitleCuesAt(int64_t mediaNanoseconds);
    std::wstring subtitleError();
    TimingDiagnostics timingDiagnostics();
    VideoPipeline::AudioDiagnostics audioDiagnostics();

    Status status();
    std::wstring error();
    std::vector<AudioTrack> audioTracks();
    int64_t selectedAudioTrack();
    double positionSeconds();
    double durationSeconds();
    int64_t mediaTimeNanoseconds();
    uint64_t generation();
    int timebaseNum();
    int timebaseDen();
    bool isMvc() const noexcept { return isMvc_; }
    bool muted() const noexcept { return muted_; }
    float volume() const noexcept { return volume_; }
    std::recursive_mutex* contextMutex();

private:
    void promoteVideo();

    const bool isMvc_;
    HANDLE cancellationEvent_{nullptr};
    std::future<std::unique_ptr<VideoPipeline>> openingVideo_;
    std::unique_ptr<VideoPipeline> video_;
    std::unique_ptr<MvcPipeline> mvc_;
    std::wstring openingError_;
    bool muted_{false};
    float volume_{1.0f};
    bool pausedRequested_{false};
    double subtitleOffsetSeconds_{0.0};
};

} // namespace odyssey
