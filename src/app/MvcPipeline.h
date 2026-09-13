#pragma once

#include "SubtitleCue.h"
#include "BluRayTitles.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
struct AVFrame;
}

namespace odyssey {

class MvcPipeline {
public:
    enum class Status {
        Opening,
        Running,
        Paused,
        Drained,
        Stopped,
        Failed,
    };

    enum class OpeningStage {
        Starting,
        MountingIso,
        ScanningTitles,
        LoadingSource,
        ConnectingGraph,
        Ready,
        Failed,
        Stopped,
    };

    struct OpeningDiagnostics {
        OpeningStage stage{OpeningStage::Starting};
        bool isoMounted{false};
        bool isoOwned{false};
    };

    struct Options {
        std::wstring lavDirectory;
        // For ISO inputs this is relative to the mounted volume root. An empty
        // value selects the longest valid playlist, falling back to
        // BDMV\\index.bdmv when no valid playlist is available.
        std::wstring playlistPath;
        bool startPaused{false};
        bool muted{false};
        float volume{1.0f};
        std::size_t frameCapacity{4};
        bool enableAudioDiagnostics{false};
    };

    struct AudioTrack {
        long streamIndex{-1};
        std::wstring name;
        bool selected{false};
    };

    struct SubtitleTrack {
        long streamIndex{-1};
        std::wstring name;
        std::string language;
        std::string codecName;
        bool supported{false};
        bool selected{false};
    };

    struct TimingDiagnostics {
        int64_t segmentStart100ns{0};
        int64_t runReference100ns{0};
        uint64_t segmentCount{0};
        uint64_t runCount{0};
    };

    MvcPipeline(const std::wstring& path, const Options& options);
    ~MvcPipeline();

    MvcPipeline(const MvcPipeline&) = delete;
    MvcPipeline& operator=(const MvcPipeline&) = delete;

    // Returns the next decoded full-SBS NV12 frame. The caller owns the frame
    // and must release it with av_frame_free().
    AVFrame* pollLatest();
    void clear();

    bool pause();
    bool resume();
    bool seekSeconds(double seconds);
    void stop();

    std::vector<AudioTrack> audioTracks() const;
    long selectedAudioTrack() const;
    bool selectAudioTrack(long streamIndex);
    bool setVolume(float scalar);
    void setMuted(bool muted);

    std::vector<SubtitleTrack> subtitleTracks() const;
    std::optional<long> selectedSubtitleTrack() const;
    bool selectSubtitleTrack(long streamIndex);
    void disableSubtitles();
    bool loadExternalSubRip(const std::wstring& path, std::string* error = nullptr);
    bool externalSubtitlesSelected() const;
    bool setSubtitleOffsetSeconds(double seconds);
    std::vector<SubtitleCue> subtitleCuesAt(int64_t mediaNanoseconds) const;
    std::string subtitleError() const;

    BluRayTitleScan titleSnapshot() const;
    std::wstring selectedTitleRelativePlaylistPath() const;
    std::wstring selectedTitleLabel() const;

    double positionSeconds() const;
    double durationSeconds() const;
    int64_t mediaTimeNanoseconds() const;
    uint64_t generation() const;
    Status status() const;
    std::string error() const;
    bool paused() const;
    bool finished() const;
    // Diagnostic used by the playback proof: LAV Audio reported finite,
    // non-silent PCM levels while its output was connected to the clock owner.
    bool audioContentObserved() const;
    TimingDiagnostics timingDiagnostics() const;
    OpeningDiagnostics openingDiagnostics() const;
    int width() const;
    int height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace odyssey
