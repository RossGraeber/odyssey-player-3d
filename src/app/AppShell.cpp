#include "AppShell.h"

#include <wincodec.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <future>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _DEBUG
#include <d3d11sdklayers.h>
#endif

#include "VideoPipeline.h"
#include "PlaybackSession.h"
#include "PlayerControls.h"
#include "VideoTiming.h"
#include "Nv12ToRgba.h"
#include "StereoCompositor.h"
#include "SubtitleRenderer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/stereo3d.h>
}

#pragma comment(lib, "windowscodecs.lib")

namespace odyssey {

// sRGB (#0A0A0A) -> linear. 0x0A/255 = 0.03921568. Below 0.04045 the transfer
// is linear/12.92, so linearValue = 0.03921568 / 12.92 ~= 0.003035.
static constexpr float kClearLinear = 0.003035f;
static const float kClearColor[4] = { kClearLinear, kClearLinear, kClearLinear, 1.0f };

namespace {

struct HostFrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};

bool validDisplayRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

std::wstring executableLavDirectory() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return (std::filesystem::path(std::wstring(path.data(), length)).parent_path() / L"lav").wstring();
}

std::string narrowUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return "unavailable";
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

struct FramePacking {
    StereoMode mode;
    bool invertEyes;
    bool supported;
};

FramePacking automaticFramePacking(const AVFrame* frame, const std::wstring& path,
                                   bool mvc) {
    if (mvc) return {StereoMode::FullSbs, false, true};

    const AVFrameSideData* sideData = av_frame_get_side_data(
        frame, AV_FRAME_DATA_STEREO3D);
    if (sideData && sideData->size >= sizeof(AVStereo3D)) {
        const auto* stereo = reinterpret_cast<const AVStereo3D*>(sideData->data);
        if (stereo->type == AV_STEREO3D_2D) return {StereoMode::Mono2D, false, true};
        if (stereo->type == AV_STEREO3D_SIDEBYSIDE) {
            const double packedAspect = static_cast<double>(frame->width) /
                                        (std::max)(1, frame->height);
            return {packedAspect >= 3.0 ? StereoMode::FullSbs : StereoMode::HalfSbs,
                    (stereo->flags & AV_STEREO3D_FLAG_INVERT) != 0, true};
        }
        if (stereo->type != AV_STEREO3D_UNSPEC) {
            return {StereoMode::FullSbs,
                    (stereo->flags & AV_STEREO3D_FLAG_INVERT) != 0, false};
        }
    }

    std::wstring lower = std::filesystem::path(path).filename().wstring();
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    if (lower.find(L"hsbs") != std::wstring::npos ||
        lower.find(L"half-sbs") != std::wstring::npos) {
        return {StereoMode::HalfSbs, false, true};
    }
    if (lower.find(L"fsbs") != std::wstring::npos ||
        lower.find(L"full-sbs") != std::wstring::npos) {
        return {StereoMode::FullSbs, false, true};
    }
    const double packedAspect = static_cast<double>(frame->width) /
                                (std::max)(1, frame->height);
    return {packedAspect >= 3.0 ? StereoMode::FullSbs : StereoMode::HalfSbs,
            false, true};
}

std::uint64_t subtitleSignature(const std::vector<SubtitleCue>& cues) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto append = [&hash](const void* data, size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    };
    for (const SubtitleCue& cue : cues) {
        append(&cue.startNanoseconds, sizeof(cue.startNanoseconds));
        const int64_t end = cue.endNanoseconds.value_or((std::numeric_limits<int64_t>::max)());
        append(&end, sizeof(end));
        append(cue.textUtf8.data(), cue.textUtf8.size());
        append(&cue.sourceWidth, sizeof(cue.sourceWidth));
        append(&cue.sourceHeight, sizeof(cue.sourceHeight));
        for (const SubtitleBitmap& bitmap : cue.bitmaps) {
            append(&bitmap.x, sizeof(bitmap.x));
            append(&bitmap.y, sizeof(bitmap.y));
            append(&bitmap.width, sizeof(bitmap.width));
            append(&bitmap.height, sizeof(bitmap.height));
            append(&bitmap.strideBytes, sizeof(bitmap.strideBytes));
            append(bitmap.pixels.data(), bitmap.pixels.size());
        }
    }
    return hash;
}

} // namespace

struct AppShell::Host {
    enum class LayoutChoice { Auto, FullSbs, HalfSbs, Mono2D };

    std::unique_ptr<ImmersityWeaver> weaver;
    std::unique_ptr<StereoCompositor> compositor;
    std::unique_ptr<Nv12ToRgba> converter;
    std::unique_ptr<PlaybackSession> session;
    std::vector<std::future<void>> retiredSessions;
    std::unique_ptr<AVFrame, HostFrameDeleter> pendingFrame;
    std::unique_ptr<AVFrame, HostFrameDeleter> retainedFrame;
    PlayerControls controls;
    SubtitleRenderer subtitleRenderer;
    std::vector<RenderedSubtitle> renderedSubtitles;
    PlayerControlSurface controlsSurface;
    std::wstring currentPath;
    std::wstring hostError;
    std::wstring captionError;
    std::wstring displayError;
    LayoutChoice layout{LayoutChoice::Auto};
    uint64_t generation{0};
    bool haveViews{false};
    bool viewsDirty{false};
    bool focused{true};
    bool minimized{false};
    bool nativeUiActive{false};
    bool swapEyes{false};
    bool muted{false};
    float volume{1.0f};
    std::optional<double> pendingSeekSeconds;
    std::uint64_t pendingSeekAt{0};
    StereoLayout retainedLayout{};
    UINT retainedSourceWidth{0};
    UINT retainedSourceHeight{0};
    StereoMode retainedMode{StereoMode::FullSbs};
    bool retainedMetadataInvert{false};
    bool controlsDirty{true};
    std::uint64_t controlsRenderedAt{0};
    std::uint64_t renderedSubtitleSignature{0};
    RECT renderedSubtitleMovieRect{};
    UINT renderedSubtitleCanvasWidth{0};
    UINT renderedSubtitleCanvasHeight{0};
    std::optional<int> renderedSubtitleTextSafeBottom;
    std::uint64_t scheduledPresented{0};
    std::uint64_t scheduledDropped{0};
    std::uint64_t scheduledWaited{0};
    std::int64_t maximumPresentedAgeNs{0};
    std::uint64_t steadyPresented{0};
    std::uint64_t steadyOutside40ms{0};
    std::vector<std::int64_t> steadyPresentationAgesNs;
    std::optional<std::int64_t> retainedPtsNs;
    std::optional<std::int64_t> retainedDurationNs;
    std::optional<std::int64_t> nominalFrameDurationNs;
    std::optional<std::int64_t> firstPresentedPtsNs;
    std::optional<std::int64_t> lastPresentedPtsNs;
    std::optional<std::int64_t> firstPresentedClockNs;
    std::optional<std::int64_t> lastPresentedClockNs;
    bool unrecordedPresentation{false};
    bool gateValidDisplay{false};
    bool gateHasFrame{false};
    bool gateStereo{false};
    bool gateSdkActive{false};
    bool gateNativeUiClear{false};
    bool gateFocused{false};
    bool gateNativeRect{false};
    bool gateAdapter{false};

    PlayerControlState controlState() {
        PlayerControlState state;
        state.muted = muted;
        state.volume = volume;
        state.menuOpen = nativeUiActive;
        state.isIso = session && session->isMvc();
        state.isoTitleMenuEnabled = state.isIso && !session->selectedTitleLabel().empty();
        state.captionsMenuEnabled = false;
        state.captionsLabel = L"Unavailable";
        state.layoutLabel = layout == LayoutChoice::Auto ? L"Layout: Auto"
            : layout == LayoutChoice::FullSbs ? L"Layout: Full SBS"
            : layout == LayoutChoice::HalfSbs ? L"Layout: Half SBS"
            : L"Layout: 2D";
        if (!currentPath.empty()) state.filename = std::filesystem::path(currentPath).filename().wstring();
        if (!hostError.empty()) state.error = hostError;
        else if (!displayError.empty()) state.error = displayError;
        else if (!captionError.empty()) state.error = captionError;
        if (!session) {
            state.status = L"Ready";
            state.audioMenuEnabled = false;
            return state;
        }
        state.positionSeconds = session->positionSeconds();
        state.durationSeconds = session->durationSeconds();
        const auto status = session->status();
        state.playing = status == PlaybackSession::Status::Running;
        state.status = status == PlaybackSession::Status::Opening ? L"Opening"
            : status == PlaybackSession::Status::Running ? L"Playing"
            : status == PlaybackSession::Status::Paused ? L"Paused"
            : status == PlaybackSession::Status::Drained ? L"Ended"
            : L"Error";
        if (status == PlaybackSession::Status::Failed) state.error = session->error();
        const std::wstring sessionCaptionError = session->subtitleError();
        if (state.error.empty() && !sessionCaptionError.empty()) state.error = sessionCaptionError;
        const auto tracks = session->audioTracks();
        state.audioMenuEnabled = !tracks.empty();
        for (const auto& track : tracks) {
            if (track.selected) state.audioLabel = track.label;
        }
        if (status != PlaybackSession::Status::Opening) {
            state.captionsMenuEnabled = true;
            state.captionsLabel = session->externalSubtitlesSelected() ? L"External" : L"Off";
            for (const auto& track : session->subtitleTracks()) {
                if (track.selected) state.captionsLabel = track.label;
            }
            const double offset = session->subtitleOffsetSeconds();
            if (std::abs(offset) >= 0.001) {
                wchar_t offsetText[32]{};
                swprintf_s(offsetText, L" %+0.1fs", offset);
                state.captionsLabel += offsetText;
            }
        }
        return state;
    }
};

