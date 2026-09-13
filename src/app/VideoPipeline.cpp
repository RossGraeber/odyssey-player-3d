#include "VideoPipeline.h"

#include "AudioOutput.h"
#include "FrameQueue.h"
#include "SubtitleDecoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <ksmedia.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

namespace odyssey {

namespace {

constexpr int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr size_t kMaximumEmbeddedSubtitleCues = 1'024;
constexpr size_t kMaximumEmbeddedSubtitleBytes = 64 * 1024 * 1024;
constexpr int64_t kMaximumSubtitleOffsetNs = 60 * kNanosecondsPerSecond;

struct CodecParametersDeleter {
    void operator()(AVCodecParameters* value) const {
        avcodec_parameters_free(&value);
    }
};

using CodecParametersPtr = std::unique_ptr<AVCodecParameters, CodecParametersDeleter>;

int64_t saturatingAddNanoseconds(int64_t left, int64_t right) {
    if (right > 0 && left > (std::numeric_limits<int64_t>::max)() - right)
        return (std::numeric_limits<int64_t>::max)();
    if (right < 0 && left < (std::numeric_limits<int64_t>::min)() - right)
        return (std::numeric_limits<int64_t>::min)();
    return left + right;
}

size_t subtitleCueBytes(const SubtitleCue& cue) {
    size_t bytes = cue.textUtf8.size();
    for (const SubtitleBitmap& bitmap : cue.bitmaps) {
        if (bitmap.pixels.size() > (std::numeric_limits<size_t>::max)() - bytes)
            return (std::numeric_limits<size_t>::max)();
        bytes += bitmap.pixels.size();
    }
    return bytes;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length == 0) throw std::runtime_error("path conversion to UTF-8 failed");
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                            result.data(), length, nullptr, nullptr) == 0) {
        throw std::runtime_error("path conversion to UTF-8 failed");
    }
    return result;
}

std::string ffmpegError(const char* operation, int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(error, text.data(), text.size());
    return std::string(operation) + ": " + text.data();
}

std::string metadataValue(const AVDictionary* metadata, const char* key) {
    const AVDictionaryEntry* entry = av_dict_get(metadata, key, nullptr, 0);
    return entry && entry->value ? entry->value : "";
}

int findMovieVideoStream(AVFormatContext* format, const AVCodec** decoder) {
    int bestIndex = -1;
    std::tuple<bool, int64_t, int64_t> bestScore{};
    const AVCodec* bestDecoder = nullptr;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        AVStream* stream = format->streams[i];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
            continue;
        }
        const AVCodec* candidate = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!candidate) continue;
        const int64_t pixels = static_cast<int64_t>(stream->codecpar->width) * stream->codecpar->height;
        const auto score = std::make_tuple(
            (stream->disposition & AV_DISPOSITION_DEFAULT) != 0, pixels, stream->codecpar->bit_rate);
        if (bestIndex < 0 || score > bestScore) {
            bestIndex = static_cast<int>(i);
            bestScore = score;
            bestDecoder = candidate;
        }
    }
    if (decoder) *decoder = bestDecoder;
    return bestIndex;
}

struct PacketItem {
    AVPacket* packet{nullptr};
    uint64_t generation{0};
    bool eof{false};

    PacketItem() = default;
    PacketItem(AVPacket* value, uint64_t gen, bool atEof = false)
        : packet(value), generation(gen), eof(atEof) {}
    ~PacketItem() { av_packet_free(&packet); }
    PacketItem(const PacketItem&) = delete;
    PacketItem& operator=(const PacketItem&) = delete;
    PacketItem(PacketItem&& other) noexcept
        : packet(std::exchange(other.packet, nullptr)), generation(other.generation), eof(other.eof) {}
    PacketItem& operator=(PacketItem&& other) noexcept {
        if (this != &other) {
            av_packet_free(&packet);
            packet = std::exchange(other.packet, nullptr);
            generation = other.generation;
            eof = other.eof;
        }
        return *this;
    }
};

class PacketQueue {
public:
    explicit PacketQueue(size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("packet queue capacity must be nonzero");
    }

    bool tryPush(PacketItem& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || items_.size() >= capacity_) return false;
        items_.push_back(std::move(item));
        changed_.notify_all();
        return true;
    }

    bool takeFor(PacketItem& item, const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(10),
                          [this, &stop] { return closed_ || stop.load() || !items_.empty(); });
        if (items_.empty()) return false;
        item = std::move(items_.front());
        items_.pop_front();
        changed_.notify_all();
        return true;
    }

    bool tryTake(PacketItem& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) return false;
        item = std::move(items_.front());
        items_.pop_front();
        changed_.notify_all();
        return true;
    }

    void clear() {
        std::deque<PacketItem> removed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            removed.swap(items_);
        }
        changed_.notify_all();
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        changed_.notify_all();
    }

    void wake() { changed_.notify_all(); }

private:
    size_t capacity_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<PacketItem> items_;
    bool closed_{false};
};

struct QueuedVideoFrame {
    AVFrame* frame{nullptr};
    uint64_t generation{0};
};

void deleteQueuedVideoFrame(QueuedVideoFrame* queued) {
    if (!queued) return;
    av_frame_free(&queued->frame);
    delete queued;
}

void d3dCtxLock(void* value) { static_cast<std::recursive_mutex*>(value)->lock(); }
void d3dCtxUnlock(void* value) { static_cast<std::recursive_mutex*>(value)->unlock(); }

struct HwPickerContext {
    AVBufferRef* device{nullptr};
    AVPixelFormat softwareFormat{AV_PIX_FMT_NV12};
};

bool setupHwFrames(AVCodecContext* codec, const HwPickerContext& picker) {
    if (codec->hw_frames_ctx) return true;
    AVBufferRef* frames = av_hwframe_ctx_alloc(picker.device);
    if (!frames) return false;
    auto* context = reinterpret_cast<AVHWFramesContext*>(frames->data);
    context->format = AV_PIX_FMT_D3D11;
    context->sw_format = picker.softwareFormat;
    context->width = codec->coded_width;
    context->height = codec->coded_height;
    context->initial_pool_size = 20;
    auto* d3d = reinterpret_cast<AVD3D11VAFramesContext*>(context->hwctx);
    d3d->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    if (av_hwframe_ctx_init(frames) < 0) {
        av_buffer_unref(&frames);
        return false;
    }
    codec->hw_frames_ctx = frames;
    return true;
}

