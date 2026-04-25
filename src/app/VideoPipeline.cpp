#include "VideoPipeline.h"

#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

namespace odyssey {

namespace {

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static void freeAvFrame(AVFrame* f) { av_frame_free(&f); }

// Allocates an AVHWFramesContext tied to our hwdevice with BindFlags that
// include D3D11_BIND_SHADER_RESOURCE. FFmpeg's default pool only requests
// D3D11_BIND_DECODER, which means the resulting NV12 textures can be decoded
// into but cannot back an SRV — CreateShaderResourceView then fails with
// E_INVALIDARG. By pre-allocating the frames context with the right bind
// flags, the main thread can sample the decoded NV12 directly, no copy.
static bool setupHwFrames(AVCodecContext* ctx, AVBufferRef* hwDevCtx) {
    if (ctx->hw_frames_ctx) return true;
    AVBufferRef* frames = av_hwframe_ctx_alloc(hwDevCtx);
    if (!frames) return false;
    auto* hwctx = reinterpret_cast<AVHWFramesContext*>(frames->data);
    hwctx->format            = AV_PIX_FMT_D3D11;
    hwctx->sw_format         = AV_PIX_FMT_NV12;
    hwctx->width             = ctx->coded_width;
    hwctx->height            = ctx->coded_height;
    hwctx->initial_pool_size = 20;
    auto* d3dctx = reinterpret_cast<AVD3D11VAFramesContext*>(hwctx->hwctx);
    d3dctx->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    if (av_hwframe_ctx_init(frames) < 0) {
        av_buffer_unref(&frames);
        return false;
    }
    ctx->hw_frames_ctx = frames;
    return true;
}

static AVPixelFormat pickD3d11(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    auto* hwDevCtx = static_cast<AVBufferRef*>(ctx->opaque);
    for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmts[i] == AV_PIX_FMT_D3D11) {
            if (hwDevCtx && setupHwFrames(ctx, hwDevCtx)) return AV_PIX_FMT_D3D11;
            break;
        }
    }
    // No D3D11VA offer or hwframes init failed — ctor will reject non-D3D11
    // frames downstream.
    return fmts[0];
}

} // namespace

VideoPipeline::VideoPipeline(ID3D11Device* device, const std::wstring& path)
    : m_device(device), m_mailbox(freeAvFrame)
{
    const std::string utf8Path = wideToUtf8(path);

    if (avformat_open_input(&m_fmt, utf8Path.c_str(), nullptr, nullptr) < 0)
        throw std::runtime_error("avformat_open_input failed: " + utf8Path);
    if (avformat_find_stream_info(m_fmt, nullptr) < 0)
        throw std::runtime_error("avformat_find_stream_info failed");

    const AVCodec* dec = nullptr;
    m_videoStreamIdx = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (m_videoStreamIdx < 0 || !dec) throw std::runtime_error("no video stream");

    AVStream* st = m_fmt->streams[m_videoStreamIdx];
    m_tbNum = st->time_base.num;
    m_tbDen = st->time_base.den;

    m_codec = avcodec_alloc_context3(dec);
    if (!m_codec) throw std::runtime_error("avcodec_alloc_context3 failed");
    if (avcodec_parameters_to_context(m_codec, st->codecpar) < 0)
        throw std::runtime_error("avcodec_parameters_to_context failed");

    // Wire our ID3D11Device into a hwdevice context. Setting device here tells
    // FFmpeg to reuse our device + immediate context rather than creating its
    // own — required so the decoded textures live on the same device as the
    // weaver, and the Immersity weaver can sample them without interop.
    m_hwDevCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!m_hwDevCtx) throw std::runtime_error("av_hwdevice_ctx_alloc(D3D11VA) failed");
    {
        auto* hwCtx = reinterpret_cast<AVHWDeviceContext*>(m_hwDevCtx->data);
        auto* d3dCtx = reinterpret_cast<AVD3D11VADeviceContext*>(hwCtx->hwctx);
        d3dCtx->device = m_device;
        m_device->AddRef();
    }
    if (av_hwdevice_ctx_init(m_hwDevCtx) < 0)
        throw std::runtime_error("av_hwdevice_ctx_init failed");

    m_codec->hw_device_ctx = av_buffer_ref(m_hwDevCtx);
    m_codec->get_format    = pickD3d11;
    m_codec->opaque        = m_hwDevCtx; // picked up by pickD3d11 to alloc hwframes
    m_codec->thread_count  = 1; // D3D11VA requires single-threaded decode.

    if (avcodec_open2(m_codec, dec, nullptr) < 0)
        throw std::runtime_error("avcodec_open2 failed (codec may lack D3D11VA support)");

    m_width  = m_codec->width;
    m_height = m_codec->height;

    m_thread = std::thread(&VideoPipeline::decodeLoop, this);
}

VideoPipeline::~VideoPipeline() {
    m_stop.store(true);
    m_mailbox.close();
    if (m_thread.joinable()) m_thread.join();

    if (m_codec)    avcodec_free_context(&m_codec);
    if (m_hwDevCtx) av_buffer_unref(&m_hwDevCtx);
    if (m_fmt)      avformat_close_input(&m_fmt);
}

AVFrame* VideoPipeline::pollLatest() {
    return m_mailbox.tryTake();
}

bool VideoPipeline::finished() const {
    return m_done.load();
}

void VideoPipeline::decodeLoop() {
    AVPacket* pkt = av_packet_alloc();

    while (!m_stop.load()) {
        int r = av_read_frame(m_fmt, pkt);
        if (r == AVERROR_EOF) {
            avcodec_send_packet(m_codec, nullptr); // flush
            while (!m_stop.load()) {
                AVFrame* f = av_frame_alloc();
                int rr = avcodec_receive_frame(m_codec, f);
                if (rr == 0) {
                    ++m_decodedCount;
                    m_mailbox.publish(f);
                } else {
                    av_frame_free(&f);
                    break;
                }
            }
            break;
        } else if (r < 0) {
            break;
        }

        if (pkt->stream_index != m_videoStreamIdx) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(m_codec, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (!m_stop.load()) {
            AVFrame* f = av_frame_alloc();
            int rr = avcodec_receive_frame(m_codec, f);
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) {
                av_frame_free(&f);
                break;
            }
            if (rr < 0) {
                av_frame_free(&f);
                m_stop.store(true);
                break;
            }
            ++m_decodedCount;
            m_mailbox.publish(f);
        }
    }

    av_packet_free(&pkt);
    m_done.store(true);
    m_mailbox.close();
}

} // namespace odyssey