AppShell::AppShell() {
    Win32Window::Callbacks cb;
    cb.onResize = [this](UINT w, UINT h) {
        if (m_device) m_device->resize(w, h);
        if (m_host) {
            m_host->viewsDirty = true;
            m_host->controlsDirty = true;
        }
        ++m_resizeCount;
    };
    cb.onToggleFullscreen = [this]() {
        if (!m_window) return;
        if (m_window->isFullscreen()) m_window->toggleBorderlessFullscreen();
        else if (m_host && m_host->weaver && validDisplayRect(m_host->weaver->displayRect())) {
            const RECT target = m_host->weaver->displayRect();
            m_window->toggleBorderlessFullscreen(MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &target);
        } else m_window->toggleBorderlessFullscreen();
    };
    cb.onQuit = [this]() {
        m_running = false;
        PostQuitMessage(0);
    };
    cb.onMouseMove = [this](int x, int y) {
        if (!m_host || !m_window) return;
        m_window->setCursorVisible(true);
        const PlayerAction action = m_host->controls.pointerMove(
            x, y, static_cast<int>(m_window->clientWidth()),
            static_cast<int>(m_window->clientHeight()), m_host->controlState(), GetTickCount64());
        m_host->controlsDirty = true;
        handlePlayerAction(static_cast<int>(action.command), action.value);
    };
    cb.onMouseLeave = [this]() {
        if (m_host) {
            m_host->controls.pointerLeave(GetTickCount64());
            m_host->controlsDirty = true;
        }
    };
    cb.onMouseDown = [this](Win32Window::MouseButton button, int x, int y) {
        if (!m_host || !m_window || button != Win32Window::MouseButton::Left) return;
        const PlayerAction action = m_host->controls.pointerDown(
            x, y, static_cast<int>(m_window->clientWidth()),
            static_cast<int>(m_window->clientHeight()), m_host->controlState(), GetTickCount64());
        m_host->controlsDirty = true;
        m_window->setMouseCapture(m_host->controls.captureNeeded());
        handlePlayerAction(static_cast<int>(action.command), action.value);
    };
    cb.onMouseUp = [this](Win32Window::MouseButton button, int x, int y) {
        if (!m_host || !m_window || button != Win32Window::MouseButton::Left) return;
        const PlayerAction action = m_host->controls.pointerUp(
            x, y, static_cast<int>(m_window->clientWidth()),
            static_cast<int>(m_window->clientHeight()), m_host->controlState(), GetTickCount64());
        m_host->controlsDirty = true;
        m_window->setMouseCapture(m_host->controls.captureNeeded());
        handlePlayerAction(static_cast<int>(action.command), action.value);
    };
    cb.onCaptureLost = [this]() {
        if (m_host) {
            m_host->controls.cancelInteraction();
            m_host->controlsDirty = true;
        }
    };
    cb.onFocusChanged = [this](bool focused) {
        if (m_host) {
            m_host->focused = focused;
            m_host->controlsDirty = true;
        }
    };
    cb.onMinimizedChanged = [this](bool minimized) {
        if (m_host) m_host->minimized = minimized;
    };
    cb.onFileDropped = [this](std::wstring path) { openMedia(path); };
    cb.onKeyDown = [this](UINT vk, bool ctrl, bool shift) -> bool {
        if (!m_host || !m_window) return false;
        if (m_host->nativeUiActive) return false;
        PlayerControlState state = m_host->controlState();
        state.fullscreen = m_window->isFullscreen();
        const std::uint64_t now = GetTickCount64();
        if (m_host->pendingSeekSeconds) {
            if (now - m_host->pendingSeekAt < 1000) {
                state.positionSeconds = *m_host->pendingSeekSeconds;
            } else {
                m_host->pendingSeekSeconds.reset();
            }
        }
        m_host->controls.keyboardActivity(now);
        m_host->controlsDirty = true;
        const PlayerAction action = PlayerControls::keyAction(vk, ctrl, shift, state);
        handlePlayerAction(static_cast<int>(action.command), action.value);
        return action.command != PlayerCommand::None;
    };
    cb.onMouseWheel = [this](int notches) {
        if (!m_host || !m_window) return;
        if (m_host->nativeUiActive) return;
        const double newVolume = std::clamp(m_host->volume + 0.05 * notches, 0.0, 1.0);
        const std::uint64_t now = GetTickCount64();
        m_host->controls.keyboardActivity(now);
        m_host->controlsDirty = true;
        handlePlayerAction(static_cast<int>(PlayerCommand::SetVolume), newVolume);
    };
    cb.onMouseDoubleClick = [this](int x, int y) {
        if (!m_host || !m_window) return;
        if (m_host->nativeUiActive) return;
        m_host->controlsDirty = true;
        if (m_host->controls.panelContains(
                x, y, static_cast<int>(m_window->clientWidth()), static_cast<int>(m_window->clientHeight()))) {
            return;
        }
        handlePlayerAction(static_cast<int>(PlayerCommand::Fullscreen), 0.0);
    };

    m_window = std::make_unique<Win32Window>(L"Odyssey Player 3D", 1280, 720, std::move(cb));
    m_device = std::make_unique<D3D11Device>(m_window->hwnd(), 1280u, 720u);
}

AppShell::~AppShell() {
    if (m_window) m_window->clearCallbacks();
    m_host.reset();
}

int AppShell::run() {
    if (!m_host) initializePersistentHost();
    while (m_running) {
        if (!m_window->pumpMessages()) break;
        renderPersistentFrame();
    }
    return 0;
}

void AppShell::initializePersistentHost() {
    m_host = std::make_unique<Host>();
    try {
        m_host->weaver = std::make_unique<ImmersityWeaver>(
            m_device->device(), m_device->context(), m_window->hwnd());
    } catch (const std::exception& exception) {
        m_host->displayError = L"3D display: " + std::wstring(
            exception.what(), exception.what() + std::strlen(exception.what()));
        m_host->weaver.reset();
    }
    retargetDeviceForDisplay();
    m_host->compositor = std::make_unique<StereoCompositor>(m_device->device());
}

void AppShell::retargetDeviceForDisplay() {
    if (!m_host || !m_host->weaver) return;
    const RECT display = m_host->weaver->displayRect();
    if (!validDisplayRect(display)) return;
    const HMONITOR monitor = MonitorFromRect(&display, MONITOR_DEFAULTTONEAREST);
    if (m_device->currentAdapterOwnsMonitor(monitor)) return;

    m_host->weaver->setDesired3D(false);
    m_host->weaver.reset();
    m_host->compositor.reset();
    m_host->converter.reset();
    m_host->renderedSubtitles.clear();
    m_host->renderedSubtitleCanvasWidth = 0;
    const UINT width = m_window->clientWidth();
    const UINT height = m_window->clientHeight();
    m_device.reset();
    try {
        m_device = std::make_unique<D3D11Device>(m_window->hwnd(), width, height, monitor);
        m_host->weaver = std::make_unique<ImmersityWeaver>(
            m_device->device(), m_device->context(), m_window->hwnd());
        m_host->displayError.clear();
    } catch (const std::exception& exception) {
        m_device.reset();
        m_device = std::make_unique<D3D11Device>(m_window->hwnd(), width, height);
        m_host->displayError = L"3D display: " + std::wstring(
            exception.what(), exception.what() + std::strlen(exception.what()));
    }
}

// Exercises the M0 success criteria without user input. Exit code 0 = pass.
// Codes distinguish which check failed to keep CTest output actionable.
int AppShell::runSmokeTest() {
    constexpr int kFrames = 60;

    for (int i = 0; i < kFrames; ++i) {
        if (!m_window->pumpMessages()) return 10;
        HRESULT hr = m_device->clearAndPresent(kClearColor);
        if (FAILED(hr)) return 11;
    }

    const unsigned resizeBefore = m_resizeCount;
    m_window->toggleBorderlessFullscreen();
    for (int i = 0; i < 5; ++i) {
        if (!m_window->pumpMessages()) return 12;
        if (FAILED(m_device->clearAndPresent(kClearColor))) return 13;
    }
    if (m_resizeCount == resizeBefore) return 14;

    const unsigned resizeAfterFs = m_resizeCount;
    m_window->toggleBorderlessFullscreen();
    for (int i = 0; i < 5; ++i) {
        if (!m_window->pumpMessages()) return 15;
        if (FAILED(m_device->clearAndPresent(kClearColor))) return 16;
    }
    if (m_resizeCount == resizeAfterFs) return 17;

    PostMessageW(m_window->hwnd(), WM_CLOSE, 0, 0);
    while (m_window->pumpMessages() && m_running) {
        m_device->clearAndPresent(kClearColor);
    }

    int live = m_device->teardownAndProbe();
    if (live > 0) return 20 + (live > 9 ? 9 : live);

    return 0;
}

// ---------------------------------------------------------------------------
// M1 spike helpers
// ---------------------------------------------------------------------------

namespace {

struct LoadedTexture {
    ID3D11Texture2D*         tex{nullptr};
    ID3D11ShaderResourceView* srv{nullptr};
    UINT width{0};
    UINT height{0};