AVPixelFormat pickVideoFormat(AVCodecContext* codec, const AVPixelFormat* formats) {
    auto* picker = static_cast<HwPickerContext*>(codec->opaque);
    if (picker && picker->device) {
        for (int i = 0; formats[i] != AV_PIX_FMT_NONE; ++i) {
            if (formats[i] == AV_PIX_FMT_D3D11 && setupHwFrames(codec, *picker)) {
                return AV_PIX_FMT_D3D11;
            }
        }
    }
    for (int i = 0; formats[i] != AV_PIX_FMT_NONE; ++i) {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(formats[i]);
        if (formats[i] != AV_PIX_FMT_D3D11 && descriptor &&
            (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
            return formats[i];
        }
    }
    return AV_PIX_FMT_NONE;
}

AVPixelFormat sourceHardwareFormat(const AVCodecParameters* parameters) {
    const auto format = static_cast<AVPixelFormat>(parameters->format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    if (!descriptor || descriptor->log2_chroma_w != 1 || descriptor->log2_chroma_h != 1) {
        return AV_PIX_FMT_NONE;
    }
    return descriptor->comp[0].depth > 8 ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
}

int64_t timestampNanoseconds(int64_t timestamp, AVRational timeBase) {
    if (timestamp == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    return av_rescale_q(timestamp, timeBase, AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
}

} // namespace

struct VideoPipeline::Impl {
    struct AudioInfo {
        AudioTrack publicInfo;
        CodecParametersPtr parameters;
        AVRational timeBase{1, 1};
        int64_t startPtsNs{AV_NOPTS_VALUE};
        int64_t endPtsNs{AV_NOPTS_VALUE};
    };

    struct SubtitleInfo {
        SubtitleTrack publicInfo;
        CodecParametersPtr parameters;
        AVRational timeBase{1, 1};
    };

    ID3D11Device* device{nullptr};
    Options options;
    AVFormatContext* format{nullptr};
    AVCodecContext* videoCodec{nullptr};
    AVBufferRef* hardwareDevice{nullptr};
    HwPickerContext picker;
    int videoStream{-1};
    int videoWidth{0};
    int videoHeight{0};
    int videoTimeBaseNum{1};
    int videoTimeBaseDen{90'000};
    int64_t timelineStartNs{0};
    int64_t durationNs{0};

    std::vector<AudioInfo> audioInfo;
    std::atomic<int> selectedAudioStream{-1};
    bool usesAudioClock{false};
    std::vector<SubtitleInfo> subtitleInfo;
    std::atomic<int> selectedSubtitleStream{-1};
    std::atomic<bool> externalSubtitleSelected{false};
    std::atomic<int64_t> subtitleOffsetNs{0};
    SubtitleDecoder subtitleDecoder;
    mutable std::mutex subtitleMutex;
    std::vector<SubtitleCue> embeddedSubtitleCues;
    std::vector<SubtitleCue> externalSubtitleCues;
    mutable std::mutex subtitleErrorMutex;
    std::string subtitleErrorText;

    FrameQueue<QueuedVideoFrame> videoFrames;
    PacketQueue videoPackets;
    PacketQueue audioPackets;
    std::recursive_mutex contextMutex;

    std::thread demuxThread;
    std::thread videoThread;
    std::thread audioThread;
    std::atomic<bool> stop{false};
    std::atomic<bool> shutdownStarted{false};
    std::atomic<bool> done{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> pauseApplied{false};
    std::atomic<Status> status{Status::Running};
    std::atomic<bool> failed{false};
    std::atomic<unsigned> decodedFrames{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<int64_t> generationTargetNs{0};
    std::atomic<uint64_t> demuxGeneration{0};
    std::atomic<bool> demuxDrained{false};
    std::atomic<bool> videoDrained{false};
    std::atomic<bool> audioDrained{false};
    std::atomic<int64_t> audioClockNs{0};
    std::atomic<float> desiredVolume{1.0f};
    std::atomic<bool> desiredMuted{false};
    std::atomic<uint64_t> audioContentFrames{0};
    std::atomic<uint64_t> audioSilenceFrames{0};
    std::atomic<uint64_t> audioGapSilenceFrames{0};
    std::atomic<uint64_t> audioLeadingSilenceFrames{0};
    std::atomic<uint64_t> audioTrailingSilenceFrames{0};
    std::atomic<uint64_t> audioDecodedFrames{0};
    std::atomic<uint64_t> audioPrerollTrimmedFrames{0};
    std::atomic<uint64_t> audioLateTrimmedFrames{0};
    std::atomic<int64_t> maximumAudioSubmissionErrorNs{0};
    std::atomic<double> lowestAudioFrequencyHz{0.0};
    std::atomic<double> highestAudioFrequencyHz{0.0};
    std::atomic<int> currentAudioSampleRate{0};
    std::atomic<uint64_t> currentFrequencyMilliHzFrames{0};
    std::atomic<uint64_t> currentFrequencyFrames{0};
    std::atomic<int64_t> currentContentStartNs{AV_NOPTS_VALUE};
    std::atomic<int64_t> currentContentEndNs{AV_NOPTS_VALUE};

    mutable std::mutex errorMutex;
    std::string errorText;
    mutable std::mutex commandMutex;
    std::condition_variable commandChanged;
    bool seekPending{false};
    int64_t seekTargetNs{0};

    mutable std::mutex fallbackClockMutex;
    std::chrono::steady_clock::time_point fallbackAnchorWall;
    int64_t fallbackAnchorMediaNs{0};

    Impl(ID3D11Device* suppliedDevice, const std::wstring& path, const Options& suppliedOptions)
        : device(suppliedDevice), options(suppliedOptions),
          videoFrames(suppliedOptions.videoFrameCapacity, deleteQueuedVideoFrame),
          videoPackets(suppliedOptions.videoPacketCapacity),
          audioPackets(suppliedOptions.audioPacketCapacity) {
        try {
            initialize(path);
            startThreads();
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~Impl() { shutdown(); }

    void initialize(const std::wstring& path) {
        if (!device) throw std::invalid_argument("VideoPipeline requires a D3D11 device");
        if (!std::isfinite(options.volume) || options.volume < 0.0f || options.volume > 1.0f) {
            throw std::invalid_argument("volume must be between zero and one");
        }
        if (externalCanceled()) throw std::runtime_error("media open canceled");

        format = avformat_alloc_context();
        if (!format) throw std::runtime_error("avformat_alloc_context failed");
        format->interrupt_callback.callback = interruptCallback;
        format->interrupt_callback.opaque = this;
        const std::string utf8Path = wideToUtf8(path);
        int result = avformat_open_input(&format, utf8Path.c_str(), nullptr, nullptr);
        if (externalCanceled()) throw std::runtime_error("media open canceled");
        if (result < 0) throw std::runtime_error(ffmpegError("avformat_open_input", result));
        result = avformat_find_stream_info(format, nullptr);
        if (externalCanceled()) throw std::runtime_error("media open canceled");
        if (result < 0) throw std::runtime_error(ffmpegError("avformat_find_stream_info", result));

        const AVCodec* decoder = nullptr;
        videoStream = findMovieVideoStream(format, &decoder);
        if (videoStream < 0 || !decoder) throw std::runtime_error("no playable movie video stream");

        AVStream* stream = format->streams[videoStream];
        videoTimeBaseNum = stream->time_base.num;
        videoTimeBaseDen = stream->time_base.den;
        videoCodec = avcodec_alloc_context3(decoder);
        if (!videoCodec) throw std::runtime_error("avcodec_alloc_context3(video) failed");
        result = avcodec_parameters_to_context(videoCodec, stream->codecpar);
        if (result < 0) throw std::runtime_error(ffmpegError("avcodec_parameters_to_context(video)", result));
        videoCodec->pkt_timebase = stream->time_base;

        if (!options.forceSoftware && sourceHardwareFormat(stream->codecpar) != AV_PIX_FMT_NONE) {
            initializeHardwareDevice(stream->codecpar);
        }
        videoCodec->get_format = pickVideoFormat;
        videoCodec->opaque = &picker;
        videoCodec->thread_count = hardwareDevice ? 1 : 0;
        result = avcodec_open2(videoCodec, decoder, nullptr);
        if (result < 0 && hardwareDevice) {
            avcodec_free_context(&videoCodec);
            av_buffer_unref(&hardwareDevice);
            picker.device = nullptr;
            videoCodec = avcodec_alloc_context3(decoder);
            if (!videoCodec) throw std::runtime_error("avcodec_alloc_context3(video fallback) failed");
            result = avcodec_parameters_to_context(videoCodec, stream->codecpar);
            videoCodec->pkt_timebase = stream->time_base;
            if (result >= 0) result = avcodec_open2(videoCodec, decoder, nullptr);
        }
        if (result < 0) throw std::runtime_error(ffmpegError("avcodec_open2(video)", result));
        videoWidth = videoCodec->width;
        videoHeight = videoCodec->height;

        timelineStartNs = format->start_time == AV_NOPTS_VALUE
            ? 0 : av_rescale_q(format->start_time, AV_TIME_BASE_Q,
                               AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
        durationNs = format->duration == AV_NOPTS_VALUE
            ? 0 : av_rescale_q(format->duration, AV_TIME_BASE_Q,
                               AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
        fallbackAnchorMediaNs = timelineStartNs;
        fallbackAnchorWall = std::chrono::steady_clock::now();
        audioClockNs.store(timelineStartNs);
        generationTargetNs.store(timelineStartNs);
        paused.store(options.startPaused);
        pauseApplied.store(options.startPaused || !options.audioEnabled);
        desiredVolume.store(options.volume);
        desiredMuted.store(options.muted);

        enumerateAudio();
        enumerateSubtitles();
        if (options.audioEnabled && !audioInfo.empty()) {
            int selected = audioInfo.front().publicInfo.streamIndex;
            for (const AudioInfo& info : audioInfo) {
                if ((format->streams[info.publicInfo.streamIndex]->disposition &
                     AV_DISPOSITION_DEFAULT) != 0) {
                    selected = info.publicInfo.streamIndex;
                    break;
                }
            }
            selectedAudioStream.store(selected);
            usesAudioClock = true;
        }
    }

    void initializeHardwareDevice(const AVCodecParameters* parameters) {
        hardwareDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hardwareDevice) return;
        auto* hardware = reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data);
        auto* d3d = reinterpret_cast<AVD3D11VADeviceContext*>(hardware->hwctx);
        d3d->device = device;
        device->AddRef();
        d3d->lock = d3dCtxLock;
        d3d->unlock = d3dCtxUnlock;
        d3d->lock_ctx = &contextMutex;
        const int result = av_hwdevice_ctx_init(hardwareDevice);
        if (result < 0) {
            av_buffer_unref(&hardwareDevice);
            return;
        }
        picker.device = hardwareDevice;
        picker.softwareFormat = sourceHardwareFormat(parameters);
        videoCodec->hw_device_ctx = av_buffer_ref(hardwareDevice);
        if (!videoCodec->hw_device_ctx) {
            av_buffer_unref(&hardwareDevice);
            picker.device = nullptr;
        }
    }

    void enumerateAudio() {
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            AVStream* stream = format->streams[i];
            if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
                !avcodec_find_decoder(stream->codecpar->codec_id)) {
                continue;
            }
            AudioInfo info;
            info.publicInfo.streamIndex = static_cast<int>(i);
            info.publicInfo.language = metadataValue(stream->metadata, "language");
            info.publicInfo.title = metadataValue(stream->metadata, "title");
            info.publicInfo.channels = stream->codecpar->ch_layout.nb_channels;
            info.publicInfo.sampleRate = stream->codecpar->sample_rate;
            info.parameters.reset(avcodec_parameters_alloc());
            if (!info.parameters) throw std::runtime_error("avcodec_parameters_alloc(audio) failed");
            const int result = avcodec_parameters_copy(info.parameters.get(), stream->codecpar);
            if (result < 0) {
                throw std::runtime_error(ffmpegError("avcodec_parameters_copy(audio)", result));
            }
            info.timeBase = stream->time_base;
            if (stream->start_time != AV_NOPTS_VALUE) {
                info.startPtsNs = timestampNanoseconds(stream->start_time, stream->time_base);
            }
            if (stream->start_time != AV_NOPTS_VALUE && stream->duration != AV_NOPTS_VALUE) {
                info.endPtsNs = timestampNanoseconds(
                    stream->start_time + stream->duration, stream->time_base);
            }
            audioInfo.push_back(std::move(info));
        }
    }

    void enumerateSubtitles() {
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            AVStream* stream = format->streams[i];
            if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
            SubtitleInfo info;
            info.publicInfo.streamIndex = static_cast<int>(i);
            info.publicInfo.language = metadataValue(stream->metadata, "language");
            info.publicInfo.title = metadataValue(stream->metadata, "title");
            const AVCodecDescriptor* descriptor = avcodec_descriptor_get(stream->codecpar->codec_id);
            info.publicInfo.codecName = descriptor && descriptor->name
                ? descriptor->name : avcodec_get_name(stream->codecpar->codec_id);
            info.publicInfo.supported =
                (stream->codecpar->codec_id == AV_CODEC_ID_SUBRIP ||
                 stream->codecpar->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) &&
                avcodec_find_decoder(stream->codecpar->codec_id);
            info.parameters.reset(avcodec_parameters_alloc());
            if (!info.parameters) throw std::runtime_error("avcodec_parameters_alloc(subtitle) failed");
            const int result = avcodec_parameters_copy(info.parameters.get(), stream->codecpar);
            if (result < 0)
                throw std::runtime_error(ffmpegError("avcodec_parameters_copy(subtitle)", result));
            info.timeBase = stream->time_base;
            subtitleInfo.push_back(std::move(info));
        }
    }

    void startThreads() {
        videoThread = std::thread(&Impl::videoThreadEntry, this);
        if (usesAudioClock) audioThread = std::thread(&Impl::audioThreadEntry, this);
        demuxThread = std::thread(&Impl::demuxThreadEntry, this);
    }

    void shutdown() {
        if (shutdownStarted.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            stop.store(true);
            if (!failed.load()) status.store(Status::Stopped);
        }
        videoPackets.close();
        audioPackets.close();
        videoFrames.close();
        commandChanged.notify_all();
        if (demuxThread.joinable()) demuxThread.join();
        if (videoThread.joinable()) videoThread.join();
        if (audioThread.joinable()) audioThread.join();
        videoFrames.clear();
        if (videoCodec) avcodec_free_context(&videoCodec);
        if (hardwareDevice) av_buffer_unref(&hardwareDevice);
        if (format) avformat_close_input(&format);
        done.store(true);
    }

    static int interruptCallback(void* opaque) {
        const auto* self = static_cast<const Impl*>(opaque);
        return self->stop.load() || self->externalCanceled() ||
               self->generation.load() != self->demuxGeneration.load();
    }

    bool externalCanceled() const {
        return options.cancellationEvent &&
               WaitForSingleObject(options.cancellationEvent, 0) == WAIT_OBJECT_0;
    }

    void finishExternalCancellation() {
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            if (failed.load() || stop.load()) return;
            stop.store(true);
            status.store(Status::Stopped);
            done.store(true);
        }
        videoPackets.close();
        audioPackets.close();
        videoFrames.close();
        commandChanged.notify_all();
    }

    void fail(std::string message) {
        {
            std::lock_guard<std::mutex> commandLock(commandMutex);
            if (failed.load() || stop.load()) return;
            failed.store(true);
            {
                std::lock_guard<std::mutex> errorLock(errorMutex);
                errorText = std::move(message);
            }
            status.store(Status::Failed);
            done.store(true);
            stop.store(true);
        }
        videoPackets.close();
        audioPackets.close();
        videoFrames.close();
        commandChanged.notify_all();
    }

    void updateDoneLocked(uint64_t completedGeneration) {
        const bool audioDone = !usesAudioClock || audioDrained.load();
        if (!failed.load() && generation.load() == completedGeneration &&
            demuxDrained.load() && videoDrained.load() && audioDone) {
            status.store(Status::Drained);
            done.store(true);
        }
    }

    void markDemuxDrained(uint64_t completedGeneration) {
        std::lock_guard<std::mutex> lock(commandMutex);
        if (generation.load() != completedGeneration || failed.load()) return;
        demuxDrained.store(true);
        updateDoneLocked(completedGeneration);
    }

    void markVideoDrained(uint64_t completedGeneration) {
        std::lock_guard<std::mutex> lock(commandMutex);
        if (generation.load() != completedGeneration || failed.load()) return;
        videoDrained.store(true);
        updateDoneLocked(completedGeneration);
    }

    void markAudioDrained(uint64_t completedGeneration) {
        std::lock_guard<std::mutex> lock(commandMutex);
        if (generation.load() != completedGeneration || failed.load()) return;
        audioDrained.store(true);
        updateDoneLocked(completedGeneration);
    }

    void workerException(const char* worker, const char* detail) noexcept {
        try {
            fail(std::string(worker) + " worker exception: " + detail);
        } catch (...) {
            failed.store(true);
            status.store(Status::Failed);
            done.store(true);
            stop.store(true);
            videoPackets.close();
            audioPackets.close();
            videoFrames.close();
            commandChanged.notify_all();
        }
    }

    void demuxThreadEntry() noexcept {
        try {
            demuxLoop();
        } catch (const std::exception& error) {
            workerException("demux", error.what());
        } catch (...) {
            workerException("demux", "unknown failure");
        }
    }

    void videoThreadEntry() noexcept {
        try {
            videoLoop();
        } catch (const std::exception& error) {
            workerException("video", error.what());
        } catch (...) {
            workerException("video", "unknown failure");
        }
    }

    void audioThreadEntry() noexcept {
        try {
            audioLoop();
        } catch (const std::exception& error) {
            workerException("audio", error.what());
        } catch (...) {
            workerException("audio", "unknown failure");
        }
    }

    int64_t fallbackClockNow() const {
        std::lock_guard<std::mutex> lock(fallbackClockMutex);
        if (paused.load()) return fallbackAnchorMediaNs;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - fallbackAnchorWall).count();
        return fallbackAnchorMediaNs + elapsed;
    }

    void resetFallbackClock(int64_t mediaNs) {
        std::lock_guard<std::mutex> lock(fallbackClockMutex);
        fallbackAnchorMediaNs = mediaNs;
        fallbackAnchorWall = std::chrono::steady_clock::now();
        if (!usesAudioClock) audioClockNs.store(mediaNs);
    }

    const SubtitleInfo* findSubtitleInfo(int streamIndex) const {
        const auto found = std::find_if(subtitleInfo.begin(), subtitleInfo.end(),
            [streamIndex](const SubtitleInfo& info) {
                return info.publicInfo.streamIndex == streamIndex;
            });
        return found == subtitleInfo.end() ? nullptr : &*found;
    }

    void clearEmbeddedSubtitleCues() {
        std::lock_guard<std::mutex> lock(subtitleMutex);
        embeddedSubtitleCues.clear();
    }

    void disableEmbeddedSubtitlesWithError(std::string message,
                                           uint64_t expectedGeneration,
                                           int expectedStream) {
        subtitleDecoder.close();
        std::lock_guard<std::mutex> commandLock(commandMutex);
        if (generation.load() != expectedGeneration ||
            selectedSubtitleStream.load() != expectedStream ||
            externalSubtitleSelected.load() || failed.load() || stop.load()) {
            return;
        }
        selectedSubtitleStream.store(-1);
        std::lock_guard<std::mutex> subtitleLock(subtitleMutex);
        embeddedSubtitleCues.clear();
        std::lock_guard<std::mutex> errorLock(subtitleErrorMutex);
        subtitleErrorText = std::move(message);
    }

    bool resetSubtitleDecoder(uint64_t targetGeneration) {
        subtitleDecoder.close();
        clearEmbeddedSubtitleCues();
        if (generation.load() != targetGeneration || externalSubtitleSelected.load()) return true;
        const int streamIndex = selectedSubtitleStream.load();
        if (streamIndex < 0) return true;
        const SubtitleInfo* info = findSubtitleInfo(streamIndex);
        if (!info || !info->publicInfo.supported) {
            disableEmbeddedSubtitlesWithError(
                "selected subtitle stream is unavailable", targetGeneration, streamIndex);
            return true;
        }
        std::string decoderError;
        if (!subtitleDecoder.open(info->parameters.get(), info->timeBase.num,
                                  info->timeBase.den, videoWidth, videoHeight,
                                  decoderError)) {
            disableEmbeddedSubtitlesWithError(
                std::move(decoderError), targetGeneration, streamIndex);
            return true;
        }
        return true;
    }

    bool decodeSubtitlePacket(const AVPacket* packet, uint64_t packetGeneration,
                              int streamIndex) {
        SubtitleDecodeOutput output;
        std::string decoderError;
        if (!subtitleDecoder.decode(packet, output, decoderError)) {
            disableEmbeddedSubtitlesWithError(
                std::move(decoderError), packetGeneration, streamIndex);
            return true;
        }

        bool overflow = false;
        {
            std::lock_guard<std::mutex> commandLock(commandMutex);
            if (generation.load() != packetGeneration ||
                selectedSubtitleStream.load() != streamIndex ||
                externalSubtitleSelected.load() || failed.load() || stop.load()) {
                return true;
            }
            std::lock_guard<std::mutex> subtitleLock(subtitleMutex);
            if (output.clearAtNanoseconds) {
                for (SubtitleCue& cue : embeddedSubtitleCues) {
                    if (!cue.endNanoseconds &&
                        cue.startNanoseconds <= *output.clearAtNanoseconds) {
                        cue.endNanoseconds = *output.clearAtNanoseconds;
                    }
                }
            }
            const int64_t mediaNs = usesAudioClock ? audioClockNs.load() : fallbackClockNow();
            const int64_t safeExpirationNs = saturatingAddNanoseconds(
                mediaNs, -kMaximumSubtitleOffsetNs);
            embeddedSubtitleCues.erase(
                std::remove_if(embeddedSubtitleCues.begin(), embeddedSubtitleCues.end(),
                    [safeExpirationNs](const SubtitleCue& cue) {
                        return cue.endNanoseconds &&
                               *cue.endNanoseconds <= safeExpirationNs;
                    }),
                embeddedSubtitleCues.end());
            size_t bufferedBytes = 0;
            bool byteOverflow = false;
            for (const SubtitleCue& cue : embeddedSubtitleCues) {
                const size_t cueBytes = subtitleCueBytes(cue);
                if (cueBytes > kMaximumEmbeddedSubtitleBytes - bufferedBytes) {
                    byteOverflow = true;
                    break;
                }
                bufferedBytes += cueBytes;
            }
            size_t addedBytes = 0;
            for (const SubtitleCue& cue : output.cues) {
                const size_t cueBytes = subtitleCueBytes(cue);
                if (cueBytes > kMaximumEmbeddedSubtitleBytes - addedBytes) {
                    byteOverflow = true;
                    break;
                }
                addedBytes += cueBytes;
            }
            if (output.cues.size() >
                    kMaximumEmbeddedSubtitleCues - embeddedSubtitleCues.size() ||
                byteOverflow ||
                addedBytes > kMaximumEmbeddedSubtitleBytes - bufferedBytes) {
                overflow = true;
            } else {
                embeddedSubtitleCues.insert(
                    embeddedSubtitleCues.end(),
                    std::make_move_iterator(output.cues.begin()),
                    std::make_move_iterator(output.cues.end()));
            }
        }
        if (overflow) {
            disableEmbeddedSubtitlesWithError(
                "embedded subtitle cue buffer capacity exceeded",
                packetGeneration, streamIndex);
        }
        return true;
    }

    bool requestSeek(int64_t relativeNs, std::optional<int> newAudioStream = std::nullopt,
                     std::optional<int> newSubtitleStream = std::nullopt) {
        if (relativeNs < 0 || (durationNs > 0 && relativeNs > durationNs)) return false;
        const int64_t absoluteNs = timelineStartNs + relativeNs;
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            if (failed.load() || stop.load()) return false;
            if (newAudioStream) selectedAudioStream.store(*newAudioStream);
            if (newSubtitleStream) {
                selectedSubtitleStream.store(*newSubtitleStream);
                externalSubtitleSelected.store(false);
                std::lock_guard<std::mutex> errorLock(subtitleErrorMutex);
                subtitleErrorText.clear();
            }
            seekTargetNs = absoluteNs;
            generationTargetNs.store(absoluteNs);
            seekPending = true;
            generation.fetch_add(1);
            done.store(false);
            demuxDrained.store(false);
            videoDrained.store(false);
            audioDrained.store(false);
            status.store(Status::Running);
            videoFrames.clear();
            videoPackets.clear();
            audioPackets.clear();
            clearEmbeddedSubtitleCues();
            resetFallbackClock(absoluteNs);
            audioClockNs.store(absoluteNs);
        }
        videoPackets.wake();
        audioPackets.wake();
        commandChanged.notify_all();
        return true;
    }

    bool pushPacket(PacketQueue& queue, PacketItem& item, uint64_t itemGeneration) {
        while (!stop.load() && generation.load() == itemGeneration) {
            if (externalCanceled()) {
                finishExternalCancellation();
                return false;
            }
            if (queue.tryPush(item)) return true;
            std::unique_lock<std::mutex> lock(commandMutex);
            commandChanged.wait_for(lock, std::chrono::milliseconds(2));
        }
        return false;
    }

    bool processSeek() {
        int64_t targetNs = 0;
        uint64_t targetGeneration = 0;
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            if (!seekPending) return false;
            targetNs = seekTargetNs;
            targetGeneration = generation.load();
            seekPending = false;
        }

        videoPackets.clear();
        audioPackets.clear();
        demuxGeneration.store(targetGeneration);
        const int64_t targetUs = av_rescale_q(
            targetNs, AVRational{1, static_cast<int>(kNanosecondsPerSecond)}, AV_TIME_BASE_Q);
        const int result = avformat_seek_file(
            format, -1, (std::numeric_limits<int64_t>::min)(), targetUs,
            (std::numeric_limits<int64_t>::max)(), AVSEEK_FLAG_BACKWARD);
        if (result < 0) {
            if (externalCanceled()) {
                finishExternalCancellation();
                return true;
            }
            if (generation.load() != targetGeneration) return true;
            fail(ffmpegError("avformat_seek_file", result));
            return true;
        }
        avformat_flush(format);
        if (generation.load() != targetGeneration) return true;
        if (!resetSubtitleDecoder(targetGeneration)) return true;
        demuxDrained.store(false);
        return true;
    }

    void demuxLoop() {
        while (!stop.load()) {
            if (externalCanceled()) {
                finishExternalCancellation();
                break;
            }
            if (processSeek()) continue;
            if (demuxDrained.load()) {
                std::unique_lock<std::mutex> lock(commandMutex);
                commandChanged.wait_for(lock, std::chrono::milliseconds(10));
                continue;
            }

            AVPacket* packet = av_packet_alloc();
            if (!packet) {
                fail("av_packet_alloc failed");
                break;
            }
            const int readResult = av_read_frame(format, packet);
            if (readResult == AVERROR_EOF) {
                av_packet_free(&packet);
                const uint64_t current = demuxGeneration.load();
                PacketItem videoEof(nullptr, current, true);
                if (!pushPacket(videoPackets, videoEof, current)) continue;
                if (usesAudioClock) {
                    PacketItem audioEof(nullptr, current, true);
                    if (!pushPacket(audioPackets, audioEof, current)) continue;
                }
                if (generation.load() != current) continue;
                markDemuxDrained(current);
                continue;
            }
            if (readResult < 0) {
                av_packet_free(&packet);
                if (stop.load()) break;
                if (externalCanceled()) {
                    finishExternalCancellation();
                    break;
                }
                if (generation.load() != demuxGeneration.load()) continue;
                fail(ffmpegError("av_read_frame", readResult));
                break;
            }

            const uint64_t current = demuxGeneration.load();
            PacketItem item(packet, current);
            if (packet->stream_index == videoStream) {
                pushPacket(videoPackets, item, current);
            } else if (usesAudioClock && packet->stream_index == selectedAudioStream.load()) {
                pushPacket(audioPackets, item, current);
            } else if (!externalSubtitleSelected.load() &&
                       packet->stream_index == selectedSubtitleStream.load()) {
                if (!decodeSubtitlePacket(item.packet, current, packet->stream_index)) break;
            }
        }
    }

    int receiveVideoFrames(uint64_t localGeneration, bool draining) {
        while (!stop.load() && generation.load() == localGeneration) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                fail("av_frame_alloc(video) failed");
                return -1;
            }
            const int result = avcodec_receive_frame(videoCodec, frame);
            if (result == 0) {
                auto* queued = new (std::nothrow) QueuedVideoFrame{frame, localGeneration};
                if (!queued) {
                    av_frame_free(&frame);
                    fail("video frame queue allocation failed");
                    return -1;
                }
                ++decodedFrames;
                if (!videoFrames.publish(queued)) return 2;
                continue;
            }
            av_frame_free(&frame);
            if (result == AVERROR_EOF) return 1;
            if (result == AVERROR(EAGAIN)) {
                if (draining) {
                    fail("video decoder requested input after drain started");
                    return -1;
                }
                return 0;
            }
            fail(ffmpegError("avcodec_receive_frame(video)", result));
            return -1;
        }
        return 2;
    }

    bool sendVideoPacket(AVPacket* packet, uint64_t localGeneration) {
        while (!stop.load() && generation.load() == localGeneration) {
            const int result = avcodec_send_packet(videoCodec, packet);
            if (result == AVERROR_EOF && packet == nullptr) return true;
            if (result == AVERROR(EAGAIN)) {
                const int receiveResult = receiveVideoFrames(localGeneration, false);
                if (receiveResult == 1 && packet == nullptr) return true;
                if (receiveResult != 0) return false;
                continue;
            }
            if (result < 0) {
                fail(ffmpegError("avcodec_send_packet(video)", result));
                return false;
            }
            const int receiveResult = receiveVideoFrames(localGeneration, packet == nullptr);
            if (receiveResult == 1 && packet != nullptr) {
                fail("video decoder reached EOF before demuxer EOF");
                return false;
            }
            return receiveResult >= 0;
        }
        return false;
    }

    void videoLoop() {
        uint64_t localGeneration = generation.load();
        while (!stop.load()) {
            const uint64_t requestedGeneration = generation.load();
            if (requestedGeneration != localGeneration) {
                avcodec_flush_buffers(videoCodec);
                videoFrames.clear();
                localGeneration = requestedGeneration;
                videoDrained.store(false);
            }

            PacketItem item;
            if (!videoPackets.takeFor(item, stop)) continue;
            commandChanged.notify_all();
            const uint64_t afterTakeGeneration = generation.load();
            if (afterTakeGeneration != localGeneration) {
                avcodec_flush_buffers(videoCodec);
                videoFrames.clear();
                localGeneration = afterTakeGeneration;
                videoDrained.store(false);
            }
            if (item.generation != localGeneration) continue;
            if (item.eof) {
                if (sendVideoPacket(nullptr, localGeneration) && !failed.load() &&
                    generation.load() == localGeneration) {
                    markVideoDrained(localGeneration);
                }
                continue;
            }
            sendVideoPacket(item.packet, localGeneration);
        }
    }

    struct AudioWorkerState {
        struct PcmChunk {
            std::vector<uint8_t> bytes;
            size_t offset{0};
            int64_t ptsNs{0};
            bool silence{false};
            double estimatedFrequencyHz{0.0};
        };

        AudioOutput output;
        AVCodecContext* codec{nullptr};
        SwrContext* resampler{nullptr};
        AVRational timeBase{1, 1};
        AVSampleFormat outputFormat{AV_SAMPLE_FMT_NONE};
        int outputRate{0};
        int outputChannels{0};
        size_t outputBytesPerFrame{0};
        bool pack24{false};
        uint8_t silenceByte{0};
        bool started{false};
        bool decoderEof{false};
        bool contentSeen{false};
        bool contentSubmitted{false};
        uint64_t prefilledFrames{0};
        std::deque<PcmChunk> pending;
        int64_t ptsAnchorNs{AV_NOPTS_VALUE};
        int64_t outputSamplesFromAnchor{0};
        int64_t lastContentEndNs{AV_NOPTS_VALUE};
        int64_t streamEndNs{AV_NOPTS_VALUE};
        int64_t streamStartNs{AV_NOPTS_VALUE};
        int64_t submittedUntilNs{0};

        ~AudioWorkerState() {
            swr_free(&resampler);
            avcodec_free_context(&codec);
        }
    };

    static std::string hresultError(const char* operation, HRESULT result) {
        char value[16]{};
        sprintf_s(value, "0x%08lX", static_cast<unsigned long>(result));
        return std::string(operation) + ": " + value;
    }

    const AudioInfo* findAudioInfo(int streamIndex) const {
        const auto found = std::find_if(audioInfo.begin(), audioInfo.end(),
            [streamIndex](const AudioInfo& info) {
                return info.publicInfo.streamIndex == streamIndex;
            });
        return found == audioInfo.end() ? nullptr : &*found;
    }

    bool configureOutputFormat(AudioWorkerState& state) {
        const WAVEFORMATEX* mix = state.output.mixFormat();
        if (!mix || mix->nChannels == 0 || mix->nSamplesPerSec == 0 || mix->nBlockAlign == 0) {
            fail("audio endpoint returned an invalid mix format");
            return false;
        }
        WORD tag = mix->wFormatTag;
        WORD validBits = mix->wBitsPerSample;
        if (tag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
            const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
            validBits = extensible->Samples.wValidBitsPerSample;
            if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                tag = WAVE_FORMAT_IEEE_FLOAT;
            } else if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
                tag = WAVE_FORMAT_PCM;
            }
        }

        state.pack24 = false;
        state.silenceByte = 0;
        if (tag == WAVE_FORMAT_IEEE_FLOAT && mix->wBitsPerSample == 32) {
            state.outputFormat = AV_SAMPLE_FMT_FLT;
        } else if (tag == WAVE_FORMAT_IEEE_FLOAT && mix->wBitsPerSample == 64) {
            state.outputFormat = AV_SAMPLE_FMT_DBL;
        } else if (tag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 8) {
            state.outputFormat = AV_SAMPLE_FMT_U8;
            state.silenceByte = 128;
        } else if (tag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 16) {
            state.outputFormat = AV_SAMPLE_FMT_S16;
        } else if (tag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 24 && validBits <= 24) {
            state.outputFormat = AV_SAMPLE_FMT_S32;
            state.pack24 = true;
        } else if (tag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 32) {
            state.outputFormat = AV_SAMPLE_FMT_S32;
        } else {
            fail("unsupported audio endpoint mix format");
            return false;
        }
        state.outputRate = static_cast<int>(mix->nSamplesPerSec);
        state.outputChannels = mix->nChannels;
        state.outputBytesPerFrame = mix->nBlockAlign;
        if (mix->wBitsPerSample == 0 || mix->wBitsPerSample % 8 != 0 ||
            state.outputBytesPerFrame !=
                static_cast<size_t>(state.outputChannels) * (mix->wBitsPerSample / 8)) {
            fail("audio endpoint block alignment does not match its container format");
            return false;
        }
        return true;
    }

    bool rebuildAudioDecoder(AudioWorkerState& state, int streamIndex) {
        swr_free(&state.resampler);
        avcodec_free_context(&state.codec);
        state.pending.clear();
        state.ptsAnchorNs = AV_NOPTS_VALUE;
        state.outputSamplesFromAnchor = 0;
        state.lastContentEndNs = generationTargetNs.load();
        state.submittedUntilNs = generationTargetNs.load();
        currentAudioSampleRate.store(state.outputRate);
        currentFrequencyMilliHzFrames.store(0);
        currentFrequencyFrames.store(0);
        currentContentStartNs.store(AV_NOPTS_VALUE);
        currentContentEndNs.store(AV_NOPTS_VALUE);
        state.decoderEof = false;
        state.contentSeen = false;
        state.contentSubmitted = false;
        state.prefilledFrames = 0;
        state.started = false;

        const AudioInfo* info = findAudioInfo(streamIndex);
        if (!info) {
            fail("selected audio stream is unavailable");
            return false;
        }
        const AVCodec* decoder = avcodec_find_decoder(info->parameters->codec_id);
        if (!decoder) {
            fail("selected audio decoder is unavailable");
            return false;
        }
        state.codec = avcodec_alloc_context3(decoder);
        if (!state.codec) {
            fail("avcodec_alloc_context3(audio) failed");
            return false;
        }
        int result = avcodec_parameters_to_context(state.codec, info->parameters.get());
        state.codec->pkt_timebase = info->timeBase;
        if (result >= 0) result = avcodec_open2(state.codec, decoder, nullptr);
        if (result < 0) {
            fail(ffmpegError("avcodec_open2(audio)", result));
            return false;
        }
        state.timeBase = info->timeBase;
        state.streamStartNs = info->startPtsNs;
        state.streamEndNs = info->endPtsNs;
        const int64_t targetNs = generationTargetNs.load();
        const bool targetOutsideContent =
            (state.streamStartNs != AV_NOPTS_VALUE && targetNs < state.streamStartNs) ||
            (state.streamEndNs != AV_NOPTS_VALUE && targetNs >= state.streamEndNs);
        if (targetOutsideContent) {
            AudioWorkerState::PcmChunk clockSeed;
            const size_t seedFrames = (std::max)(
                size_t{1}, static_cast<size_t>(state.output.bufferFrameCapacity() / 2));
            clockSeed.bytes.assign(seedFrames * state.outputBytesPerFrame, state.silenceByte);
            clockSeed.ptsNs = targetNs;
            clockSeed.silence = true;
            state.pending.push_back(std::move(clockSeed));
        }
        return true;
    }

    bool ensureResampler(AudioWorkerState& state, const AVFrame* frame) {
        if (state.resampler) return true;
        AVChannelLayout inputLayout{};
        if (frame->ch_layout.nb_channels > 0) {
            const int copyResult = av_channel_layout_copy(&inputLayout, &frame->ch_layout);
            if (copyResult < 0) {
                fail(ffmpegError("av_channel_layout_copy", copyResult));
                return false;
            }
        } else {
            av_channel_layout_default(&inputLayout, state.codec->ch_layout.nb_channels);
        }
        AVChannelLayout outputLayout{};
        const WAVEFORMATEX* mix = state.output.mixFormat();
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
            const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
            if (extensible->dwChannelMask != 0) {
                if (std::bitset<32>(extensible->dwChannelMask).count() !=
                    static_cast<size_t>(state.outputChannels)) {
                    fail("audio endpoint channel mask does not match its channel count");
                    return false;
                }
                const int layoutResult =
                    av_channel_layout_from_mask(&outputLayout, extensible->dwChannelMask);
                if (layoutResult < 0) {
                    fail(ffmpegError("av_channel_layout_from_mask", layoutResult));
                    return false;
                }
            }
        }
        if (outputLayout.nb_channels == 0) {
            av_channel_layout_default(&outputLayout, state.outputChannels);
        }
        const int result = swr_alloc_set_opts2(
            &state.resampler, &outputLayout, state.outputFormat, state.outputRate,
            &inputLayout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate,
            0, nullptr);
        av_channel_layout_uninit(&outputLayout);
        av_channel_layout_uninit(&inputLayout);
        if (result < 0 || !state.resampler) {
            fail(ffmpegError("swr_alloc_set_opts2", result));
            return false;
        }
        const int initResult = swr_init(state.resampler);
        if (initResult < 0) {
            fail(ffmpegError("swr_init", initResult));
            return false;
        }
        return true;
    }

    static double estimateAudioFrequency(const AudioWorkerState& state,
                                         const std::vector<uint8_t>& converted,
                                         int frames) {
        if (frames < 2) return 0.0;
        auto sample = [&](int frameIndex) {
            const size_t sampleIndex = static_cast<size_t>(frameIndex) * state.outputChannels;
            switch (state.outputFormat) {
            case AV_SAMPLE_FMT_FLT:
                return static_cast<double>(reinterpret_cast<const float*>(converted.data())[sampleIndex]);
            case AV_SAMPLE_FMT_DBL:
                return reinterpret_cast<const double*>(converted.data())[sampleIndex];
            case AV_SAMPLE_FMT_U8:
                return (static_cast<double>(converted[sampleIndex]) - 128.0) / 128.0;
            case AV_SAMPLE_FMT_S16:
                return static_cast<double>(reinterpret_cast<const int16_t*>(converted.data())[sampleIndex]);
            case AV_SAMPLE_FMT_S32:
                return static_cast<double>(reinterpret_cast<const int32_t*>(converted.data())[sampleIndex]);
            default:
                return 0.0;
            }
        };

        unsigned crossings = 0;
        double previous = sample(0);
        for (int i = 1; i < frames; ++i) {
            const double current = sample(i);
            if ((previous < 0.0 && current >= 0.0) || (previous >= 0.0 && current < 0.0)) {
                ++crossings;
            }
            previous = current;
        }
        const double frequency = static_cast<double>(crossings) * state.outputRate /
                                 (2.0 * static_cast<double>(frames - 1));
        return frequency >= 100.0 && frequency <= 5'000.0 ? frequency : 0.0;
    }

    bool appendResampled(AudioWorkerState& state, const AVFrame* frame) {
        if (frame && !ensureResampler(state, frame)) return false;
        if (!state.resampler) return true;
        const int inputRate = frame ? frame->sample_rate : state.codec->sample_rate;
        const int inputSamples = frame ? frame->nb_samples : 0;
        const int64_t delay = swr_get_delay(state.resampler, inputRate);
        const int outputCapacity = static_cast<int>(av_rescale_rnd(
            delay + inputSamples, state.outputRate, inputRate, AV_ROUND_UP));
        if (outputCapacity <= 0) return true;

        const int temporaryBytesPerSample = av_get_bytes_per_sample(state.outputFormat);
        std::vector<uint8_t> converted(
            static_cast<size_t>(outputCapacity) * state.outputChannels * temporaryBytesPerSample);
        uint8_t* outputData[] = {converted.data()};
        const uint8_t* const* inputData = frame ? frame->extended_data : nullptr;
        const int produced = swr_convert(
            state.resampler, outputData, outputCapacity, inputData, inputSamples);
        if (produced < 0) {
            fail(ffmpegError("swr_convert", produced));
            return false;
        }
        if (produced == 0) return true;
        state.contentSeen = true;
        if (options.enableAudioDiagnostics) audioDecodedFrames.fetch_add(produced);

        int64_t candidatePtsNs = AV_NOPTS_VALUE;
        if (frame) {
            candidatePtsNs = timestampNanoseconds(frame->best_effort_timestamp, state.timeBase);
            if (candidatePtsNs != AV_NOPTS_VALUE) {
                candidatePtsNs -= av_rescale_q(
                    delay, AVRational{1, inputRate},
                    AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
            }
        }
        int64_t expectedPtsNs = state.ptsAnchorNs == AV_NOPTS_VALUE
            ? AV_NOPTS_VALUE
            : state.ptsAnchorNs + av_rescale_q(
                state.outputSamplesFromAnchor, AVRational{1, state.outputRate},
                AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
        const int64_t toleranceNs = (2 * kNanosecondsPerSecond) / state.outputRate;
        if (state.ptsAnchorNs == AV_NOPTS_VALUE ||
            (candidatePtsNs != AV_NOPTS_VALUE &&
             std::llabs(candidatePtsNs - expectedPtsNs) > toleranceNs)) {
            state.ptsAnchorNs = candidatePtsNs == AV_NOPTS_VALUE
                ? audioClockNs.load() : candidatePtsNs;
            state.outputSamplesFromAnchor = 0;
            expectedPtsNs = state.ptsAnchorNs;
        }

        AudioWorkerState::PcmChunk chunk;
        chunk.ptsNs = expectedPtsNs;
        if (options.enableAudioDiagnostics) {
            chunk.estimatedFrequencyHz = estimateAudioFrequency(state, converted, produced);
        }
        if (state.pack24) {
            const size_t values = static_cast<size_t>(produced) * state.outputChannels;
            chunk.bytes.resize(values * 3);
            for (size_t i = 0; i < values; ++i) {
                chunk.bytes[i * 3] = converted[i * 4 + 1];
                chunk.bytes[i * 3 + 1] = converted[i * 4 + 2];
                chunk.bytes[i * 3 + 2] = converted[i * 4 + 3];
            }
        } else {
            const size_t bytes = static_cast<size_t>(produced) * state.outputBytesPerFrame;
            chunk.bytes.assign(converted.begin(), converted.begin() + bytes);
        }
        state.outputSamplesFromAnchor += produced;
        state.lastContentEndNs = expectedPtsNs + av_rescale_q(
            produced, AVRational{1, state.outputRate},
            AVRational{1, static_cast<int>(kNanosecondsPerSecond)});

        const int64_t targetNs = generationTargetNs.load();
        if (chunk.ptsNs < targetNs) {
            const int64_t deltaNs = targetNs - chunk.ptsNs;
            const int64_t skipFrames = (std::min)(
                static_cast<int64_t>(produced),
                av_rescale_rnd(deltaNs, state.outputRate, kNanosecondsPerSecond, AV_ROUND_UP));
            chunk.offset = static_cast<size_t>(skipFrames) * state.outputBytesPerFrame;
            if (options.enableAudioDiagnostics) {
                audioPrerollTrimmedFrames.fetch_add(static_cast<uint64_t>(skipFrames));
            }
            chunk.ptsNs += av_rescale_q(
                skipFrames, AVRational{1, state.outputRate},
                AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
        }
        if (chunk.offset < chunk.bytes.size()) state.pending.push_back(std::move(chunk));
        return true;
    }

    bool receiveAudioFrames(AudioWorkerState& state, uint64_t localGeneration, bool draining) {
        while (!stop.load() && generation.load() == localGeneration) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                fail("av_frame_alloc(audio) failed");
                return false;
            }
            const int result = avcodec_receive_frame(state.codec, frame);
            if (result == 0) {
                const bool appended = appendResampled(state, frame);
                av_frame_free(&frame);
                if (!appended) return false;
                continue;
            }
            av_frame_free(&frame);
            if (result == AVERROR_EOF) {
                if (state.resampler) {
                    while (swr_get_delay(state.resampler, state.codec->sample_rate) > 0) {
                        const size_t before = state.pending.size();
                        if (!appendResampled(state, nullptr)) return false;
                        if (state.pending.size() == before) break;
                    }
                }
                state.decoderEof = true;
                return true;
            }
            if (result == AVERROR(EAGAIN)) {
                if (draining) {
                    fail("audio decoder requested input after drain started");
                    return false;
                }
                return true;
            }
            fail(ffmpegError("avcodec_receive_frame(audio)", result));
            return false;
        }
        return false;
    }

    bool sendAudioPacket(AudioWorkerState& state, AVPacket* packet, uint64_t localGeneration) {
        while (!stop.load() && generation.load() == localGeneration) {
            const int result = avcodec_send_packet(state.codec, packet);
            if (result == AVERROR_EOF && packet == nullptr) {
                state.decoderEof = true;
                return true;
            }
            if (result == AVERROR(EAGAIN)) {
                if (!receiveAudioFrames(state, localGeneration, false)) return false;
                continue;
            }
            if (result < 0) {
                fail(ffmpegError("avcodec_send_packet(audio)", result));
                return false;
            }
            return receiveAudioFrames(state, localGeneration, packet == nullptr);
        }
        return false;
    }

    bool publishAudioClock(AudioWorkerState& state, uint64_t localGeneration) {
        int64_t mediaNs = 0;
        const HRESULT result = state.output.mediaTimeNanoseconds(&mediaNs);
        if (result == S_OK) {
            std::lock_guard<std::mutex> lock(commandMutex);
            if (generation.load() == localGeneration && !failed.load()) {
                audioClockNs.store(mediaNs);
            }
        } else if (result != S_FALSE) {
            fail(hresultError("IAudioClock::GetPosition", result));
            return false;
        }
        return true;
    }

    static size_t pendingAudioFrames(const AudioWorkerState& state) {
        size_t frames = 0;
        for (const AudioWorkerState::PcmChunk& chunk : state.pending) {
            frames += (chunk.bytes.size() - chunk.offset) / state.outputBytesPerFrame;
        }
        return frames;
    }

    bool pumpAudio(AudioWorkerState& state, uint64_t localGeneration) {
        UINT32 available = 0;
        HRESULT result = state.output.availableFrames(&available);
        if (FAILED(result)) {
            fail(hresultError("IAudioClient::GetCurrentPadding", result));
            return false;
        }
        if (!publishAudioClock(state, localGeneration)) return false;
        int64_t currentClockNs = audioClockNs.load();
        int64_t queuedUntilNs = (std::max)(currentClockNs, state.submittedUntilNs);

        if (!state.pending.empty()) {
            AudioWorkerState::PcmChunk& chunk = state.pending.front();
            const int64_t lateNs = queuedUntilNs - chunk.ptsNs;
            if (state.started && lateNs > 0) {
                const int64_t lateFrames = (std::min)(
                    static_cast<int64_t>((chunk.bytes.size() - chunk.offset) /
                                         state.outputBytesPerFrame),
                    av_rescale_rnd(lateNs, state.outputRate, kNanosecondsPerSecond, AV_ROUND_DOWN));
                chunk.offset += static_cast<size_t>(lateFrames) * state.outputBytesPerFrame;
                if (options.enableAudioDiagnostics) {
                    audioLateTrimmedFrames.fetch_add(static_cast<uint64_t>(lateFrames));
                }
                chunk.ptsNs += av_rescale_q(
                    lateFrames, AVRational{1, state.outputRate},
                    AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
            }
        }

        // Keep the endpoint clock advancing through leading gaps and after a
        // short audio stream. Content is submitted only when its source PTS is
        // due at the end of the already queued endpoint region.
        const bool knownStreamEnded = state.streamEndNs != AV_NOPTS_VALUE &&
            currentClockNs >= state.streamEndNs;
        const bool needsSilence =
            (!state.pending.empty() && state.pending.front().ptsNs > queuedUntilNs) ||
            (state.pending.empty() &&
             (!state.contentSeen || state.decoderEof || knownStreamEnded));
        if (state.started && available > 0 && needsSilence) {
            size_t silenceFrames = available;
            if (!state.pending.empty()) {
                const int64_t gapNs = state.pending.front().ptsNs - queuedUntilNs;
                silenceFrames = (std::min)(
                    silenceFrames,
                    static_cast<size_t>(av_rescale_rnd(
                        gapNs, state.outputRate, kNanosecondsPerSecond, AV_ROUND_DOWN)));
            }
            if (silenceFrames > 0) {
                std::vector<uint8_t> silence(
                    silenceFrames * state.outputBytesPerFrame, state.silenceByte);
                result = state.output.write(silence.data(), silence.size(), queuedUntilNs);
                if (FAILED(result)) {
                    fail(hresultError("IAudioRenderClient::ReleaseBuffer(silence)", result));
                    return false;
                }
                if (options.enableAudioDiagnostics) {
                    audioSilenceFrames.fetch_add(silenceFrames);
                    if (!state.contentSubmitted) {
                        audioLeadingSilenceFrames.fetch_add(silenceFrames);
                    } else if (!state.pending.empty()) {
                        audioGapSilenceFrames.fetch_add(silenceFrames);
                    } else {
                        audioTrailingSilenceFrames.fetch_add(silenceFrames);
                    }
                }
                available -= static_cast<UINT32>(silenceFrames);
                queuedUntilNs += av_rescale_q(
                    static_cast<int64_t>(silenceFrames), AVRational{1, state.outputRate},
                    AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
                state.submittedUntilNs = queuedUntilNs;
            }
        }

        if (!state.pending.empty()) {
            AudioWorkerState::PcmChunk& chunk = state.pending.front();
            const int64_t oneSampleNs = kNanosecondsPerSecond / state.outputRate;
            const bool dueForSubmission =
                !state.started || chunk.ptsNs <= queuedUntilNs + oneSampleNs;
            const size_t pendingFrames =
                (chunk.bytes.size() - chunk.offset) / state.outputBytesPerFrame;
            const size_t writeFrames = dueForSubmission
                ? (std::min)(pendingFrames, static_cast<size_t>(available)) : 0;
            if (writeFrames > 0) {
                const size_t writeBytes = writeFrames * state.outputBytesPerFrame;
                const int64_t submissionErrorNs = std::llabs(queuedUntilNs - chunk.ptsNs);
                result = state.output.write(chunk.bytes.data() + chunk.offset,
                                            writeBytes, chunk.ptsNs);
                if (FAILED(result)) {
                    fail(hresultError("IAudioRenderClient::ReleaseBuffer", result));
                    return false;
                }
                if (!chunk.silence) state.contentSubmitted = true;
                if (options.enableAudioDiagnostics && chunk.silence) {
                    audioSilenceFrames.fetch_add(writeFrames);
                } else if (options.enableAudioDiagnostics) {
                    audioContentFrames.fetch_add(writeFrames);
                    int64_t noStart = AV_NOPTS_VALUE;
                    currentContentStartNs.compare_exchange_strong(noStart, chunk.ptsNs);
                    currentContentEndNs.store(chunk.ptsNs + av_rescale_q(
                        static_cast<int64_t>(writeFrames), AVRational{1, state.outputRate},
                        AVRational{1, static_cast<int>(kNanosecondsPerSecond)}));
                    int64_t maximumError = maximumAudioSubmissionErrorNs.load();
                    while (submissionErrorNs > maximumError &&
                           !maximumAudioSubmissionErrorNs.compare_exchange_weak(
                               maximumError, submissionErrorNs)) {}
                    if (chunk.estimatedFrequencyHz > 0.0) {
                        const uint64_t milliHz = static_cast<uint64_t>(
                            std::llround(chunk.estimatedFrequencyHz * 1'000.0));
                        currentFrequencyMilliHzFrames.fetch_add(milliHz * writeFrames);
                        currentFrequencyFrames.fetch_add(writeFrames);
                        double lowest = lowestAudioFrequencyHz.load();
                        while ((lowest == 0.0 || chunk.estimatedFrequencyHz < lowest) &&
                               !lowestAudioFrequencyHz.compare_exchange_weak(
                                   lowest, chunk.estimatedFrequencyHz)) {}
                        double highest = highestAudioFrequencyHz.load();
                        while (chunk.estimatedFrequencyHz > highest &&
                               !highestAudioFrequencyHz.compare_exchange_weak(
                                   highest, chunk.estimatedFrequencyHz)) {}
                    }
                }
                chunk.offset += writeBytes;
                chunk.ptsNs += av_rescale_q(
                    static_cast<int64_t>(writeFrames), AVRational{1, state.outputRate},
                    AVRational{1, static_cast<int>(kNanosecondsPerSecond)});
                state.submittedUntilNs = chunk.ptsNs;
                state.prefilledFrames += writeFrames;
            }
            if (chunk.offset == chunk.bytes.size()) state.pending.pop_front();
        }

        const uint64_t startThreshold = (std::max)(
            static_cast<uint64_t>(1), static_cast<uint64_t>(state.output.bufferFrameCapacity() / 2));
        if (!state.started && !paused.load() &&
            (state.prefilledFrames >= startThreshold ||
             (state.decoderEof && state.prefilledFrames > 0))) {
            result = state.output.start();
            if (FAILED(result)) {
                fail(hresultError("IAudioClient::Start", result));
                return false;
            }
            state.started = true;
        }
        if (!publishAudioClock(state, localGeneration)) return false;

        if ((state.decoderEof || knownStreamEnded) && state.pending.empty() &&
            audioClockNs.load() >= state.lastContentEndNs &&
            generation.load() == localGeneration) {
            markAudioDrained(localGeneration);
        }
        return true;
    }

    bool resetAudioGeneration(AudioWorkerState& state, uint64_t newGeneration,
                              int& localStream, uint64_t& localGeneration) {
        HRESULT result = state.output.pause();
        if (FAILED(result)) {
            fail(hresultError("IAudioClient::Stop", result));
            return false;
        }
        result = state.output.reset();
        if (FAILED(result)) {
            fail(hresultError("IAudioClient::Reset", result));
            return false;
        }
        localStream = selectedAudioStream.load();
        if (!rebuildAudioDecoder(state, localStream)) return false;
        localGeneration = newGeneration;
        audioDrained.store(false);
        return true;
    }

    void audioLoop() {
        AudioWorkerState state;
        HRESULT result = state.output.initialize();
        if (FAILED(result)) {
            fail(hresultError("AudioOutput::initialize", result));
            return;
        }
        if (!configureOutputFormat(state)) return;
        result = state.output.setVolume(desiredVolume.load());
        if (SUCCEEDED(result)) result = state.output.setMuted(desiredMuted.load());
        if (FAILED(result)) {
            fail(hresultError("audio volume initialization", result));
            return;
        }

        uint64_t localGeneration = generation.load();
        int localStream = selectedAudioStream.load();
        if (!rebuildAudioDecoder(state, localStream)) return;
        bool locallyPaused = paused.load();
        float appliedVolume = desiredVolume.load();
        bool appliedMuted = desiredMuted.load();

        while (!stop.load()) {
            const uint64_t requestedGeneration = generation.load();
            if (requestedGeneration != localGeneration ||
                selectedAudioStream.load() != localStream) {
                if (!resetAudioGeneration(state, requestedGeneration, localStream, localGeneration)) return;
                locallyPaused = paused.load();
            }

            const bool pauseRequested = paused.load();
            if (pauseRequested != locallyPaused) {
                if (pauseRequested) {
                    result = state.output.pause();
                    if (FAILED(result)) {
                        fail(hresultError("IAudioClient::Stop", result));
                        return;
                    }
                    state.started = false;
                } else if (state.prefilledFrames > 0) {
                    result = state.output.start();
                    if (FAILED(result)) {
                        fail(hresultError("IAudioClient::Start", result));
                        return;
                    }
                    state.started = true;
                }
                locallyPaused = pauseRequested;
                pauseApplied.store(pauseRequested);
                commandChanged.notify_all();
            }

            if (desiredVolume.load() != appliedVolume) {
                appliedVolume = desiredVolume.load();
                result = state.output.setVolume(appliedVolume);
                if (FAILED(result)) {
                    fail(hresultError("ISimpleAudioVolume::SetMasterVolume", result));
                    return;
                }
            }
            if (desiredMuted.load() != appliedMuted) {
                appliedMuted = desiredMuted.load();
                result = state.output.setMuted(appliedMuted);
                if (FAILED(result)) {
                    fail(hresultError("ISimpleAudioVolume::SetMute", result));
                    return;
                }
            }

            const size_t prefetchLimit =
                (std::max)(static_cast<size_t>(state.output.bufferFrameCapacity()) * 2,
                           static_cast<size_t>(state.outputRate / 2));
            while (!state.decoderEof && pendingAudioFrames(state) < prefetchLimit) {
                PacketItem readyItem;
                if (!audioPackets.tryTake(readyItem)) break;
                commandChanged.notify_all();
                const uint64_t afterTakeGeneration = generation.load();
                if (afterTakeGeneration != localGeneration ||
                    selectedAudioStream.load() != localStream) {
                    if (!resetAudioGeneration(state, afterTakeGeneration,
                                              localStream, localGeneration)) {
                        return;
                    }
                    locallyPaused = paused.load();
                }
                if (readyItem.generation != localGeneration) continue;
                const bool accepted = readyItem.eof
                    ? sendAudioPacket(state, nullptr, localGeneration)
                    : sendAudioPacket(state, readyItem.packet, localGeneration);
                if (!accepted && (stop.load() || failed.load() ||
                                  generation.load() == localGeneration)) {
                    return;
                }
            }

            if (state.contentSeen && state.pending.empty() && !state.decoderEof) {
                PacketItem awaitedItem;
                if (audioPackets.takeFor(awaitedItem, stop)) {
                    commandChanged.notify_all();
                    const uint64_t afterTakeGeneration = generation.load();
                    if (afterTakeGeneration != localGeneration ||
                        selectedAudioStream.load() != localStream) {
                        if (!resetAudioGeneration(state, afterTakeGeneration,
                                                  localStream, localGeneration)) {
                            return;
                        }
                        locallyPaused = paused.load();
                    }
                    if (awaitedItem.generation != localGeneration) continue;
                    const bool accepted = awaitedItem.eof
                        ? sendAudioPacket(state, nullptr, localGeneration)
                        : sendAudioPacket(state, awaitedItem.packet, localGeneration);
                    if (!accepted && (stop.load() || failed.load() ||
                                      generation.load() == localGeneration)) {
                        return;
                    }
                    continue;
                }
            }

            if (!pumpAudio(state, localGeneration)) return;
            if (paused.load()) {
                WaitForSingleObject(state.output.eventHandle(), 5);
                continue;
            }
            if (!state.pending.empty()) {
                UINT32 immediatelyAvailable = 0;
                result = state.output.availableFrames(&immediatelyAvailable);
                if (FAILED(result)) {
                    fail(hresultError("IAudioClient::GetCurrentPadding", result));
                    return;
                }
                if (immediatelyAvailable == 0) {
                    WaitForSingleObject(state.output.eventHandle(), 5);
                }
                continue;
            }
            if (state.decoderEof) {
                WaitForSingleObject(state.output.eventHandle(), 5);
                continue;
            }

            PacketItem item;
            if (!audioPackets.takeFor(item, stop)) continue;
            commandChanged.notify_all();
            const uint64_t afterTakeGeneration = generation.load();
            if (afterTakeGeneration != localGeneration ||
                selectedAudioStream.load() != localStream) {
                if (!resetAudioGeneration(state, afterTakeGeneration,
                                          localStream, localGeneration)) {
                    return;
                }
                locallyPaused = paused.load();
            }
            if (item.generation != localGeneration) continue;
            if (item.eof) {
                if (!sendAudioPacket(state, nullptr, localGeneration)) {
                    if (!stop.load() && !failed.load() &&
                        generation.load() != localGeneration) {
                        continue;
                    }
                    return;
                }
            } else if (!sendAudioPacket(state, item.packet, localGeneration)) {
                if (!stop.load() && !failed.load() && generation.load() != localGeneration) {
                    continue;
                }
                return;
            }
        }
    }
};

VideoPipeline::VideoPipeline(ID3D11Device* device, const std::wstring& path)
    : VideoPipeline(device, path, Options{}) {}

VideoPipeline::VideoPipeline(ID3D11Device* device, const std::wstring& path,
                             const Options& options)
    : m_impl(std::make_unique<Impl>(device, path, options)) {}

VideoPipeline::~VideoPipeline() = default;

AVFrame* VideoPipeline::pollLatest() {
    while (QueuedVideoFrame* queued = m_impl->videoFrames.tryTake()) {
        AVFrame* frame = queued->frame;
        const bool current = queued->generation == m_impl->generation.load();
        queued->frame = nullptr;
        delete queued;
        if (current) return frame;
        av_frame_free(&frame);
    }
    return nullptr;
}

void VideoPipeline::clear() {
    m_impl->videoFrames.clear();
}

bool VideoPipeline::pause() {
    {
        std::lock_guard<std::mutex> commandLock(m_impl->commandMutex);
        if (m_impl->failed.load() || m_impl->stop.load() || m_impl->paused.load()) return false;
        m_impl->paused.store(true);
        if (!m_impl->usesAudioClock) {
            std::lock_guard<std::mutex> clockLock(m_impl->fallbackClockMutex);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - m_impl->fallbackAnchorWall).count();
            m_impl->fallbackAnchorMediaNs += elapsed;
            m_impl->pauseApplied.store(true);
        }
    }
    m_impl->commandChanged.notify_all();
    if (!m_impl->usesAudioClock) return true;
    std::unique_lock<std::mutex> lock(m_impl->commandMutex);
    return m_impl->commandChanged.wait_for(
        lock, std::chrono::milliseconds(500),
        [this] {
            return m_impl->pauseApplied.load() || m_impl->failed.load() || m_impl->stop.load();
        }) && m_impl->pauseApplied.load();
}

bool VideoPipeline::resume() {
    {
        std::lock_guard<std::mutex> commandLock(m_impl->commandMutex);
        if (m_impl->failed.load() || m_impl->stop.load() || !m_impl->paused.load()) return false;
        m_impl->paused.store(false);
        if (!m_impl->usesAudioClock) {
            std::lock_guard<std::mutex> clockLock(m_impl->fallbackClockMutex);
            m_impl->fallbackAnchorWall = std::chrono::steady_clock::now();
        }
        m_impl->pauseApplied.store(false);
    }
    m_impl->commandChanged.notify_all();
    return true;
}

void VideoPipeline::stop() {
    m_impl->shutdown();
}

bool VideoPipeline::paused() const {
    return m_impl->paused.load();
}

bool VideoPipeline::seekSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0 ||
        seconds > static_cast<double>((std::numeric_limits<int64_t>::max)()) /
                      static_cast<double>(kNanosecondsPerSecond)) {
        return false;
    }
    return m_impl->requestSeek(static_cast<int64_t>(seconds * kNanosecondsPerSecond));
}

std::vector<VideoPipeline::AudioTrack> VideoPipeline::audioTracks() const {
    std::vector<AudioTrack> result;
    result.reserve(m_impl->audioInfo.size());
    for (const Impl::AudioInfo& info : m_impl->audioInfo) result.push_back(info.publicInfo);
    return result;
}

int VideoPipeline::selectedAudioTrack() const {
    return m_impl->selectedAudioStream.load();
}

bool VideoPipeline::selectAudioTrack(int streamIndex) {
    if (!m_impl->usesAudioClock || !m_impl->findAudioInfo(streamIndex)) return false;
    {
        std::lock_guard<std::mutex> lock(m_impl->commandMutex);
        if (m_impl->failed.load() || m_impl->stop.load()) return false;
        if (streamIndex == m_impl->selectedAudioStream.load()) return true;
    }
    const double currentPosition = positionSeconds();
    if (!std::isfinite(currentPosition) || currentPosition < 0.0) return false;
    const auto relativeNs = static_cast<int64_t>(currentPosition * kNanosecondsPerSecond);
    return m_impl->requestSeek(relativeNs, streamIndex);
}

bool VideoPipeline::setVolume(float scalar) {
    if (!std::isfinite(scalar) || scalar < 0.0f || scalar > 1.0f) return false;
    {
        std::lock_guard<std::mutex> lock(m_impl->commandMutex);
        if (m_impl->failed.load() || m_impl->stop.load()) return false;
        m_impl->desiredVolume.store(scalar);
    }
    m_impl->commandChanged.notify_all();
    return true;
}

void VideoPipeline::setMuted(bool muted) {
    {
        std::lock_guard<std::mutex> lock(m_impl->commandMutex);
        if (m_impl->failed.load() || m_impl->stop.load()) return;
        m_impl->desiredMuted.store(muted);
    }
    m_impl->commandChanged.notify_all();
}

bool VideoPipeline::muted() const {
    return m_impl->desiredMuted.load();
}

float VideoPipeline::volume() const {
    return m_impl->desiredVolume.load();
}

VideoPipeline::AudioDiagnostics VideoPipeline::audioDiagnostics() const {
    AudioDiagnostics result;
    result.contentFramesSubmitted = m_impl->audioContentFrames.load();
    result.silenceFramesSubmitted = m_impl->audioSilenceFrames.load();
    result.gapSilenceFramesSubmitted = m_impl->audioGapSilenceFrames.load();
    result.leadingSilenceFramesSubmitted = m_impl->audioLeadingSilenceFrames.load();
    result.trailingSilenceFramesSubmitted = m_impl->audioTrailingSilenceFrames.load();
    result.contentFramesDecoded = m_impl->audioDecodedFrames.load();
    result.prerollFramesTrimmed = m_impl->audioPrerollTrimmedFrames.load();
    result.lateFramesTrimmed = m_impl->audioLateTrimmedFrames.load();
    result.maximumSubmissionErrorNs = m_impl->maximumAudioSubmissionErrorNs.load();
    result.lowestObservedFrequencyHz = m_impl->lowestAudioFrequencyHz.load();
    result.highestObservedFrequencyHz = m_impl->highestAudioFrequencyHz.load();
    const uint64_t frequencyFrames = m_impl->currentFrequencyFrames.load();
    result.currentTrackFrequencyHz = frequencyFrames == 0 ? 0.0 :
        static_cast<double>(m_impl->currentFrequencyMilliHzFrames.load()) /
        (1'000.0 * static_cast<double>(frequencyFrames));
    result.outputSampleRate = m_impl->currentAudioSampleRate.load();
    result.currentContentStartNs = m_impl->currentContentStartNs.load();
    result.currentContentEndNs = m_impl->currentContentEndNs.load();
    return result;
}

std::vector<VideoPipeline::SubtitleTrack> VideoPipeline::subtitleTracks() const {
    std::vector<SubtitleTrack> result;
    result.reserve(m_impl->subtitleInfo.size());
    for (const Impl::SubtitleInfo& info : m_impl->subtitleInfo)
        result.push_back(info.publicInfo);
    return result;
}

std::optional<int> VideoPipeline::selectedSubtitleTrack() const {
    if (m_impl->externalSubtitleSelected.load()) return std::nullopt;
    const int selected = m_impl->selectedSubtitleStream.load();
    return selected >= 0 ? std::optional<int>(selected) : std::nullopt;
}

bool VideoPipeline::selectSubtitleTrack(int streamIndex) {
    const Impl::SubtitleInfo* info = m_impl->findSubtitleInfo(streamIndex);
    if (!info || !info->publicInfo.supported) return false;
    {
        std::lock_guard<std::mutex> lock(m_impl->commandMutex);
        if (m_impl->failed.load() || m_impl->stop.load()) return false;
        if (!m_impl->externalSubtitleSelected.load() &&
            m_impl->selectedSubtitleStream.load() == streamIndex) {
            return true;
        }
    }
    const double currentPosition = positionSeconds();
    if (!std::isfinite(currentPosition) || currentPosition < 0.0) return false;
    return m_impl->requestSeek(
        static_cast<int64_t>(currentPosition * kNanosecondsPerSecond),
        std::nullopt, streamIndex);
}

void VideoPipeline::disableSubtitles() {
    std::lock_guard<std::mutex> commandLock(m_impl->commandMutex);
    if (m_impl->failed.load() || m_impl->stop.load()) return;
    m_impl->selectedSubtitleStream.store(-1);
    m_impl->externalSubtitleSelected.store(false);
    std::lock_guard<std::mutex> subtitleLock(m_impl->subtitleMutex);
    m_impl->embeddedSubtitleCues.clear();
    m_impl->externalSubtitleCues.clear();
    std::lock_guard<std::mutex> errorLock(m_impl->subtitleErrorMutex);
    m_impl->subtitleErrorText.clear();
}

bool VideoPipeline::loadExternalSubRip(const std::wstring& path, std::string* error) {
    std::vector<SubtitleCue> cues;
    std::string parseError;
    if (!SubtitleDecoder::parseSrtFile(path, cues, parseError)) {
        {
            std::lock_guard<std::mutex> lock(m_impl->subtitleErrorMutex);
            m_impl->subtitleErrorText = parseError;
        }
        if (error) *error = std::move(parseError);
        return false;
    }
    {
        std::lock_guard<std::mutex> commandLock(m_impl->commandMutex);
        if (m_impl->failed.load() || m_impl->stop.load()) {
            if (error) *error = "pipeline is stopped";
            return false;
        }
        m_impl->selectedSubtitleStream.store(-1);
        m_impl->externalSubtitleSelected.store(true);
        std::lock_guard<std::mutex> subtitleLock(m_impl->subtitleMutex);
        m_impl->embeddedSubtitleCues.clear();
        m_impl->externalSubtitleCues = std::move(cues);
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->subtitleErrorMutex);
        m_impl->subtitleErrorText.clear();
    }
    if (error) error->clear();
    return true;
}

bool VideoPipeline::externalSubtitlesSelected() const {
    return m_impl->externalSubtitleSelected.load();
}

bool VideoPipeline::setSubtitleOffsetSeconds(double seconds) {
    if (!std::isfinite(seconds) ||
        std::abs(seconds) > static_cast<double>(kMaximumSubtitleOffsetNs) /
                                kNanosecondsPerSecond) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_impl->commandMutex);
    if (m_impl->failed.load() || m_impl->stop.load()) return false;
    m_impl->subtitleOffsetNs.store(
        static_cast<int64_t>(seconds * kNanosecondsPerSecond));
    return true;
}

