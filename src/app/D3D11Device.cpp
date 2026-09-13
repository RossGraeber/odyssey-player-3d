#include "D3D11Device.h"

#include <d3d10.h>
#include <dxgi1_2.h>

#ifdef _DEBUG
#include <d3d11sdklayers.h>
#endif

#include <stdexcept>
#include <string>
#include <utility>

namespace odyssey {

static void hrCheck(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[128];
        sprintf_s(buf, "D3D11 init failed: %s (hr=0x%08lX)", what, (unsigned long)hr);
        throw std::runtime_error(buf);
    }
}

D3D11Device::D3D11Device(HWND hwnd, UINT width, UINT height, HMONITOR targetMonitor)
    : m_hwnd(hwnd), m_width(width), m_height(height)
{
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    Microsoft::WRL::ComPtr<IDXGIAdapter1> selectedAdapter;
    if (targetMonitor) {
        Microsoft::WRL::ComPtr<IDXGIFactory1> selectionFactory;
        hrCheck(CreateDXGIFactory1(IID_PPV_ARGS(&selectionFactory)), "CreateDXGIFactory1");
        for (UINT adapterIndex = 0; ; ++adapterIndex) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapterResult = selectionFactory->EnumAdapters1(adapterIndex, &adapter);
            if (adapterResult == DXGI_ERROR_NOT_FOUND) break;
            hrCheck(adapterResult, "EnumAdapters1");
            for (UINT outputIndex = 0; ; ++outputIndex) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
                if (outputResult == DXGI_ERROR_NOT_FOUND) break;
                hrCheck(outputResult, "EnumOutputs");
                DXGI_OUTPUT_DESC outputDescription{};
                hrCheck(output->GetDesc(&outputDescription), "IDXGIOutput::GetDesc");
                if (outputDescription.Monitor == targetMonitor) {
                    selectedAdapter = adapter;
                    break;
                }
            }
            if (selectedAdapter) break;
        }
        if (!selectedAdapter) {
            hrCheck(DXGI_ERROR_NOT_FOUND, "target monitor adapter");
        }
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL    got{};
    HRESULT hr = D3D11CreateDevice(
        selectedAdapter.Get(), selectedAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &device, &got, &context);
    hrCheck(hr, "D3D11CreateDevice");

    // We supply this device to FFmpeg instead of using FFmpeg's device
    // creation path, so mirror its multithread protection before decode can
    // overlap a v-synced Present on the main thread.
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    hr = device.As(&multithread);
    hrCheck(hr, "QI ID3D10Multithread");
    multithread->SetMultithreadProtected(TRUE);
    const BOOL isMultithreadProtected = multithread->GetMultithreadProtected();
    if (!isMultithreadProtected) {
        throw std::runtime_error("D3D11 multithread protection could not be enabled");
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hrCheck(device.As(&dxgiDevice), "QI IDXGIDevice");
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hrCheck(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hrCheck(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetParent IDXGIFactory2");

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

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    hrCheck(factory->CreateSwapChainForHwnd(device.Get(), m_hwnd, &scd,
                                             nullptr, nullptr, &swapChain),
            "CreateSwapChainForHwnd");
    hrCheck(factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER),
            "MakeWindowAssociation");

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    hrCheck(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "GetBuffer");
    D3D11_RENDER_TARGET_VIEW_DESC renderTargetDescription{};
    renderTargetDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    renderTargetDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget;
    hrCheck(device->CreateRenderTargetView(backBuffer.Get(), &renderTargetDescription,
                                            &renderTarget),
            "CreateRenderTargetView");

    m_device = std::move(device);
    m_context = std::move(context);
    m_swapChain = std::move(swapChain);
    m_rtv = std::move(renderTarget);
}

D3D11Device::~D3D11Device() {
    teardownAndProbe();
}

void D3D11Device::createBackBufferView() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    hrCheck(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "GetBuffer");
    // View format is sRGB so ClearRenderTargetView's linear color is correctly
    // encoded on write; the underlying buffer is _UNORM per flip-model rules.
    D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format        = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hrCheck(m_device->CreateRenderTargetView(backBuffer.Get(), &rtvd, &m_rtv),
            "CreateRenderTargetView");
}

void D3D11Device::releaseBackBufferView() {
    m_rtv.Reset();
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
    ID3D11RenderTargetView* renderTarget = m_rtv.Get();
    m_context->OMSetRenderTargets(1, &renderTarget, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width    = (FLOAT)m_width;
    vp.Height   = (FLOAT)m_height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    m_context->ClearRenderTargetView(m_rtv.Get(), rgbaLinear);
    return m_swapChain->Present(1, 0);
}

void D3D11Device::bindBackBufferForWeave() {
    ID3D11RenderTargetView* renderTarget = m_rtv.Get();
    m_context->OMSetRenderTargets(1, &renderTarget, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width    = (FLOAT)m_width;
    vp.Height   = (FLOAT)m_height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

HRESULT D3D11Device::present() {
    return m_swapChain->Present(1, 0);
}

bool D3D11Device::currentAdapterOwnsMonitor(HMONITOR monitor) const noexcept {
    if (!monitor || !m_device) return false;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device.As(&dxgiDevice))) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
    for (UINT index = 0; ; ++index) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        const HRESULT hr = adapter->EnumOutputs(index, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) return false;
        if (FAILED(hr)) return false;
        DXGI_OUTPUT_DESC description{};
        if (FAILED(output->GetDesc(&description))) return false;
        if (description.Monitor == monitor) return true;
    }
}

int D3D11Device::teardownAndProbe() {
    releaseBackBufferView();
    m_swapChain.Reset();
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
        m_context.Reset();
    }

    if (!m_device) return 0;

    int unexpected = -1;
#ifdef _DEBUG
    Microsoft::WRL::ComPtr<ID3D11Debug> debug;
    if (SUCCEEDED(m_device.As(&debug))) {
        OutputDebugStringW(L"[odyssey] D3D11 live-object report follows:\n");
        // IGNORE_INTERNAL filters out D3D11's own book-keeping objects so the
        // summary reflects only things we (or our callers) failed to release.
        debug->ReportLiveDeviceObjects((D3D11_RLDO_FLAGS)(D3D11_RLDO_SUMMARY | D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL));

        // At this point our tracked objects (swap chain, context, RTV) are gone.
        // The only refs on the device should be: 1 from us + 1 from the QI that
        // produced `dbg`. Release dbg, then probe the device's refcount: AddRef
        // returns the new count, Release returns the decremented count. If the
        // post-Release count is not 1, something else still holds the device.
        debug.Reset();
        ULONG after = m_device->AddRef();
        m_device->Release();
        unexpected = (after == 2) ? 0 : static_cast<int>(after) - 2;
    }
#endif
    m_device.Reset();
    return unexpected;
}

} // namespace odyssey