    ~LoadedTexture() {
        if (srv) srv->Release();
        if (tex) tex->Release();
    }
};

// Loads a PNG via WIC into an ID3D11Texture2D. The texture is created as
// R8G8B8A8_TYPELESS so the SRV can be R8G8B8A8_UNORM_SRGB Ã¢â‚¬â€ DX11 only allows
// the UNORM/UNORM_SRGB view cast over a TYPELESS backing resource. The SRV
// format drives sampling, letting the weaver read in linear space.
static void loadPngToTexture(ID3D11Device* device, const std::wstring& path, LoadedTexture& out) {
    IWICImagingFactory* factory{};
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) throw std::runtime_error("WIC: CoCreateInstance failed");

    IWICBitmapDecoder* decoder{};
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) { factory->Release(); throw std::runtime_error("WIC: CreateDecoderFromFilename failed"); }

    IWICBitmapFrameDecode* frame{};
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { decoder->Release(); factory->Release(); throw std::runtime_error("WIC: GetFrame failed"); }

    IWICFormatConverter* conv{};
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr)) { frame->Release(); decoder->Release(); factory->Release(); throw std::runtime_error("WIC: CreateFormatConverter failed"); }

    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                          nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { conv->Release(); frame->Release(); decoder->Release(); factory->Release(); throw std::runtime_error("WIC: converter Initialize failed"); }

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    const UINT rowPitch = w * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(rowPitch) * h);
    hr = conv->CopyPixels(nullptr, rowPitch, static_cast<UINT>(pixels.size()), pixels.data());
    conv->Release(); frame->Release(); decoder->Release(); factory->Release();
    if (FAILED(hr)) throw std::runtime_error("WIC: CopyPixels failed");

    D3D11_TEXTURE2D_DESC td{};
    td.Width          = w;
    td.Height         = h;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc     = {1, 0};
    td.Usage          = D3D11_USAGE_IMMUTABLE;
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem     = pixels.data();
    sd.SysMemPitch = rowPitch;

    hr = device->CreateTexture2D(&td, &sd, &out.tex);
    if (FAILED(hr)) throw std::runtime_error("CreateTexture2D failed");

    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format              = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(out.tex, &sv, &out.srv);
    if (FAILED(hr)) throw std::runtime_error("CreateShaderResourceView failed");

    out.width  = w;
    out.height = h;
}

struct ComScope {
    bool needsUninit{false};
    ComScope() {
        // STA is fine for single-threaded WIC use. If something else already
        // initialized the apartment with a different model, we tolerate it.
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        needsUninit = SUCCEEDED(hr);
    }
    ~ComScope() { if (needsUninit) CoUninitialize(); }
};

#ifdef _DEBUG
// Returns the count of non-empty stored D3D11 messages, or -1 if unavailable.
static int d3dValidationErrorCount(ID3D11Device* dev) {
    ID3D11InfoQueue* iq{};
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&iq))) return -1;
    const UINT64 n = iq->GetNumStoredMessagesAllowedByRetrievalFilter();
    iq->Release();
    return static_cast<int>(n);
}
#endif

static bool native3DEligible(const Win32Window& window, const RECT& displayRect) {
    RECT windowRect{};
    RECT clientRect{};
    const HWND foreground = GetForegroundWindow();
    const bool foregroundEligible = foreground == window.hwnd()
        || (!foreground && GetActiveWindow() == window.hwnd()
            && GetFocus() == window.hwnd());
    const LONG nativeWidth = displayRect.right - displayRect.left;
    const LONG nativeHeight = displayRect.bottom - displayRect.top;
    return window.isFullscreen() && !IsIconic(window.hwnd())
        && foregroundEligible
        && GetWindowRect(window.hwnd(), &windowRect)
        && EqualRect(&windowRect, &displayRect)
        && GetClientRect(window.hwnd(), &clientRect)
        && clientRect.right - clientRect.left == nativeWidth
        && clientRect.bottom - clientRect.top == nativeHeight;
}

static bool prepareNativeSmokeWindow(Win32Window& window, const RECT& displayRect) {
    if (!window.isFullscreen()) {
        window.toggleBorderlessFullscreen(nullptr, &displayRect);
    }
    BringWindowToTop(window.hwnd());
    SetForegroundWindow(window.hwnd());
    SetActiveWindow(window.hwnd());
    SetFocus(window.hwnd());
    constexpr auto foregroundWait = std::chrono::seconds(8);
    const auto deadline = std::chrono::steady_clock::now() + foregroundWait;
    bool eligible = native3DEligible(window, displayRect);
    while (!eligible && std::chrono::steady_clock::now() < deadline) {
        if (!window.pumpMessages()) return false;
        if (MsgWaitForMultipleObjectsEx(
                0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED) {
            break;
        }
        eligible = native3DEligible(window, displayRect);
    }
    if (!eligible) {
        RECT actual{};
        GetWindowRect(window.hwnd(), &actual);
        std::fprintf(
            stderr,
            "Native 3D eligibility failed: fullscreen=%d iconic=%d foreground=%d "
            "window=(%ld,%ld,%ld,%ld) display=(%ld,%ld,%ld,%ld)\n",
            window.isFullscreen() ? 1 : 0, IsIconic(window.hwnd()) ? 1 : 0,
            (GetForegroundWindow() == window.hwnd()
             || (!GetForegroundWindow() && GetActiveWindow() == window.hwnd()
                 && GetFocus() == window.hwnd())) ? 1 : 0,
            actual.left, actual.top, actual.right, actual.bottom,
            displayRect.left, displayRect.top, displayRect.right, displayRect.bottom);
    }
    return eligible;
}

} // namespace

void AppShell::openMedia(const std::wstring& path, const std::wstring& playlistPath,
                         bool startPaused) {
    if (!m_host || path.empty()) return;
    m_host->retiredSessions.erase(
        std::remove_if(m_host->retiredSessions.begin(), m_host->retiredSessions.end(),
            [](std::future<void>& retired) {
                return retired.wait_for(std::chrono::milliseconds(0)) ==
                       std::future_status::ready;
            }),
        m_host->retiredSessions.end());
    if (m_host->retiredSessions.size() >= 2) {
        m_host->hostError = L"The previous movie is still closing. Try again in a moment.";
        m_host->controlsDirty = true;
        return;
    }
    m_host->controls.cancelInteraction();
    m_window->setMouseCapture(false);
    m_host->pendingFrame.reset();
    m_host->retainedFrame.reset();
    if (m_host->weaver) m_host->weaver->setDesired3D(false);
    m_device->clearAndPresent(kClearColor);
    if (m_host->session) {
        m_host->retiredSessions.push_back(std::async(
            std::launch::async,
            [session = std::move(m_host->session)]() mutable { session.reset(); }));
    }
    if (m_host->weaver && m_host->weaver->status() == ImmersityWeaver::Status::Unavailable) {
        m_host->weaver->tryReconnect();
    }
    try {
        retargetDeviceForDisplay();
        if (!m_host->compositor) {
            m_host->compositor = std::make_unique<StereoCompositor>(m_device->device());
        }
    } catch (const std::exception& exception) {
        m_host->hostError.assign(exception.what(),
                                 exception.what() + std::strlen(exception.what()));
        m_host->controlsDirty = true;
        return;
    }
    m_host->converter.reset();
    m_host->renderedSubtitles.clear();
    m_host->renderedSubtitleCanvasWidth = 0;
    m_host->haveViews = false;
    m_host->viewsDirty = false;
    m_host->generation = 0;
    m_host->hostError.clear();
    m_host->currentPath = path;
    m_host->controlsDirty = true;

    if (!m_window->isFullscreen()) {
        if (m_host->weaver && validDisplayRect(m_host->weaver->displayRect())) {
            const RECT target = m_host->weaver->displayRect();
            m_window->toggleBorderlessFullscreen(
                MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &target);
        } else {
            m_window->toggleBorderlessFullscreen();
        }
    }
    try {
        m_host->session = std::make_unique<PlaybackSession>(
            m_device->device(), path, executableLavDirectory(), startPaused,
            m_host->muted, m_host->volume, playlistPath);
    } catch (const std::exception& exception) {
        m_host->hostError.assign(exception.what(), exception.what() + std::strlen(exception.what()));
    }
}

void AppShell::showIsoTitleMenu() {
    if (!m_host || !m_host->session || !m_host->session->isMvc()) return;
    if (m_host->session->selectedTitleLabel().empty()) return;
    const bool wasPlaying = m_host->session->status() == PlaybackSession::Status::Running;
    const auto scan = m_host->session->titleSnapshot();
    const std::wstring selectedPath = m_host->session->selectedTitleRelativePlaylistPath();
    setNativeUiActive(true, wasPlaying);
    HMENU menu = CreatePopupMenu();
    UINT selectedCommand = 0;
    if (menu) {
        constexpr UINT kFirstTitle = 5000;
        if (scan.titles.empty()) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, kFirstTitle,
                        L"Automatic disc title");
        } else {
            for (size_t index = 0; index < scan.titles.size(); ++index) {
                const auto& title = scan.titles[index];
                wchar_t duration[32]{};
                swprintf_s(duration, L" [%02d:%02d:%02d]",
                           static_cast<int>(title.durationSeconds / 3600.0),
                           static_cast<int>(title.durationSeconds / 60.0) % 60,
                           static_cast<int>(title.durationSeconds) % 60);
                std::wstring label = title.label + duration;
                const bool selected = !_wcsicmp(title.relativePlaylistPath.c_str(),
                                                selectedPath.c_str());
                AppendMenuW(menu, MF_STRING | (selected ? MF_CHECKED : 0),
                            kFirstTitle + static_cast<UINT>(index), label.c_str());
            }
        }
        POINT point{};
        GetCursorPos(&point);
        selectedCommand = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                         point.x, point.y, 0, m_window->hwnd(), nullptr);
        DestroyMenu(menu);
        if (selectedCommand >= kFirstTitle &&
            selectedCommand < kFirstTitle + scan.titles.size()) {
            const auto& title = scan.titles[selectedCommand - kFirstTitle];
            openMedia(m_host->currentPath, title.relativePlaylistPath, !wasPlaying);
        }
    }
    setNativeUiActive(false, wasPlaying);
}

