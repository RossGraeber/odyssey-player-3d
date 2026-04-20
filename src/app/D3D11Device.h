#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

namespace odyssey {

class D3D11Device {
public:
    D3D11Device(HWND hwnd, UINT width, UINT height);
    ~D3D11Device();

    D3D11Device(const D3D11Device&) = delete;
    D3D11Device& operator=(const D3D11Device&) = delete;

    void resize(UINT width, UINT height);
    HRESULT clearAndPresent(const float rgbaLinear[4]);

    // Tears down swap chain / context / RTV and releases the device, reporting
    // any D3D11 objects that outlived us. Returns the number of unexpected
    // live objects (0 == clean). -1 in non-debug builds where we can't probe.
    // Safe to call multiple times; the dtor calls it if the caller didn't.
    int teardownAndProbe();

    ID3D11Device*        device()  const { return m_device; }
    ID3D11DeviceContext* context() const { return m_context; }

private:
    void createBackBufferView();
    void releaseBackBufferView();

    HWND m_hwnd{nullptr};
    UINT m_width{0};
    UINT m_height{0};

    ID3D11Device*           m_device{nullptr};
    ID3D11DeviceContext*    m_context{nullptr};
    IDXGISwapChain1*        m_swapChain{nullptr};
    ID3D11RenderTargetView* m_rtv{nullptr};
};

} // namespace odyssey