std::vector<SubtitleCue> VideoPipeline::subtitleCuesAt(int64_t mediaNanoseconds) const {
    std::lock_guard<std::mutex> lock(m_impl->subtitleMutex);
    if (m_impl->externalSubtitleSelected.load()) {
        return SubtitleDecoder::activeCues(
            m_impl->externalSubtitleCues, mediaNanoseconds,
            m_impl->subtitleOffsetNs.load());
    }
    if (m_impl->selectedSubtitleStream.load() >= 0) {
        return SubtitleDecoder::activeCues(
            m_impl->embeddedSubtitleCues, mediaNanoseconds,
            m_impl->subtitleOffsetNs.load());
    }
    return {};
}

std::string VideoPipeline::subtitleError() const {
    std::lock_guard<std::mutex> lock(m_impl->subtitleErrorMutex);
    return m_impl->subtitleErrorText;
}

int64_t VideoPipeline::mediaTimeNanoseconds() const {
    return m_impl->usesAudioClock ? m_impl->audioClockNs.load() : m_impl->fallbackClockNow();
}

uint64_t VideoPipeline::generation() const {
    return m_impl->generation.load();
}

double VideoPipeline::positionSeconds() const {
    const int64_t relative = (std::max)(
        int64_t{0}, mediaTimeNanoseconds() - m_impl->timelineStartNs);
    const int64_t bounded = m_impl->durationNs > 0
        ? (std::min)(relative, m_impl->durationNs) : relative;
    return static_cast<double>(bounded) / static_cast<double>(kNanosecondsPerSecond);
}

double VideoPipeline::durationSeconds() const {
    return static_cast<double>(m_impl->durationNs) / static_cast<double>(kNanosecondsPerSecond);
}

bool VideoPipeline::finished() const {
    return m_impl->done.load();
}

VideoPipeline::Status VideoPipeline::status() const {
    return m_impl->status.load();
}

std::string VideoPipeline::error() const {
    std::lock_guard<std::mutex> lock(m_impl->errorMutex);
    return m_impl->errorText;
}

int VideoPipeline::width() const { return m_impl->videoWidth; }
int VideoPipeline::height() const { return m_impl->videoHeight; }
int VideoPipeline::timebaseNum() const { return m_impl->videoTimeBaseNum; }
int VideoPipeline::timebaseDen() const { return m_impl->videoTimeBaseDen; }
unsigned VideoPipeline::decodedFrameCount() const { return m_impl->decodedFrames.load(); }
unsigned VideoPipeline::droppedFrameCount() const { return 0; }

std::recursive_mutex& VideoPipeline::contextMutex() {
    return m_impl->contextMutex;
}

} // namespace odyssey