void AppShell::setNativeUiActive(bool active, bool restorePlayback) {
    if (!m_host) return;
    m_host->nativeUiActive = active;
    m_host->controlsDirty = true;
    if (active) {
        if (m_host->weaver) m_host->weaver->setDesired3D(false);
        if (m_host->session && restorePlayback) m_host->session->pause();
        renderPersistentFrame();
    } else if (m_host->session && restorePlayback) {
        m_host->session->resume();
    }
}

void AppShell::showOpenDialog() {
    if (!m_host) return;
    const bool wasPlaying = m_host->session &&
        m_host->session->status() == PlaybackSession::Status::Running;
    setNativeUiActive(true, wasPlaying);
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    std::wstring selected;
    if (SUCCEEDED(hr)) {
        const COMDLG_FILTERSPEC filters[] = {
            {L"Movies and Blu-ray images", L"*.mkv;*.mp4;*.m2ts;*.iso"},
            {L"All files", L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"Open Movie");
        hr = dialog->Show(m_window->hwnd());
        if (SUCCEEDED(hr)) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    selected = path;
                    CoTaskMemFree(path);
                }
            }
        }
    }
    setNativeUiActive(false, selected.empty() && wasPlaying);
    if (!selected.empty()) openMedia(selected);
}

void AppShell::showAudioMenu() {
    if (!m_host || !m_host->session) return;
    const auto tracks = m_host->session->audioTracks();
    if (tracks.empty()) return;
    const bool wasPlaying = m_host->session->status() == PlaybackSession::Status::Running;
    setNativeUiActive(true, wasPlaying);
    HMENU menu = CreatePopupMenu();
    if (menu) {
        constexpr UINT kFirstTrack = 1000;
        for (size_t index = 0; index < tracks.size(); ++index) {
            AppendMenuW(menu, MF_STRING | (tracks[index].selected ? MF_CHECKED : 0),
                        kFirstTrack + static_cast<UINT>(index), tracks[index].label.c_str());
        }
        POINT point{};
        GetCursorPos(&point);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                            point.x, point.y, 0, m_window->hwnd(), nullptr);
        DestroyMenu(menu);
        if (command >= kFirstTrack && command < kFirstTrack + tracks.size()) {
            m_host->session->selectAudioTrack(tracks[command - kFirstTrack].id);
            m_host->pendingFrame.reset();
            m_host->retainedFrame.reset();
            m_host->haveViews = false;
            m_host->renderedSubtitles.clear();
            m_host->renderedSubtitleCanvasWidth = 0;
        }
    }
    setNativeUiActive(false, wasPlaying);
}

void AppShell::showCaptionsMenu() {
    if (!m_host || !m_host->session) return;
    const auto tracks = m_host->session->subtitleTracks();
    const bool wasPlaying = m_host->session->status() == PlaybackSession::Status::Running;
    setNativeUiActive(true, wasPlaying);
    HMENU menu = CreatePopupMenu();
    UINT command = 0;
    if (menu) {
        constexpr UINT kOff = 3000;
        constexpr UINT kFirstTrack = 3001;
        constexpr UINT kExternal = 3999;
        constexpr UINT kOffsetEarlier = 4000;
        constexpr UINT kOffsetReset = 4001;
        constexpr UINT kOffsetLater = 4002;
        const bool external = m_host->session->externalSubtitlesSelected();
        const bool embedded = m_host->session->selectedSubtitleTrack().has_value();
        AppendMenuW(menu, MF_STRING | (!external && !embedded ? MF_CHECKED : 0),
                    kOff, L"Off");
        for (size_t index = 0; index < tracks.size(); ++index) {
            const UINT flags = MF_STRING |
                (tracks[index].selected ? MF_CHECKED : 0) |
                (tracks[index].supported ? 0 : MF_GRAYED);
            AppendMenuW(menu, flags, kFirstTrack + static_cast<UINT>(index),
                        tracks[index].label.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (external ? MF_CHECKED : 0),
                    kExternal, L"Load SubRip file...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kOffsetEarlier, L"Offset -0.5 seconds");
        AppendMenuW(menu, MF_STRING, kOffsetReset, L"Reset caption offset");
        AppendMenuW(menu, MF_STRING, kOffsetLater, L"Offset +0.5 seconds");
        POINT point{};
        GetCursorPos(&point);
        command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                 point.x, point.y, 0, m_window->hwnd(), nullptr);
        DestroyMenu(menu);
        if (command == kOff) {
            m_host->session->disableSubtitles();
        } else if (command >= kFirstTrack && command < kFirstTrack + tracks.size()) {
            m_host->session->selectSubtitleTrack(
                tracks[command - kFirstTrack].id);
        } else if (command == kExternal) {
            Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
            HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
            if (SUCCEEDED(hr)) {
                const COMDLG_FILTERSPEC filters[] = {
                    {L"SubRip captions", L"*.srt"}, {L"All files", L"*.*"},
                };
                dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
                dialog->SetTitle(L"Load Captions");
                hr = dialog->Show(m_window->hwnd());
                if (SUCCEEDED(hr)) {
                    Microsoft::WRL::ComPtr<IShellItem> item;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item)) &&
                        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        std::wstring error;
                        if (!m_host->session->loadExternalSubRip(path, error)) {
                            m_host->hostError = error;
                        }
                        CoTaskMemFree(path);
                    }
                }
            }
        } else if (command == kOffsetEarlier || command == kOffsetReset || command == kOffsetLater) {
            double offset = m_host->session->subtitleOffsetSeconds();
            if (command == kOffsetEarlier) offset -= 0.5;
            if (command == kOffsetLater) offset += 0.5;
            if (m_host->session->setSubtitleOffsetSeconds(offset)) {
                m_host->renderedSubtitles.clear();
                m_host->renderedSubtitleSignature = 0;
            }
        }
    }
    m_host->renderedSubtitles.clear();
    m_host->renderedSubtitleSignature = 0;
    m_host->controlsDirty = true;
    setNativeUiActive(false, wasPlaying);
}

void AppShell::showLayoutMenu() {
    if (!m_host) return;
    const bool wasPlaying = m_host->session &&
        m_host->session->status() == PlaybackSession::Status::Running;
    setNativeUiActive(true, wasPlaying);
    HMENU menu = CreatePopupMenu();
    if (menu) {
        constexpr UINT kAuto = 2000;
        const wchar_t* labels[] = {L"Auto", L"Full SBS", L"Half SBS", L"2D"};
        for (UINT index = 0; index < std::size(labels); ++index) {
            const bool selected = static_cast<UINT>(m_host->layout) == index;
            AppendMenuW(menu, MF_STRING | (selected ? MF_CHECKED : 0),
                        kAuto + index, labels[index]);
        }
        POINT point{};
        GetCursorPos(&point);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                            point.x, point.y, 0, m_window->hwnd(), nullptr);
        DestroyMenu(menu);
        if (command >= kAuto && command < kAuto + std::size(labels)) {
            m_host->layout = static_cast<Host::LayoutChoice>(command - kAuto);
            m_host->viewsDirty = true;
        }
    }
    setNativeUiActive(false, wasPlaying);
}

void AppShell::handlePlayerAction(int rawCommand, double value) {
    if (!m_host || rawCommand == static_cast<int>(PlayerCommand::None)) return;
    const PlayerCommand command = static_cast<PlayerCommand>(rawCommand);
    m_host->controlsDirty = true;
    switch (command) {
    case PlayerCommand::Open: showOpenDialog(); break;
    case PlayerCommand::PlayPause:
        if (m_host->session) {
            if (m_host->session->status() == PlaybackSession::Status::Running) {
                m_host->session->pause();
            } else {
                if (m_host->session->status() == PlaybackSession::Status::Drained) {
                    m_host->session->seek(0.0);
                }
                m_host->session->resume();
            }
        }
        break;
    case PlayerCommand::Stop:
        if (m_host->session) {
            m_host->session->pause();
            m_host->session->seek(0.0);
            m_host->session->clearFrames();
            m_host->pendingFrame.reset();
            m_host->retainedFrame.reset();
            m_host->haveViews = false;
        }
        break;
    case PlayerCommand::ToggleMute:
        m_host->muted = !m_host->muted;
        if (m_host->session) m_host->session->setMuted(m_host->muted);
        break;
    case PlayerCommand::AudioMenu: showAudioMenu(); break;
    case PlayerCommand::CaptionsMenu: showCaptionsMenu(); break;
    case PlayerCommand::LayoutMenu: showLayoutMenu(); break;
    case PlayerCommand::SwapEyes:
        m_host->swapEyes = !m_host->swapEyes;
        m_host->viewsDirty = true;
        break;
    case PlayerCommand::Fullscreen:
        if (m_window->isFullscreen()) m_window->toggleBorderlessFullscreen();
        else if (m_host->weaver && validDisplayRect(m_host->weaver->displayRect())) {
            const RECT target = m_host->weaver->displayRect();
            m_window->toggleBorderlessFullscreen(
                MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &target);
        } else m_window->toggleBorderlessFullscreen();
        break;
    case PlayerCommand::Minimize: m_window->minimize(); break;
    case PlayerCommand::Close:
        if (m_host->weaver) m_host->weaver->setDesired3D(false);
        m_running = false;
        PostQuitMessage(0);
        break;
    case PlayerCommand::Seek:
        if (m_host->session && m_host->session->seek(value)) {
            m_host->session->clearFrames();
            m_host->pendingFrame.reset();
            m_host->retainedFrame.reset();
            m_host->haveViews = false;
            m_host->pendingSeekSeconds = value;
            m_host->pendingSeekAt = GetTickCount64();
        }
        break;
    case PlayerCommand::SetVolume:
        if (std::isfinite(value) && value >= 0.0 && value <= 1.0) {
            if (value > m_host->volume && m_host->muted) {
                m_host->muted = false;
                if (m_host->session) m_host->session->setMuted(false);
            }
            m_host->volume = static_cast<float>(value);
            if (m_host->session) m_host->session->setVolume(m_host->volume);
        }
        break;
    case PlayerCommand::IsoTitleMenu:
        showIsoTitleMenu();
        break;
    case PlayerCommand::None:
        break;
    }
}

