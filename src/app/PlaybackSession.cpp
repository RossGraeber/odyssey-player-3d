#include "PlaybackSession.h"

#include "MvcPipeline.h"
#include "VideoPipeline.h"

#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace odyssey {
namespace {

bool hasIsoExtension(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    return extension == L".iso";
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return L"Playback failed";
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

} // namespace

PlaybackSession::PlaybackSession(ID3D11Device* device, std::wstring path,
                                 std::wstring lavDirectory, bool startPaused,
                                 bool muted, float volume, std::wstring playlistPath)
    : isMvc_(hasIsoExtension(path)), muted_(muted), volume_(volume),
      pausedRequested_(startPaused) {
    if (!device || path.empty() || !std::isfinite(volume) || volume < 0.0f || volume > 1.0f) {
        throw std::invalid_argument("PlaybackSession input is invalid");
    }
    cancellationEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancellationEvent_) throw std::runtime_error("Playback cancellation event failed");

    try {
        if (isMvc_) {
            MvcPipeline::Options options;
            options.lavDirectory = std::move(lavDirectory);
            options.playlistPath = std::move(playlistPath);
            options.startPaused = startPaused;
            options.muted = muted;
            options.volume = volume;
            mvc_ = std::make_unique<MvcPipeline>(path, options);
        } else {
            VideoPipeline::Options options;
            options.audioEnabled = true;
            options.enableAudioDiagnostics = true;
            options.startPaused = startPaused;
            options.muted = muted;
            options.volume = volume;
            options.cancellationEvent = cancellationEvent_;
            Microsoft::WRL::ComPtr<ID3D11Device> deviceReference(device);
            openingVideo_ = std::async(std::launch::async,
                [deviceReference = std::move(deviceReference), path = std::move(path), options]() mutable {
                    return std::make_unique<VideoPipeline>(deviceReference.Get(), path, options);
                });
        }
    } catch (...) {
        CloseHandle(cancellationEvent_);
        cancellationEvent_ = nullptr;
        throw;
    }
}

PlaybackSession::~PlaybackSession() {
    if (cancellationEvent_) SetEvent(cancellationEvent_);
    if (openingVideo_.valid()) {
        try {
            video_ = openingVideo_.get();
        } catch (...) {
        }
    }
    if (video_) video_->stop();
    if (mvc_) mvc_->stop();
    mvc_.reset();
    video_.reset();
    if (cancellationEvent_) CloseHandle(cancellationEvent_);
}

