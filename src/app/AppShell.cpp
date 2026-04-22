#include "AppShell.h"

#include <wincodec.h>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#include <d3d11sdklayers.h>
#endif

#pragma comment(lib, "windowscodecs.lib")

namespace odyssey {

// sRGB (#0A0A0A) -> linear. 0x0A/255 = 0.03921568. Below 0.04045 the transfer
// is linear/12.92, so linearValue = 0.03921568 / 12.92 ~= 0.003035.
static constexpr float kClearLinear = 0.003035f;
static const float kClearColor[4] = { kClearLinear, kClearLinear, kClearLinear, 1.0f };

AppShell::AppShell() {
    Win32Window::Callbacks cb;
    cb.onResize = [this](UINT w, UINT h) {
        if (m_device) m_device->resize(w, h);
        ++m_resizeCount;
    };
    cb.onToggleFullscreen = [this]() {
        if (m_window) m_window->toggleBorderlessFullscreen();
    };
    cb.onQuit = [this]() {
        m_running = false;
        PostQuitMessage(0);
    };

    m_window = std::make_unique<Win32Window>(L"Odyssey Player 3D", 1280, 720, std::move(cb));
    m_device = std::make_unique<D3D11Device>(m_window->hwnd(), 1280u, 720u);
}

AppShell::~AppShell() = default;

int AppShell::run() {
    while (m_running) {
        if (!m_window->pumpMessages()) break;
        m_device->clearAndPresent(kClearColor);
    }
    return 0;
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
// R8G8B8A8_TYPELESS so the SRV can be R8G8B8A8_UNORM_SRGB — DX11 only allows
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

} // namespace

int AppShell::runSpike(const std::wstring& pngPath) {
    ComScope com;

    LoadedTexture png;
    loadPngToTexture(m_device->device(), pngPath, png);

    ImmersityWeaver weaver(m_device->device(), m_device->context(), m_window->hwnd());

    while (m_running) {
        if (!m_window->pumpMessages()) break;

        weaver.frameBegin();
        m_device->bindBackBufferForWeave();
        weaver.frameWeave(png.srv, png.width, png.height);
        m_device->present();
    }
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
        weaver = std::make_unique<ImmersityWeaver>(m_device->device(), m_device->context(),
                                                   m_window->hwnd(), 2.0);
    } catch (const std::exception&) {
        return 3;
    }

    if (weaver->status() != ImmersityWeaver::Status::Active) {
        return 77; // CTest soft-skip: SR service or display not available here.
    }

    constexpr int kFrames = 30;
    for (int i = 0; i < kFrames; ++i) {
        if (!m_window->pumpMessages()) return 4;
        weaver->frameBegin();
        m_device->bindBackBufferForWeave();
        weaver->frameWeave(png.srv, png.width, png.height);
        if (FAILED(m_device->present())) return 5;
    }

#ifdef _DEBUG
    if (int n = d3dValidationErrorCount(m_device->device()); n > 0) return 6;
#endif

    return 0;
}

} // namespace odyssey