void AppShell::renderPersistentFrame() {
    if (!m_host || !m_host->compositor) {
        m_device->clearAndPresent(kClearColor);
        return;
    }

    m_host->retiredSessions.erase(
        std::remove_if(m_host->retiredSessions.begin(), m_host->retiredSessions.end(),
            [](std::future<void>& retired) {
                return retired.wait_for(std::chrono::milliseconds(0)) ==
                       std::future_status::ready;
            }),
        m_host->retiredSessions.end());

    if (m_host->session) {
        const uint64_t currentGeneration = m_host->session->generation();
        if (currentGeneration != m_host->generation) {
            m_host->generation = currentGeneration;
            m_host->pendingFrame.reset();
            m_host->retainedFrame.reset();
            m_host->haveViews = false;
            m_host->viewsDirty = false;
        }
        if (m_host->session->status() == PlaybackSession::Status::Failed) {
            m_host->hostError = m_host->session->error();
        }
    }

    for (int attempts = 0; m_host->session && attempts < 16; ++attempts) {
        if (!m_host->pendingFrame) m_host->pendingFrame.reset(m_host->session->pollFrame());
        if (!m_host->pendingFrame) break;

        const int numerator = m_host->session->timebaseNum();
        const int denominator = m_host->session->timebaseDen();
        VideoTimingDecision timing = VideoTimingDecision::Present;
        std::optional<int64_t> framePtsNs;
        std::optional<int64_t> frameDurationNs;
        if (m_host->pendingFrame->pts != AV_NOPTS_VALUE && numerator > 0 && denominator > 0) {
            const AVRational sourceTimeBase{numerator, denominator};
            const AVRational nanoseconds{1, 1'000'000'000};
            const int64_t ptsNs = av_rescale_q(m_host->pendingFrame->pts,
                                               sourceTimeBase, nanoseconds);
            framePtsNs = ptsNs;
            std::optional<int64_t> durationNs;
            if (m_host->pendingFrame->duration > 0) {
                durationNs = av_rescale_q(m_host->pendingFrame->duration,
                                          sourceTimeBase, nanoseconds);
                frameDurationNs = durationNs;
            }
            timing = decideVideoTiming(ptsNs, durationNs,
                                       m_host->session->mediaTimeNanoseconds());
        }
        if (timing == VideoTimingDecision::Wait) {
            ++m_host->scheduledWaited;
            break;
        }
        if (timing == VideoTimingDecision::Drop) {
            ++m_host->scheduledDropped;
            m_host->pendingFrame.reset();
            continue;
        }

        const FramePacking packing = automaticFramePacking(
            m_host->pendingFrame.get(), m_host->currentPath, m_host->session->isMvc());
        if (!packing.supported && m_host->layout == Host::LayoutChoice::Auto) {
            m_host->hostError = L"This stereo packing is not supported. Choose a side-by-side layout.";
            m_host->session->pause();
            m_host->pendingFrame.reset();
            m_host->retainedFrame.reset();
            m_host->haveViews = false;
            break;
        }
        m_host->retainedMode = packing.mode;
        m_host->retainedMetadataInvert = packing.invertEyes;
        m_host->retainedSourceWidth = static_cast<UINT>(m_host->pendingFrame->width);
        m_host->retainedSourceHeight = static_cast<UINT>(m_host->pendingFrame->height);
        m_host->retainedPtsNs = framePtsNs;
        m_host->retainedDurationNs = frameDurationNs;
        if (frameDurationNs && *frameDurationNs > 0 && !m_host->nominalFrameDurationNs) {
            m_host->nominalFrameDurationNs = frameDurationNs;
        }
        m_host->unrecordedPresentation = true;
        m_host->retainedFrame = std::move(m_host->pendingFrame);
        m_host->viewsDirty = true;
        break;
    }

    const UINT clientWidth = m_window->clientWidth();
    const UINT clientHeight = m_window->clientHeight();
    if (clientWidth == 0 || clientHeight == 0 || m_host->minimized) {
        if (m_host->weaver) m_host->weaver->setDesired3D(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return;
    }

    if (m_host->weaver && m_host->weaver->status() == ImmersityWeaver::Status::Unavailable) {
        m_host->weaver->tryReconnect();
    }

    StereoMode mode = m_host->retainedMode;
    if (m_host->layout == Host::LayoutChoice::Mono2D) mode = StereoMode::Mono2D;
    else if (m_host->session && m_host->session->isMvc()) mode = StereoMode::FullSbs;
    else if (m_host->layout == Host::LayoutChoice::FullSbs) mode = StereoMode::FullSbs;
    else if (m_host->layout == Host::LayoutChoice::HalfSbs) mode = StereoMode::HalfSbs;

    const RECT targetDisplay = m_host->weaver ? m_host->weaver->displayRect() : RECT{};
    const HMONITOR targetMonitor = validDisplayRect(targetDisplay)
        ? MonitorFromRect(&targetDisplay, MONITOR_DEFAULTTONEAREST) : nullptr;
    m_host->gateValidDisplay = validDisplayRect(targetDisplay);
    m_host->gateHasFrame = m_host->retainedFrame != nullptr;
    m_host->gateStereo = mode != StereoMode::Mono2D;
    m_host->gateSdkActive = m_host->weaver &&
        m_host->weaver->status() == ImmersityWeaver::Status::Active;
    m_host->gateNativeUiClear = !m_host->nativeUiActive;
    m_host->gateFocused = m_host->focused;
    m_host->gateNativeRect = m_host->weaver &&
        native3DEligible(*m_window, targetDisplay);
    m_host->gateAdapter = m_device->currentAdapterOwnsMonitor(targetMonitor);
    const bool candidate3D = m_host->weaver && m_host->gateHasFrame &&
        m_host->gateStereo && m_host->gateValidDisplay && m_host->gateSdkActive &&
        m_host->gateNativeUiClear && m_host->gateFocused && m_host->gateNativeRect &&
        m_host->gateAdapter;
    if (m_host->weaver) {
        m_host->weaver->setDesired3D(candidate3D);
        m_host->weaver->frameBegin();
    }
    const bool readyFor3D = candidate3D && m_host->weaver->lensEnabled();

    PlayerControlState controlsState = m_host->controlState();
    controlsState.fullscreen = m_window->isFullscreen();
    if (!readyFor3D) {
        controlsState.forceVisible = true;
        if (controlsState.error.empty()) {
            controlsState.status = L"2D Preview - " + controlsState.status;
        }
    }
    const std::uint64_t nowMilliseconds = GetTickCount64();
    if (m_host->controlsDirty || nowMilliseconds < m_host->controlsRenderedAt ||
        nowMilliseconds - m_host->controlsRenderedAt >= 100) {
        m_host->controlsSurface = m_host->controls.render(
            controlsState, static_cast<int>(clientWidth), static_cast<int>(clientHeight),
            nowMilliseconds);
        m_host->controlsRenderedAt = nowMilliseconds;
        m_host->controlsDirty = false;
    }
    const PlayerControlSurface& controlsSurface = m_host->controlsSurface;
    m_window->setCursorVisible(controlsSurface.visible || !controlsState.playing);

    auto prepareSubtitles = [&](UINT aspectWidth, UINT aspectHeight) {
        if (!m_host->retainedFrame || clientWidth == 0 || clientHeight == 0 ||
            aspectWidth == 0 || aspectHeight == 0) {
            m_host->renderedSubtitles.clear();
            m_host->renderedSubtitleSignature = 0;
            m_host->renderedSubtitleTextSafeBottom.reset();
            m_host->captionError.clear();
            return;
        }
        const AVRational sar = m_host->retainedFrame->sample_aspect_ratio;
        const int sarNumerator = sar.num > 0 && sar.den > 0 ? sar.num : 1;
        const int sarDenominator = sar.num > 0 && sar.den > 0 ? sar.den : 1;
        const auto layout = makeStereoLayout(
            mode, m_host->retainedFrame->width, m_host->retainedFrame->height,
            sarNumerator, sarDenominator, static_cast<int>(aspectWidth),
            static_cast<int>(aspectHeight),
            m_host->swapEyes != m_host->retainedMetadataInvert);
        if (!layout || layout->left.destination.right <= layout->left.destination.left ||
            layout->left.destination.bottom <= layout->left.destination.top) {
            m_host->renderedSubtitles.clear();
            m_host->renderedSubtitleSignature = 0;
            m_host->renderedSubtitleTextSafeBottom.reset();
            m_host->captionError.clear();
            return;
        }
        m_host->retainedLayout = *layout;
        const RECT movieRect{
            static_cast<LONG>(std::lround(layout->left.destination.left * clientWidth)),
            static_cast<LONG>(std::lround(layout->left.destination.top * clientHeight)),
            static_cast<LONG>(std::lround(layout->left.destination.right * clientWidth)),
            static_cast<LONG>(std::lround(layout->left.destination.bottom * clientHeight))};
        if (movieRect.right <= movieRect.left || movieRect.bottom <= movieRect.top) {
            m_host->renderedSubtitles.clear();
            m_host->renderedSubtitleSignature = 0;
            m_host->renderedSubtitleTextSafeBottom.reset();
            m_host->captionError.clear();
            return;
        }
        const auto activeCues = m_host->session
            ? m_host->session->subtitleCuesAt(m_host->session->mediaTimeNanoseconds())
            : std::vector<SubtitleCue>{};
        if (activeCues.empty()) {
            m_host->renderedSubtitles.clear();
            m_host->renderedSubtitleSignature = 0;
            m_host->renderedSubtitleTextSafeBottom.reset();
            m_host->captionError.clear();
            return;
        }
        const std::uint64_t cueSignature = subtitleSignature(activeCues);
        const std::optional<int> textSafeBottom = controlsSurface.visible
            ? std::optional<int>(controlsSurface.top - 8) : std::nullopt;
        if (cueSignature == m_host->renderedSubtitleSignature &&
            EqualRect(&movieRect, &m_host->renderedSubtitleMovieRect) &&
            clientWidth == m_host->renderedSubtitleCanvasWidth &&
            clientHeight == m_host->renderedSubtitleCanvasHeight &&
            textSafeBottom == m_host->renderedSubtitleTextSafeBottom) {
            return;
        }
        try {
            m_host->renderedSubtitles = m_host->subtitleRenderer.render(
                activeCues, movieRect, clientWidth, clientHeight, textSafeBottom);
            m_host->renderedSubtitleSignature = cueSignature;
            m_host->renderedSubtitleMovieRect = movieRect;
            m_host->renderedSubtitleCanvasWidth = clientWidth;
            m_host->renderedSubtitleCanvasHeight = clientHeight;
            m_host->renderedSubtitleTextSafeBottom = textSafeBottom;
            m_host->captionError.clear();
        } catch (const std::exception& exception) {
            m_host->renderedSubtitles.clear();
            m_host->renderedSubtitleSignature = cueSignature;
            m_host->renderedSubtitleMovieRect = movieRect;
            m_host->renderedSubtitleCanvasWidth = clientWidth;
            m_host->renderedSubtitleCanvasHeight = clientHeight;
            m_host->renderedSubtitleTextSafeBottom = textSafeBottom;
            m_host->captionError = L"Captions disabled: ";
            const int length = MultiByteToWideChar(CP_UTF8, 0, exception.what(),
                                                   static_cast<int>(std::strlen(exception.what())),
                                                   nullptr, 0);
            if (length > 0) {
                std::wstring message(static_cast<size_t>(length), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, exception.what(),
                                    static_cast<int>(std::strlen(exception.what())),
                                    message.data(), length);
                m_host->captionError += message;
            }
        }
    };

    const UINT initialAspectWidth = readyFor3D
        ? static_cast<UINT>(targetDisplay.right - targetDisplay.left) : clientWidth;
    const UINT initialAspectHeight = readyFor3D
        ? static_cast<UINT>(targetDisplay.bottom - targetDisplay.top) : clientHeight;
    prepareSubtitles(initialAspectWidth, initialAspectHeight);

    auto composeScene = [&](UINT viewWidth, UINT viewHeight,
                            UINT aspectWidth, UINT aspectHeight) {
        if (m_host->retainedFrame) {
            const AVRational sar = m_host->retainedFrame->sample_aspect_ratio;
            const int sarNumerator = sar.num > 0 && sar.den > 0 ? sar.num : 1;
            const int sarDenominator = sar.num > 0 && sar.den > 0 ? sar.den : 1;
            const auto layout = makeStereoLayout(
                mode, m_host->retainedFrame->width, m_host->retainedFrame->height,
                sarNumerator, sarDenominator, static_cast<int>(aspectWidth),
                static_cast<int>(aspectHeight),
                m_host->swapEyes != m_host->retainedMetadataInvert);
            if (!layout) throw std::runtime_error("This movie has invalid stereo dimensions");
            if (!m_host->converter ||
                m_host->converter->width() != m_host->retainedSourceWidth ||
                m_host->converter->height() != m_host->retainedSourceHeight) {
                m_host->converter = std::make_unique<Nv12ToRgba>(
                    m_device->device(), m_host->retainedSourceWidth,
                    m_host->retainedSourceHeight);
                m_host->viewsDirty = true;
            }
            if (m_host->viewsDirty) {
                m_host->converter->convert(m_device->context(),
                                           m_host->retainedFrame.get(), mode);
                m_host->viewsDirty = false;
            }
            m_host->compositor->compose(
                m_device->context(), m_host->converter->outputSrv(),
                m_host->retainedSourceWidth, m_host->retainedSourceHeight,
                *layout, viewWidth, viewHeight);
            m_host->haveViews = true;
        } else {
            m_host->compositor->clearViews(m_device->context(), viewWidth, viewHeight);
            m_host->haveViews = false;
        }
        for (const RenderedSubtitle& subtitle : m_host->renderedSubtitles) {
            m_host->compositor->drawOverlay(
                m_device->context(), subtitle.bgra.data(), subtitle.width,
                subtitle.height, subtitle.destination, clientWidth, clientHeight);
        }
        if (controlsSurface.visible && !controlsSurface.bgra.empty()) {
            const RECT destination{
                controlsSurface.left, controlsSurface.top,
                controlsSurface.left + controlsSurface.width,
                controlsSurface.top + controlsSurface.height};
            m_host->compositor->drawOverlay(
                m_device->context(), controlsSurface.bgra.data(),
                static_cast<UINT>(controlsSurface.width),
                static_cast<UINT>(controlsSurface.height), destination,
                clientWidth, clientHeight);
        }
    };

    bool woven = false;
    try {
        {
            std::unique_lock<std::recursive_mutex> contextLock;
            if (m_host->session) {
                if (std::recursive_mutex* mutex = m_host->session->contextMutex()) {
                    contextLock = std::unique_lock<std::recursive_mutex>(*mutex);
                }
            }
            const UINT viewWidth = readyFor3D && m_host->weaver->recommendedViewWidth() > 0
                ? m_host->weaver->recommendedViewWidth() : clientWidth;
            const UINT viewHeight = readyFor3D && m_host->weaver->recommendedViewHeight() > 0
                ? m_host->weaver->recommendedViewHeight() : clientHeight;
            const UINT aspectWidth = readyFor3D
                ? static_cast<UINT>(targetDisplay.right - targetDisplay.left) : clientWidth;
            const UINT aspectHeight = readyFor3D
                ? static_cast<UINT>(targetDisplay.bottom - targetDisplay.top) : clientHeight;
            composeScene(viewWidth, viewHeight, aspectWidth, aspectHeight);
            m_device->bindBackBufferForWeave();
            if (readyFor3D) {
                woven = m_host->weaver->frameWeave(
                    m_host->compositor->outputSrv(), m_host->compositor->width(),
                    m_host->compositor->height());
            }
            if (!woven) {
                if (m_host->weaver &&
                    m_host->weaver->status() != ImmersityWeaver::Status::Active) {
                    m_host->weaver->setDesired3D(false);
                }
                if (viewWidth != clientWidth || viewHeight != clientHeight) {
                    const bool hadContextLock = contextLock.owns_lock();
                    if (hadContextLock) contextLock.unlock();
                    prepareSubtitles(clientWidth, clientHeight);
                    if (hadContextLock) contextLock.lock();
                    composeScene(clientWidth, clientHeight, clientWidth, clientHeight);
                }
                m_device->bindBackBufferForWeave();
                m_host->compositor->blitLeftEye(
                    m_device->context(), clientWidth, clientHeight);
            }
        }
        const HRESULT presentResult = m_device->present();
        if (FAILED(presentResult)) throw std::runtime_error("Display presentation failed");
        if (m_host->unrecordedPresentation) {
            const int64_t clockNs = m_host->session
                ? m_host->session->mediaTimeNanoseconds() : 0;
            ++m_host->scheduledPresented;
            if (m_host->retainedPtsNs) {
                const int64_t age = clockNs - *m_host->retainedPtsNs;
                m_host->maximumPresentedAgeNs =
                    (std::max)(m_host->maximumPresentedAgeNs, age);
                if (!m_host->firstPresentedPtsNs) {
                    m_host->firstPresentedPtsNs = *m_host->retainedPtsNs;
                    m_host->firstPresentedClockNs = clockNs;
                }
                m_host->lastPresentedPtsNs = *m_host->retainedPtsNs;
                m_host->lastPresentedClockNs = clockNs;
                constexpr std::uint64_t warmupFrames = 48;
                if (m_host->scheduledPresented > warmupFrames) {
                    ++m_host->steadyPresented;
                    if (age < -40'000'000 || age > 40'000'000) {
                        ++m_host->steadyOutside40ms;
                    }
                    if (m_host->steadyPresentationAgesNs.size() < 200'000) {
                        m_host->steadyPresentationAgesNs.push_back(age);
                    }
                }
            }
            m_host->unrecordedPresentation = false;
        }
    } catch (const std::exception& exception) {
        m_host->hostError.assign(exception.what(),
                                 exception.what() + std::strlen(exception.what()));
        if (m_host->weaver) m_host->weaver->setDesired3D(false);
        m_host->retainedFrame.reset();
        m_host->pendingFrame.reset();
        m_host->converter.reset();
        m_host->haveViews = false;
        m_host->viewsDirty = false;
        m_host->unrecordedPresentation = false;
        if (m_host->session) m_host->session->pause();
        m_host->controlsDirty = true;
        m_device->clearAndPresent(kClearColor);
    }
}

int AppShell::runSpike(const std::wstring& pngPath) {
    ComScope com;

    LoadedTexture png;
    loadPngToTexture(m_device->device(), pngPath, png);

    ImmersityWeaver weaver(m_device->device(), m_device->context(), m_window->hwnd());

    while (m_running) {
        if (!m_window->pumpMessages()) break;

        weaver.setDesired3D(native3DEligible(*m_window, weaver.displayRect()));
        weaver.frameBegin();
        m_device->bindBackBufferForWeave();
        weaver.frameWeave(png.srv, png.width, png.height);
        m_device->present();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// M2 playback helpers
// ---------------------------------------------------------------------------

namespace {

struct AvFrameDeleter {
    void operator()(AVFrame* frame) const {
        av_frame_free(&frame);
    }
};

using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

static bool environmentFlag(const char* name) {
    char value[2]{};
    return GetEnvironmentVariableA(name, value, ARRAYSIZE(value)) > 0;
}

static std::optional<StereoLayout> fullSbsLayoutForFrame(
    const AVFrame* frame,
    const RECT& displayRect) {
    const AVRational sar = frame->sample_aspect_ratio;
    const int sarNumerator = sar.num > 0 && sar.den > 0 ? sar.num : 1;
    const int sarDenominator = sar.num > 0 && sar.den > 0 ? sar.den : 1;
    return makeStereoLayout(
        StereoMode::FullSbs,
        frame->width,
        frame->height,
        sarNumerator,
        sarDenominator,
        static_cast<int>(displayRect.right - displayRect.left),
        static_cast<int>(displayRect.bottom - displayRect.top),
        false);
}

} // namespace

int AppShell::runPlay(const std::wstring& videoPath) {
    if (!m_host) initializePersistentHost();
    openMedia(videoPath);
    return run();
}

int AppShell::runPlaySmokeTest(const std::wstring& videoPath) {
    // File missing -> CTest skip. Path comes from an env var; empty or
    // non-existent means the dev box just doesn't have the corpus.
    if (videoPath.empty() || GetFileAttributesW(videoPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return 77;
    }

    // Init order matters: weaver first so the SR SDK enumerates a quiescent
    // device. Bringing up VideoPipeline first races the decode-thread's
    // D3D11VA pool allocation against SR display init and crashes the
    // immediate context on some driver versions.
    std::unique_ptr<ImmersityWeaver> weaver;
    try {
        weaver = std::make_unique<ImmersityWeaver>(
            m_device->device(), m_device->context(), m_window->hwnd());
    } catch (const std::exception&) {
        return 3;
    }
    if (weaver->status() != ImmersityWeaver::Status::Active) return 77;
    if (!prepareNativeSmokeWindow(*m_window, weaver->displayRect())) return 10;
    weaver->setDesired3D(true);

    StereoCompositor compositor(m_device->device());

    std::unique_ptr<VideoPipeline> vp;
    try {
        VideoPipeline::Options options;
        options.forceSoftware = environmentFlag("ODYSSEY_FORCE_SOFTWARE_DECODE");
        vp = std::make_unique<VideoPipeline>(m_device->device(), videoPath, options);
    } catch (const std::exception&) {
        return 2;
    }

    std::unique_ptr<Nv12ToRgba> converter;

    const bool benchmark = environmentFlag("ODYSSEY_BENCHMARK_CONVERTER");
    const unsigned frameTarget = benchmark ? 240u : 60u;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    AvFramePtr lastFrame;
    unsigned rendered = 0;
    std::vector<double> conversionTimes;
    std::vector<double> compositorTimes;

    while (rendered < frameTarget) {
        if (std::chrono::steady_clock::now() > deadline) {
            return 7;
        }
        if (!m_window->pumpMessages()) {
            return 4;
        }
        if (!native3DEligible(*m_window, weaver->displayRect())) return 10;

        weaver->frameBegin();
        if (!weaver->lensEnabled()) {
            continue;
        }

        AVFrame* f = vp->pollLatest();
        if (!f) continue;

        lastFrame.reset(f);

        // Serialize the render sequence against the decode thread; see the
        // contextMutex comment in VideoPipeline.h.
        std::unique_lock<std::recursive_mutex> ctxLk(vp->contextMutex());
        const auto layout = fullSbsLayoutForFrame(f, weaver->displayRect());
        if (!layout) return 9;
        if (!converter
            || converter->width() != static_cast<UINT>(f->width)
            || converter->height() != static_cast<UINT>(f->height)) {
            converter = std::make_unique<Nv12ToRgba>(
                m_device->device(),
                static_cast<UINT>(f->width),
                static_cast<UINT>(f->height));
        }
        const auto conversionStart = std::chrono::steady_clock::now();
        converter->convert(m_device->context(), f, StereoMode::FullSbs);
        const auto conversionEnd = std::chrono::steady_clock::now();
        compositor.compose(
            m_device->context(),
            converter->outputSrv(),
            static_cast<UINT>(f->width),
            static_cast<UINT>(f->height),
            *layout,
            weaver->recommendedViewWidth(),
            weaver->recommendedViewHeight());
        const auto compositorEnd = std::chrono::steady_clock::now();
        if (benchmark && rendered >= 20) {
            conversionTimes.push_back(
                std::chrono::duration<double, std::milli>(
                    conversionEnd - conversionStart).count());
            compositorTimes.push_back(
                std::chrono::duration<double, std::milli>(
                    compositorEnd - conversionEnd).count());
        }
        m_device->bindBackBufferForWeave();
        if (!weaver->frameWeave(
                compositor.outputSrv(), compositor.width(), compositor.height())) {
            return 10;
        }
        ctxLk.unlock();
        if (FAILED(m_device->present())) {
            return 5;
        }
        ++rendered;
    }
    if (weaver->successfulWeaveCount() != rendered) return 10;

    if (benchmark) {
        const auto reportTimes = [](const char* label, std::vector<double> times) {
            std::sort(times.begin(), times.end());
            const auto percentile = [&times](double fraction) {
                return times[static_cast<std::size_t>((times.size() - 1) * fraction)];
            };
            std::fprintf(stderr, "%s n=%zu median=%.3fms p95=%.3fms p99=%.3fms\n",
                         label, times.size(), percentile(0.50), percentile(0.95),
                         percentile(0.99));
        };
        reportTimes("convert", conversionTimes);
        reportTimes("compose", compositorTimes);
    }

#ifdef _DEBUG
    ID3D11InfoQueue* iq{};
    if (SUCCEEDED(m_device->device()->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&iq))) {
        UINT64 n = iq->GetNumStoredMessagesAllowedByRetrievalFilter();
        iq->Release();
        if (n > 0) return 6;
    }
#endif

    return 0;
}

int AppShell::runPlayerSmoke(const std::wstring& videoPath, double seconds) {
    if (videoPath.empty() || GetFileAttributesW(videoPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return 77;
    }
    if (!std::isfinite(seconds) || seconds < 0.25 || seconds > 7200.0) return 2;
    if (!m_host) initializePersistentHost();
    if (!m_host->weaver || m_host->weaver->status() != ImmersityWeaver::Status::Active) {
        return 77;
    }
    m_host->muted = true;
    openMedia(videoPath);
    if (!m_host->session) return 3;
    BringWindowToTop(m_window->hwnd());
    SetForegroundWindow(m_window->hwnd());
    SetActiveWindow(m_window->hwnd());
    SetFocus(m_window->hwnd());

    const std::uint64_t weaveStart = m_host->weaver->successfulWeaveCount();
#ifdef _DEBUG
    const int debugStart = d3dValidationErrorCount(m_device->device());
#endif
    std::optional<int64_t> mediaStart;
    const auto wallStart = std::chrono::steady_clock::now();
    auto nextProgress = wallStart + std::chrono::seconds(60);
    auto lastClockMovement = wallStart;
    int64_t lastClockNs = 0;
    bool clockStalled = false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<int64_t>((seconds + 60.0) * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        if (!m_window->pumpMessages()) return 4;
        renderPersistentFrame();
        const PlaybackSession::Status currentStatus = m_host->session->status();
        if (currentStatus == PlaybackSession::Status::Failed ||
            currentStatus == PlaybackSession::Status::Paused || !m_host->hostError.empty()) break;
        const int64_t currentClockNs = m_host->session->mediaTimeNanoseconds();
        if (currentClockNs != lastClockNs) {
            lastClockNs = currentClockNs;
            lastClockMovement = std::chrono::steady_clock::now();
        } else if (mediaStart && std::chrono::steady_clock::now() - lastClockMovement >
                                    std::chrono::seconds(5)) {
            clockStalled = true;
            break;
        }
        if (m_host->scheduledPresented > 0 && !mediaStart) {
            mediaStart = m_host->session->mediaTimeNanoseconds();
            lastClockMovement = std::chrono::steady_clock::now();
            lastClockNs = *mediaStart;
        }
        if (mediaStart && m_host->session->mediaTimeNanoseconds() - *mediaStart >=
                              static_cast<int64_t>(seconds * 1'000'000'000.0)) {
            break;
        }
        if (m_host->session->status() == PlaybackSession::Status::Drained) break;
        if (std::chrono::steady_clock::now() >= nextProgress) {
            std::fprintf(stderr, "PLAYER_SMOKE_PROGRESS presented=%llu dropped=%llu media=%.3f\n",
                static_cast<unsigned long long>(m_host->scheduledPresented),
                static_cast<unsigned long long>(m_host->scheduledDropped),
                m_host->session->positionSeconds());
            nextProgress += std::chrono::seconds(60);
        }
    }
    const double elapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wallStart).count();
    const PlaybackSession::Status finalStatus = m_host->session->status();
    const std::wstring finalError = !m_host->hostError.empty()
        ? m_host->hostError : m_host->session->error();
    m_host->weaver->setDesired3D(false);
    m_host->session->pause();
    const std::uint64_t weaves = m_host->weaver->successfulWeaveCount() - weaveStart;
    const RECT target = m_host->weaver->displayRect();
    const PlaybackSession::TimingDiagnostics timing =
        m_host->session->timingDiagnostics();
    RECT windowRect{};
    RECT clientRect{};
    GetWindowRect(m_window->hwnd(), &windowRect);
    GetClientRect(m_window->hwnd(), &clientRect);
    std::vector<int64_t> sortedAges = m_host->steadyPresentationAgesNs;
    std::sort(sortedAges.begin(), sortedAges.end());
    const auto percentile = [&](double fraction) {
        if (sortedAges.empty()) return int64_t{0};
        const size_t index = static_cast<size_t>(
            fraction * static_cast<double>(sortedAges.size() - 1));
        return sortedAges[index];
    };
    const int64_t sourceDeltaNs = m_host->firstPresentedPtsNs && m_host->lastPresentedPtsNs
        ? *m_host->lastPresentedPtsNs - *m_host->firstPresentedPtsNs : 0;
    const int64_t consumedClockDeltaNs =
        m_host->firstPresentedClockNs && m_host->lastPresentedClockNs
        ? *m_host->lastPresentedClockNs - *m_host->firstPresentedClockNs : 0;
    const double expectedFrames =
        m_host->nominalFrameDurationNs && *m_host->nominalFrameDurationNs > 0
        ? static_cast<double>(sourceDeltaNs) / *m_host->nominalFrameDurationNs + 1.0 : 0.0;
    std::fprintf(stderr,
        "PLAYER_SMOKE presented=%llu dropped=%llu waited=%llu max_age_ns=%lld "
        "p95_age_ns=%lld p99_age_ns=%lld outside40=%llu/%llu expected=%.2f "
        "elapsed=%.3f source_delta=%.3f clock_delta=%.3f weaves=%llu "
        "source=%ux%u target=%ldx%ld client=%ux%u srv=%ux%u "
        "age_point=present_return status=%d gates=%d%d%d%d%d%d%d%d "
        "sdk_status=%d lens=%d fg=%d active=%d focus=%d "
        "fullscreen=%d iconic=%d foreground_hwnd=%p window_hwnd=%p "
        "window=%ld,%ld,%ld,%ld clientrect=%ld,%ld,%ld,%ld "
        "segment=%lld runref=%lld segments=%llu runs=%llu error=%s\n",
        static_cast<unsigned long long>(m_host->scheduledPresented),
        static_cast<unsigned long long>(m_host->scheduledDropped),
        static_cast<unsigned long long>(m_host->scheduledWaited),
        static_cast<long long>(m_host->maximumPresentedAgeNs),
        static_cast<long long>(percentile(0.95)),
        static_cast<long long>(percentile(0.99)),
        static_cast<unsigned long long>(m_host->steadyOutside40ms),
        static_cast<unsigned long long>(m_host->steadyPresented), expectedFrames,
        elapsedSeconds, static_cast<double>(sourceDeltaNs) / 1'000'000'000.0,
        static_cast<double>(consumedClockDeltaNs) / 1'000'000'000.0,
        static_cast<unsigned long long>(weaves), m_host->retainedSourceWidth,
        m_host->retainedSourceHeight, target.right - target.left,
        target.bottom - target.top, m_window->clientWidth(), m_window->clientHeight(),
        m_host->compositor->width(), m_host->compositor->height(),
        static_cast<int>(finalStatus),
        m_host->gateValidDisplay, m_host->gateHasFrame, m_host->gateStereo,
        m_host->gateSdkActive, m_host->gateNativeUiClear, m_host->gateFocused,
        m_host->gateNativeRect, m_host->gateAdapter,
        static_cast<int>(m_host->weaver->status()), m_host->weaver->lensEnabled(),
        GetForegroundWindow() == m_window->hwnd(), GetActiveWindow() == m_window->hwnd(),
        GetFocus() == m_window->hwnd(), m_window->isFullscreen(), IsIconic(m_window->hwnd()),
        static_cast<void*>(GetForegroundWindow()), static_cast<void*>(m_window->hwnd()),
        windowRect.left, windowRect.top,
        windowRect.right, windowRect.bottom, clientRect.left, clientRect.top,
        clientRect.right, clientRect.bottom,
        static_cast<long long>(timing.segmentStart100ns),
        static_cast<long long>(timing.runReference100ns),
        static_cast<unsigned long long>(timing.segmentCount),
        static_cast<unsigned long long>(timing.runCount), narrowUtf8(finalError).c_str());
    {
        // Audio A/V-sync diagnostics for smoke verification; written to a temp file
        // since this is a WIN32 app and stderr may not be visible.
        const VideoPipeline::AudioDiagnostics audio = m_host->session->audioDiagnostics();
        char line[256];
        std::snprintf(line, sizeof(line),
            "PLAYER_SMOKE_AUDIO gap_silence=%llu late_trimmed=%llu content=%llu max_err_ns=%lld\n",
            static_cast<unsigned long long>(audio.gapSilenceFramesSubmitted),
            static_cast<unsigned long long>(audio.lateFramesTrimmed),
            static_cast<unsigned long long>(audio.contentFramesSubmitted),
            static_cast<long long>(audio.maximumSubmissionErrorNs));
        OutputDebugStringA(line);
        std::fputs(line, stderr);
        wchar_t tempDir[MAX_PATH]{};
        GetEnvironmentVariableW(L"TEMP", tempDir, MAX_PATH);
        FILE* diagFile = nullptr;
        _wfopen_s(&diagFile,
            (std::filesystem::path(tempDir) / L"odyssey-audio-diag.txt").c_str(), L"a");
        if (diagFile) {
            std::fputs(line, diagFile);
            std::fclose(diagFile);
        }
    }
    if (!finalError.empty() || finalStatus == PlaybackSession::Status::Failed ||
        finalStatus == PlaybackSession::Status::Paused) return 5;
    if (finalStatus == PlaybackSession::Status::Opening) return 11;
    if (clockStalled) return 12;
    const std::uint64_t minimumFrames = static_cast<std::uint64_t>(seconds * 20.0);
    if (m_host->scheduledPresented < (std::max)(std::uint64_t{1}, minimumFrames)) return 6;
    if (expectedFrames > 1.0 &&
        static_cast<double>(m_host->scheduledPresented) < expectedFrames * 0.9) return 6;
    if (weaves == 0) return 7;
    if (m_host->maximumPresentedAgeNs > 250'000'000) return 8;
    if (m_host->steadyPresented > 0 &&
        m_host->steadyOutside40ms * 100 > m_host->steadyPresented) return 10;
#ifdef _DEBUG
    const int debugEnd = d3dValidationErrorCount(m_device->device());
    if (debugStart >= 0 && debugEnd > debugStart) return 9;
#endif
    return 0;
}

int AppShell::runSpikeSmokeTest(const std::wstring& pngPath) {
    ComScope com;

    LoadedTexture png;
    try {
        loadPngToTexture(m_device->device(), pngPath, png);
    } catch (const std::exception&) {
        return 2; // PNG load failure is a real error, not a soft skip.
    }

    std::unique_ptr<ImmersityWeaver> weaver;
    try {
        weaver = std::make_unique<ImmersityWeaver>(
            m_device->device(), m_device->context(), m_window->hwnd());
    } catch (const std::exception&) {
        return 3;
    }

    if (weaver->status() != ImmersityWeaver::Status::Active) {
        return 77; // CTest soft-skip: SR service or display not available here.
    }
    if (!prepareNativeSmokeWindow(*m_window, weaver->displayRect())) return 7;
    weaver->setDesired3D(true);

    constexpr int kFrames = 30;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int frames = 0;
    while (frames < kFrames) {
        if (std::chrono::steady_clock::now() > deadline) return 7;
        if (!m_window->pumpMessages()) return 4;
        if (!native3DEligible(*m_window, weaver->displayRect())) return 7;
        weaver->frameBegin();
        if (!weaver->lensEnabled()) continue;
        m_device->bindBackBufferForWeave();
        if (!weaver->frameWeave(png.srv, png.width, png.height)) return 7;
        if (FAILED(m_device->present())) return 5;
        ++frames;
    }
    if (weaver->successfulWeaveCount() != static_cast<std::uint64_t>(kFrames)) return 7;

#ifdef _DEBUG
    if (int n = d3dValidationErrorCount(m_device->device()); n > 0) return 6;
#endif

    return 0;
}

} // namespace odyssey
