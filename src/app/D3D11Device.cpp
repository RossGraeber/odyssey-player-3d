#include "D3D11Device.h"

#include <dxgi1_2.h>

#ifdef _DEBUG
#include <d3d11sdklayers.h>
#endif

#include <stdexcept>
#include <string>

namespace odyssey {

static void hrCheck(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[128];
        sprintf_s(buf, "D3D11 init failed: %s (hr=0x%08lX)", what, (unsigned long)hr);
        throw std::runtime_error(buf);
    }
}

D3D11Device::D3D11Device(HWND hwnd, UINT width, UINT height)
    : m_hwnd(hwnd), m_width(width), m_height(height)
{
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    ID3D11Device*        dev{};
    ID3D11DeviceContext* ctx{};
    D3D_FEATURE_LEVEL    got{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &dev, &got, &ctx);
    hrCheck(hr, "D3D11CreateDevice");
    m_device  = dev;
    m_context = ctx;

    IDXGIDevice* dxgiDev{};
    hrCheck(m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev), "QI IDXGIDevice");
    IDXGIAdapter* adapter{};
    hrCheck(dxgiDev->GetAdapter(&adapter), "GetAdapter");
    IDXGIFactory2* factory{};
    hrCheck(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory), "GetParent IDXGIFactory2");

    DXGI_SWAP_CHAIN_DESC1 scd{};
    // Flip-model swap chains cannot use _SRGB formats directly — the buffer
    // is _UNORM and we get sRGB via the RTV's view format below.
    scd.Width       = m_width;
    scd.Height      = m_height;
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;

    hrCheck(factory->CreateSwapChainForHwnd(m_device, m_hwnd, &scd, nullptr, nullptr, &m_swapChain),
            "CreateSwapChainForHwnd");
    factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

    factory->Release();
    adapter->Release();
    dxgiDev->Release();

    createBackBufferView();
}

D3D11Device::~D3D11Device() {
    teardownAndProbe();
}

void D3D11Device::createBackBufferView() {
    ID3D11Texture2D* backBuffer{};
    hrCheck(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer), "GetBuffer");
    // View format is sRGB so ClearRenderTargetView's linear color is correctly
    // encoded on write; the underlying buffer is _UNORM per flip-model rules.
    D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format        = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hrCheck(m_device->CreateRenderTargetView(backBuffer, &rtvd, &m_rtv), "CreateRenderTargetView");
    backBuffer->Release();
}

void D3D11Device::releaseBackBufferView() {
    if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }
}

void D3D11Device::resize(UINT width, UINT height) {
    if (!m_swapChain || width == 0 || height == 0) return;
    m_width = width; m_height = height;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    releaseBackBufferView();
    hrCheck(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
    createBackBufferView();
}

HRESULT D3D11Device::clearAndPresent(const float rgbaLinear[4]) {
    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width    = (FLOAT)m_width;
    vp.Height   = (FLOAT)m_height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    m_context->ClearRenderTargetView(m_rtv, rgbaLinear);
    return m_swapChain->Present(1, 0);
}

void D3D11Device::bindBackBufferForWeave() {
    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width    = (FLOAT)m_width;
    vp.Height   = (FLOAT)m_height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

HRESULT D3D11Device::present() {
    return m_swapChain->Present(1, 0);
}

int D3D11Device::teardownAndProbe() {
    releaseBackBufferView();
    if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
    if (m_context)   { m_context->ClearState(); m_context->Flush(); m_context->Release(); m_context = nullptr; }

    if (!m_device) return 0;

    int unexpected = -1;
#ifdef _DEBUG
    ID3D11Debug* dbg{};
    if (SUCCEEDED(m_device->QueryInterface(__uuidof(ID3D11Debug), (void**)&dbg))) {
        OutputDebugStringW(L"[odyssey] D3D11 live-object report follows:\n");
        // IGNORE_INTERNAL filters out D3D11's own book-keeping objects so the
        // summary reflects only things we (or our callers) failed to release.
        dbg->ReportLiveDeviceObjects((D3D11_RLDO_FLAGS)(D3D11_RLDO_SUMMARY | D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL));

        // At this point our tracked objects (swap chain, context, RTV) are gone.
        // The only refs on the device should be: 1 from us + 1 from the QI that
        // produced `dbg`. Release dbg, then probe the device's refcount: AddRef
        // returns the new count, Release returns the decremented count. If the
        // post-Release count is not 1, something else still holds the device.
        dbg->Release();
        ULONG after = m_device->AddRef();
        m_device->Release();
        unexpected = (after == 2) ? 0 : static_cast<int>(after) - 2;
    }
#endif
    m_device->Release();
    m_device = nullptr;
    return unexpected;
}

} // namespace odyssey
