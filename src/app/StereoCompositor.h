#pragma once

#include "StereoLayout.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace odyssey {

// Places two views from the current RGBA frame into an SDK-sized SBS texture.
// The input must use DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, matching Nv12ToRgba.
class StereoCompositor {
public:
    explicit StereoCompositor(ID3D11Device* device);

    StereoCompositor(const StereoCompositor&) = delete;
    StereoCompositor& operator=(const StereoCompositor&) = delete;

    void compose(
        ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* input,
        UINT sourceWidth,
        UINT sourceHeight,
        const StereoLayout& layout,
        UINT perEyeWidth,
        UINT perEyeHeight);
    void clearViews(
        ID3D11DeviceContext* context,
        UINT perEyeWidth,
        UINT perEyeHeight);
    void drawOverlay(
        ID3D11DeviceContext* context,
        const std::uint8_t* bgra,
        UINT imageWidth,
        UINT imageHeight,
        const RECT& destination,
        UINT canvasWidth,
        UINT canvasHeight);
    void blitLeftEye(
        ID3D11DeviceContext* context,
        UINT clientWidth,
        UINT clientHeight);

    ID3D11ShaderResourceView* outputSrv() const noexcept;
    UINT width() const noexcept;
    UINT height() const noexcept;

private:
    void createShaders();
    void ensureOutput(UINT perEyeWidth, UINT perEyeHeight);
    void ensureOverlay(UINT imageWidth, UINT imageHeight);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_overlayPixelShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_alphaBlend;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_outputTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_outputRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_outputSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_overlayTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_overlaySrv;
    UINT m_width{0};
    UINT m_height{0};
    UINT m_overlayWidth{0};
    UINT m_overlayHeight{0};
};

} // namespace odyssey
