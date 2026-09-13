#pragma once

#include "StereoLayout.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <vector>

struct AVFrame;
struct SwsContext;

namespace odyssey {

// Converts a decoded conventional SDR frame into a source-resolution RGBA sRGB
// texture. The encoded SDR transfer is preserved; wide-gamut primaries require
// a separate color-management path.
// SBS eyes are converted independently so chroma upsampling cannot cross the
// eye boundary. PQ and HLG input require a separate tone-mapping path.
class Nv12ToRgba {
public:
    Nv12ToRgba(ID3D11Device* device, UINT logicalWidth, UINT logicalHeight);
    ~Nv12ToRgba();

    Nv12ToRgba(const Nv12ToRgba&) = delete;
    Nv12ToRgba& operator=(const Nv12ToRgba&) = delete;

    void convert(ID3D11DeviceContext* context, const AVFrame* frame, StereoMode mode);

    ID3D11ShaderResourceView* outputSrv() const { return m_outputSrv.Get(); }
    UINT width() const { return m_width; }
    UINT height() const { return m_height; }

private:
    struct FrameDeleter {
        void operator()(AVFrame* frame) const noexcept;
    };
    struct SwsDeleter {
        void operator()(SwsContext* context) const noexcept;
    };

    const AVFrame* softwareFrame(const AVFrame* frame);
    void configureScaler(int pixelFormat, UINT eyeWidth, int colorSpace, int sourceRange);
    void convertEye(const AVFrame* frame, UINT sourceX, UINT destinationX);
    bool convertHardwareNv12(
        ID3D11DeviceContext* context,
        const AVFrame* frame,
        StereoMode mode,
        int colorSpace,
        int sourceRange);
    bool convertSoftwareSemiPlanar(
        ID3D11DeviceContext* context,
        const AVFrame* frame,
        StereoMode mode,
        int colorSpace,
        int sourceRange);
    void ensureYuvTexture(UINT codedWidth, UINT codedHeight, DXGI_FORMAT format);
    void renderYuv(
        ID3D11DeviceContext* context,
        StereoMode mode,
        int colorSpace,
        int sourceRange,
        unsigned bitDepth);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_outputTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_outputRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_outputSrv;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_yuvTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_yPlaneSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_uvPlaneSrv;
    std::unique_ptr<AVFrame, FrameDeleter> m_transferFrame;
    std::unique_ptr<SwsContext, SwsDeleter> m_scaler;
    std::vector<std::uint8_t> m_rgba;
    std::vector<std::uint8_t> m_eyeRgba;
    std::vector<std::uint8_t> m_yuvUpload;
    UINT m_width{0};
    UINT m_height{0};
    UINT m_transferWidth{0};
    UINT m_transferHeight{0};
    UINT m_stagingWidth{0};
    UINT m_stagingHeight{0};
    DXGI_FORMAT m_stagingFormat{DXGI_FORMAT_UNKNOWN};
    int m_transferFormat{-1};
    UINT m_scalerEyeWidth{0};
    int m_scalerFormat{-1};
    int m_scalerColorSpace{-1};
    int m_scalerSourceRange{-1};
    UINT m_eyeStride{0};
};

} // namespace odyssey
