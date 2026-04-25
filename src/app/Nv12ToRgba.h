#pragma once

#include <windows.h>
#include <d3d11.h>

namespace odyssey {

// Converts decoded NV12 frames into a sampleable RGBA SRV.
//
// The decoder writes into FFmpeg's hwframes pool — a Texture2DArray of NV12
// surfaces. Sampling a single array slice via a typed (R8 / R8G8) texture-
// array SRV is the textbook path, but on this driver / DXGI build it
// hard-faults the immediate context at Draw time. Instead we keep a private
// non-arrayed NV12 staging texture with D3D11_BIND_SHADER_RESOURCE, copy the
// decoded slice into it via CopySubresourceRegion, and sample that.
// One copy per frame; trivial cost vs. the YUV->RGB shader work, and it
// matches the integration pattern most non-FFmpeg D3D11 video samples use.
class Nv12ToRgba {
public:
    Nv12ToRgba(ID3D11Device* device, UINT logicalWidth, UINT logicalHeight);
    ~Nv12ToRgba();

    Nv12ToRgba(const Nv12ToRgba&) = delete;
    Nv12ToRgba& operator=(const Nv12ToRgba&) = delete;

    void convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* coded, UINT slice);

    ID3D11ShaderResourceView* outputSrv() const { return m_outSrv; }
    UINT width()  const { return m_width; }
    UINT height() const { return m_height; }

private:
    void compileShaders(ID3D11Device*);
    void createTargets(ID3D11Device*);
    void ensureStaging(UINT codedW, UINT codedH);

    UINT m_width{0};
    UINT m_height{0};

    ID3D11Device*             m_device{nullptr};
    ID3D11VertexShader*       m_vs{nullptr};
    ID3D11PixelShader*        m_ps{nullptr};
    ID3D11SamplerState*       m_sampler{nullptr};
    ID3D11Buffer*             m_cb{nullptr};

    // Output (RGBA) target.
    ID3D11Texture2D*          m_outTex{nullptr};
    ID3D11RenderTargetView*   m_outRtv{nullptr};
    ID3D11ShaderResourceView* m_outSrv{nullptr};

    // Private NV12 staging tex + plane SRVs. Allocated lazily on first
    // convert() once we know the decoded coded dimensions.
    UINT                      m_stagingW{0};
    UINT                      m_stagingH{0};
    ID3D11Texture2D*          m_nv12Tex{nullptr};
    ID3D11ShaderResourceView* m_yPlaneSrv{nullptr};
    ID3D11ShaderResourceView* m_uvPlaneSrv{nullptr};
};

} // namespace odyssey