void PlaybackSession::promoteVideo() {
    if (!openingVideo_.valid() ||
        openingVideo_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    try {
        video_ = openingVideo_.get();
        video_->setMuted(muted_);
        video_->setVolume(volume_);
        video_->setSubtitleOffsetSeconds(subtitleOffsetSeconds_);
        if (pausedRequested_) video_->pause();
        else video_->resume();
    } catch (const std::exception& exception) {
        openingError_ = widen(exception.what());
    } catch (...) {
        openingError_ = L"Playback opening failed";
    }
}

AVFrame* PlaybackSession::pollFrame() {
    promoteVideo();
    return mvc_ ? mvc_->pollLatest() : (video_ ? video_->pollLatest() : nullptr);
}

void PlaybackSession::clearFrames() {
    promoteVideo();
    if (mvc_) mvc_->clear();
    if (video_) video_->clear();
}

bool PlaybackSession::pause() {
    promoteVideo();
    pausedRequested_ = true;
    return mvc_ ? mvc_->pause() : (video_ ? video_->pause() : openingVideo_.valid());
}

bool PlaybackSession::resume() {
    promoteVideo();
    pausedRequested_ = false;
    return mvc_ ? mvc_->resume() : (video_ ? video_->resume() : openingVideo_.valid());
}

bool PlaybackSession::seek(double seconds) {
    promoteVideo();
    return mvc_ ? mvc_->seekSeconds(seconds) : (video_ && video_->seekSeconds(seconds));
}

bool PlaybackSession::selectAudioTrack(int64_t id) {
    promoteVideo();
    if (mvc_) return mvc_->selectAudioTrack(static_cast<long>(id));
    return video_ && video_->selectAudioTrack(static_cast<int>(id));
}

bool PlaybackSession::setVolume(float volume) {
    if (!std::isfinite(volume) || volume < 0.0f || volume > 1.0f) return false;
    promoteVideo();
    const bool accepted = mvc_ ? mvc_->setVolume(volume)
                               : (video_ ? video_->setVolume(volume) : openingVideo_.valid());
    if (accepted) volume_ = volume;
    return accepted;
}

void PlaybackSession::setMuted(bool muted) {
    promoteVideo();
    if (mvc_) mvc_->setMuted(muted);
    if (video_) video_->setMuted(muted);
    muted_ = muted;
}

std::vector<PlaybackSession::SubtitleTrack> PlaybackSession::subtitleTracks() {
    promoteVideo();
    std::vector<SubtitleTrack> result;
    if (mvc_) {
        for (const auto& track : mvc_->subtitleTracks()) {
            result.push_back({track.streamIndex, track.name, track.supported, track.selected});
        }
        return result;
    }
    if (!video_) return result;
    const std::optional<int> selected = video_->selectedSubtitleTrack();
    for (const auto& track : video_->subtitleTracks()) {
        std::wstring label = widen(track.language);
        if (!track.title.empty()) {
            if (!label.empty()) label += L" - ";
            label += widen(track.title);
        }
        if (label.empty()) label = widen(track.codecName);
        if (label.empty()) label = L"Captions " + std::to_wstring(track.streamIndex);
        result.push_back({track.streamIndex, std::move(label), track.supported,
                          selected && *selected == track.streamIndex});
    }
    return result;
}

std::optional<int> PlaybackSession::selectedSubtitleTrack() {
    promoteVideo();
    if (mvc_) {
        const std::optional<long> selected = mvc_->selectedSubtitleTrack();
        return selected ? std::optional<int>(static_cast<int>(*selected)) : std::nullopt;
    }
    return video_ ? video_->selectedSubtitleTrack() : std::nullopt;
}

bool PlaybackSession::selectSubtitleTrack(int id) {
    promoteVideo();
    return mvc_ ? mvc_->selectSubtitleTrack(static_cast<long>(id))
                : (video_ && video_->selectSubtitleTrack(id));
}

void PlaybackSession::disableSubtitles() {
    promoteVideo();
    if (mvc_) mvc_->disableSubtitles();
    if (video_) video_->disableSubtitles();
}

bool PlaybackSession::loadExternalSubRip(const std::wstring& path, std::wstring& error) {
    promoteVideo();
    if (!video_ && !mvc_) {
        error = L"Captions are not available while the movie is opening";
        return false;
    }
    std::string narrowError;
    const bool loaded = mvc_ ? mvc_->loadExternalSubRip(path, &narrowError)
                             : video_->loadExternalSubRip(path, &narrowError);
    error = widen(narrowError);
    return loaded;
}

bool PlaybackSession::setSubtitleOffsetSeconds(double seconds) {
    if (!std::isfinite(seconds) || std::abs(seconds) > 60.0) return false;
    promoteVideo();
    const bool accepted = mvc_ ? mvc_->setSubtitleOffsetSeconds(seconds)
                               : (video_ ? video_->setSubtitleOffsetSeconds(seconds)
                                         : openingVideo_.valid());
    if (accepted) subtitleOffsetSeconds_ = seconds;
    return accepted;
}

BluRayTitleScan PlaybackSession::titleSnapshot() {
    promoteVideo();
    return mvc_ ? mvc_->titleSnapshot() : BluRayTitleScan{};
}

std::wstring PlaybackSession::selectedTitleRelativePlaylistPath() {
    promoteVideo();
    return mvc_ ? mvc_->selectedTitleRelativePlaylistPath() : std::wstring{};
}

std::wstring PlaybackSession::selectedTitleLabel() {
    promoteVideo();
    return mvc_ ? mvc_->selectedTitleLabel() : std::wstring{};
}

bool PlaybackSession::externalSubtitlesSelected() {
    promoteVideo();
    return mvc_ ? mvc_->externalSubtitlesSelected()
                : (video_ && video_->externalSubtitlesSelected());
}

std::vector<SubtitleCue> PlaybackSession::subtitleCuesAt(int64_t mediaNanoseconds) {
    promoteVideo();
    return mvc_ ? mvc_->subtitleCuesAt(mediaNanoseconds)
                : (video_ ? video_->subtitleCuesAt(mediaNanoseconds)
                          : std::vector<SubtitleCue>{});
}

std::wstring PlaybackSession::subtitleError() {
    promoteVideo();
    return mvc_ ? widen(mvc_->subtitleError())
                : (video_ ? widen(video_->subtitleError()) : std::wstring{});
}

VideoPipeline::AudioDiagnostics PlaybackSession::audioDiagnostics() {
    promoteVideo();
    return video_ ? video_->audioDiagnostics() : VideoPipeline::AudioDiagnostics{};
}

PlaybackSession::TimingDiagnostics PlaybackSession::timingDiagnostics() {
    promoteVideo();
    if (!mvc_) return {};
    const MvcPipeline::TimingDiagnostics timing = mvc_->timingDiagnostics();
    return {timing.segmentStart100ns, timing.runReference100ns,
            timing.segmentCount, timing.runCount};
}

PlaybackSession::Status PlaybackSession::status() {
    promoteVideo();
    if (!openingError_.empty()) return Status::Failed;
    if (mvc_) {
        switch (mvc_->status()) {
        case MvcPipeline::Status::Opening: return Status::Opening;
        case MvcPipeline::Status::Running: return Status::Running;
        case MvcPipeline::Status::Paused: return Status::Paused;
        case MvcPipeline::Status::Drained:
        case MvcPipeline::Status::Stopped: return Status::Drained;
        case MvcPipeline::Status::Failed: return Status::Failed;
        }
    }
    if (!video_) return Status::Opening;
    switch (video_->status()) {
    case VideoPipeline::Status::Running: return video_->paused() ? Status::Paused : Status::Running;
    case VideoPipeline::Status::Drained:
    case VideoPipeline::Status::Stopped: return Status::Drained;
    case VideoPipeline::Status::Failed: return Status::Failed;
    }
    return Status::Failed;
}

std::wstring PlaybackSession::error() {
    promoteVideo();
    if (!openingError_.empty()) return openingError_;
    return mvc_ ? widen(mvc_->error()) : (video_ ? widen(video_->error()) : std::wstring{});
}

std::vector<PlaybackSession::AudioTrack> PlaybackSession::audioTracks() {
    promoteVideo();
    std::vector<AudioTrack> result;
    if (mvc_) {
        for (const auto& track : mvc_->audioTracks()) {
            result.push_back({track.streamIndex, track.name, track.selected});
        }
    } else if (video_) {
        const int selected = video_->selectedAudioTrack();
        for (const auto& track : video_->audioTracks()) {
            std::wstring label = widen(track.language);
            if (!track.title.empty()) {
                if (!label.empty()) label += L" - ";
                label += widen(track.title);
            }
            if (label.empty()) label = L"Audio " + std::to_wstring(track.streamIndex);
            result.push_back({track.streamIndex, std::move(label), track.streamIndex == selected});
        }
    }
    return result;
}

int64_t PlaybackSession::selectedAudioTrack() {
    promoteVideo();
    return mvc_ ? mvc_->selectedAudioTrack()
                : (video_ ? video_->selectedAudioTrack() : -1);
}

double PlaybackSession::positionSeconds() {
    promoteVideo();
    return mvc_ ? mvc_->positionSeconds() : (video_ ? video_->positionSeconds() : 0.0);
}

double PlaybackSession::durationSeconds() {
    promoteVideo();
    return mvc_ ? mvc_->durationSeconds() : (video_ ? video_->durationSeconds() : 0.0);
}

int64_t PlaybackSession::mediaTimeNanoseconds() {
    promoteVideo();
    return mvc_ ? mvc_->mediaTimeNanoseconds() : (video_ ? video_->mediaTimeNanoseconds() : 0);
}

uint64_t PlaybackSession::generation() {
    promoteVideo();
    return mvc_ ? mvc_->generation() : (video_ ? video_->generation() : 0);
}

int PlaybackSession::timebaseNum() {
    promoteVideo();
    return mvc_ ? 1 : (video_ ? video_->timebaseNum() : 0);
}

int PlaybackSession::timebaseDen() {
    promoteVideo();
    return mvc_ ? 10'000'000 : (video_ ? video_->timebaseDen() : 0);
}

std::recursive_mutex* PlaybackSession::contextMutex() {
    promoteVideo();
    return video_ ? &video_->contextMutex() : nullptr;
}

} // namespace odyssey
