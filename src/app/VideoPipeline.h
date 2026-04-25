#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <d3d11.h>

#include "FrameMailbox.h"

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVRational;
}

namespace odyssey {

// Owns the FFmpeg demux + decode pipeline for a single file. M2 scope:
// D3D11VA hardware decode of H.264 into NV12 texture-array frames. Software
// fallback (YUV420P + CPU->GPU upload) is reserved for M7 where it unblocks
// non-Odyssey dev boxes; for M2 the ctor throws if D3D11VA cannot be
// established against the supplied ID3D11Device.
class VideoPipeline {
public:
    VideoPipeline(ID3D11Device* device, const std::wstring& path);
    ~VideoPipeline();

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // Non-blocking poll for the newest decoded frame. The caller owns the
    // returned AVFrame and must av_frame_free it. Returns null when no new
    // frame has landed since the last call.
    AVFrame* pollLatest();

    // True once the decode thread has drained the file (EOF or error) and
    // the mailbox has been emptied. Used by the main loop to know when to
    // stop requesting frames.
    bool finished() const;

    int width()  const { return m_width; }
    int height() const { return m_height; }

    // Stream timebase for PTS -> nanoseconds conversion (see PtsMath).
    int timebaseNum() const { return m_tbNum; }
    int timebaseDen() const { return m_tbDen; }

    unsigned decodedFrameCount() const { return m_decodedCount.load(); }
    unsigned droppedFrameCount() const { return m_mailbox.dropCount(); }

    // Recursive mutex shared with FFmpeg's D3D11VA decoder so both threads
    // serialize all ID3D11DeviceContext access. The render thread MUST hold
    // this lock around any D3D11 call (CopySubresourceRegion, Map, Draw,
    // weaver frameBegin/frameWeave, Present) — D3D11's MT-protect serializes
    // individual calls atomically, but not multi-call render sequences. The
    // FFmpeg decode thread acquires/releases this same lock via the callbacks
    // we registered on AVD3D11VADeviceContext. Per FFmpeg's contract the lock
    // must be recursive.
    std::recursive_mutex& contextMutex() { return m_ctxMutex; }

private:
    void decodeLoop();

    ID3D11Device* m_device{nullptr};

    AVFormatContext* m_fmt{nullptr};
    AVCodecContext*  m_codec{nullptr};
    AVBufferRef*     m_hwDevCtx{nullptr};
    int              m_videoStreamIdx{-1};
    int              m_width{0}, m_height{0};
    int              m_tbNum{1}, m_tbDen{90000};

    FrameMailbox<AVFrame> m_mailbox;
    std::recursive_mutex  m_ctxMutex;
    std::thread           m_thread;
    std::atomic<bool>     m_stop{false};
    std::atomic<bool>     m_done{false};
    std::atomic<unsigned> m_decodedCount{0};
};

} // namespace odyssey
