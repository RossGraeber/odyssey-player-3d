#include "StereoCompositor.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace odyssey {

namespace {

using Microsoft::WRL::ComPtr;

constexpr char kVertexShader[] = R"(
struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut main(uint vertexId : SV_VertexID) {
    VsOut output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = uv;
    output.position = float4(
        uv * float2(2.0, -2.0) + float2(-1.0, 1.0),
        0.0,
        1.0);
    return output;
}
)";

constexpr char kPixelShader[] = R"(
Texture2D<float4> inputTexture : register(t0);
SamplerState inputSampler : register(s0);

cbuffer Parameters : register(b0) {
    float4 sourceRect;
    float4 sampleBounds;
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VsOut input) : SV_Target {
    float2 uv = lerp(sourceRect.xy, sourceRect.zw, input.uv);
    uv = clamp(uv, sampleBounds.xy, sampleBounds.zw);
    return inputTexture.Sample(inputSampler, uv);
}
)";

constexpr char kOverlayPixelShader[] = R"(
Texture2D<float4> overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VsOut input) : SV_Target {
    return overlayTexture.Sample(overlaySampler, input.uv);
}
)";

struct ShaderParameters {
    float sourceRect[4];
    float sampleBounds[4];
};

void checkHr(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char message[160]{};
        sprintf_s(
            message,
            "Stereo compositor %s failed (hr=0x%08lX)",
            operation,
            static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}

ComPtr<ID3DBlob> compileShader(const char* source, const char* target) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        "main",
        target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &code,
        &errors);
    if (FAILED(result)) {
        std::string message = "Stereo compositor shader compilation failed";
        if (errors) {
            message.append(": ");
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return code;
}

bool validRect(const NormalizedRect& rect) {
    return std::isfinite(rect.left)
        && std::isfinite(rect.top)
        && std::isfinite(rect.right)
        && std::isfinite(rect.bottom)
        && rect.left >= 0.0
        && rect.top >= 0.0
        && rect.right <= 1.0
        && rect.bottom <= 1.0
        && rect.left < rect.right
        && rect.top < rect.bottom;
}

} // namespace

StereoCompositor::StereoCompositor(ID3D11Device* device)
    : m_device(device) {
    if (!m_device) {
        throw std::invalid_argument("Stereo compositor requires a D3D11 device");
    }
    createShaders();
}

void StereoCompositor::createShaders() {
    const ComPtr<ID3DBlob> vertexCode = compileShader(kVertexShader, "vs_5_0");
    const ComPtr<ID3DBlob> pixelCode = compileShader(kPixelShader, "ps_5_0");
    const ComPtr<ID3DBlob> overlayPixelCode =
        compileShader(kOverlayPixelShader, "ps_5_0");
    checkHr(
        m_device->CreateVertexShader(
            vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(),
            nullptr,
            &m_vertexShader),
        "CreateVertexShader");
    checkHr(
        m_device->CreatePixelShader(
            pixelCode->GetBufferPointer(),
            pixelCode->GetBufferSize(),
            nullptr,
            &m_pixelShader),
        "CreatePixelShader");
    checkHr(
        m_device->CreatePixelShader(
            overlayPixelCode->GetBufferPointer(),
            overlayPixelCode->GetBufferSize(),
            nullptr,
            &m_overlayPixelShader),
        "CreatePixelShader(overlay)");

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    checkHr(
        m_device->CreateSamplerState(&samplerDesc, &m_sampler),
        "CreateSamplerState");

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(ShaderParameters);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    checkHr(
        m_device->CreateBuffer(&bufferDesc, nullptr, &m_constants),
        "CreateBuffer");

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    checkHr(m_device->CreateBlendState(&blendDesc, &m_alphaBlend), "CreateBlendState");
}

void StereoCompositor::ensureOutput(UINT perEyeWidth, UINT perEyeHeight) {
    if (perEyeWidth == 0 || perEyeHeight == 0
        || perEyeWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / 2
        || perEyeHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        throw std::invalid_argument("Stereo compositor output dimensions are invalid");
    }

    const UINT outputWidth = perEyeWidth * 2;
    if (m_outputTexture && m_width == outputWidth && m_height == perEyeHeight) {
        return;
    }

    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = outputWidth;
    textureDesc.Height = perEyeHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    checkHr(
        m_device->CreateTexture2D(&textureDesc, nullptr, &texture),
        "CreateTexture2D");

    // Sampling Nv12ToRgba's sRGB SRV decodes to linear. The sRGB RTV encodes
    // once on write, preserving encoded values while retaining that converter's
    // sRGB SRV contract for the weaver.
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    checkHr(
        m_device->CreateRenderTargetView(texture.Get(), &rtvDesc, &rtv),
        "CreateRenderTargetView");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    checkHr(
        m_device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv),
        "CreateShaderResourceView");

    m_outputTexture = std::move(texture);
    m_outputRtv = std::move(rtv);
    m_outputSrv = std::move(srv);
    m_width = outputWidth;
    m_height = perEyeHeight;
}

void StereoCompositor::ensureOverlay(UINT imageWidth, UINT imageHeight) {
    if (imageWidth == 0 || imageHeight == 0
        || imageWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        || imageHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        throw std::invalid_argument("Stereo compositor overlay dimensions are invalid");
    }
    if (m_overlayTexture && m_overlayWidth == imageWidth
        && m_overlayHeight == imageHeight) {
        return;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = imageWidth;
    textureDesc.Height = imageHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DYNAMIC;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Texture2D> texture;
    checkHr(m_device->CreateTexture2D(&textureDesc, nullptr, &texture),
            "CreateTexture2D(overlay)");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    checkHr(m_device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv),
            "CreateShaderResourceView(overlay)");

    m_overlayTexture = std::move(texture);
    m_overlaySrv = std::move(srv);
    m_overlayWidth = imageWidth;
    m_overlayHeight = imageHeight;
}

void StereoCompositor::clearViews(
    ID3D11DeviceContext* context, UINT perEyeWidth, UINT perEyeHeight) {
    if (!context) {
        throw std::invalid_argument("Stereo compositor context is required");
    }
    ensureOutput(perEyeWidth, perEyeHeight);
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTargetView(m_outputRtv.Get(), clearColor);
}

void StereoCompositor::compose(
    ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* input,
    UINT sourceWidth,
    UINT sourceHeight,
    const StereoLayout& layout,
    UINT perEyeWidth,
    UINT perEyeHeight) {
    if (!context || !input || sourceWidth == 0 || sourceHeight == 0) {
        throw std::invalid_argument("Stereo compositor input is invalid");
    }
    if (!validRect(layout.left.source)
        || !validRect(layout.left.destination)
        || !validRect(layout.right.source)
        || !validRect(layout.right.destination)) {
        throw std::invalid_argument("Stereo compositor layout is invalid");
    }

    const float halfTexelX = 0.5f / static_cast<float>(sourceWidth);
    const float halfTexelY = 0.5f / static_cast<float>(sourceHeight);
    const auto sourceContainsTexel = [halfTexelX, halfTexelY](const NormalizedRect& source) {
        return static_cast<float>(source.left) + halfTexelX
                <= static_cast<float>(source.right) - halfTexelX
            && static_cast<float>(source.top) + halfTexelY
                <= static_cast<float>(source.bottom) - halfTexelY;
    };
    if (!sourceContainsTexel(layout.left.source)
        || !sourceContainsTexel(layout.right.source)) {
        throw std::invalid_argument("Stereo compositor source region is smaller than one texel");
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC inputDesc{};
    input->GetDesc(&inputDesc);
    if (inputDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        || inputDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
        throw std::invalid_argument("Stereo compositor input must be a 2D RGBA8 sRGB view");
    }

    clearViews(context, perEyeWidth, perEyeHeight);
    ID3D11RenderTargetView* renderTarget = m_outputRtv.Get();
    context->OMSetRenderTargets(1, &renderTarget, nullptr);
    context->OMSetBlendState(nullptr, nullptr, (std::numeric_limits<UINT>::max)());
    context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* shaderInput = input;
    context->PSSetShaderResources(0, 1, &shaderInput);
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* constants = m_constants.Get();
    context->PSSetConstantBuffers(0, 1, &constants);

    const auto drawEye = [&](const EyeLayout& eye, UINT eyeOffset) {
        ShaderParameters parameters{
            {
                static_cast<float>(eye.source.left),
                static_cast<float>(eye.source.top),
                static_cast<float>(eye.source.right),
                static_cast<float>(eye.source.bottom),
            },
            {
                static_cast<float>(eye.source.left) + halfTexelX,
                static_cast<float>(eye.source.top) + halfTexelY,
                static_cast<float>(eye.source.right) - halfTexelX,
                static_cast<float>(eye.source.bottom) - halfTexelY,
            },
        };
        context->UpdateSubresource(m_constants.Get(), 0, nullptr, &parameters, 0, 0);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(eyeOffset)
            + static_cast<float>(eye.destination.left * perEyeWidth);
        viewport.TopLeftY = static_cast<float>(eye.destination.top * perEyeHeight);
        viewport.Width = static_cast<float>(
            (eye.destination.right - eye.destination.left) * perEyeWidth);
        viewport.Height = static_cast<float>(
            (eye.destination.bottom - eye.destination.top) * perEyeHeight);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->Draw(3, 0);
    };

    drawEye(layout.left, 0);
    drawEye(layout.right, perEyeWidth);

    ID3D11ShaderResourceView* nullInput = nullptr;
    context->PSSetShaderResources(0, 1, &nullInput);
    ID3D11RenderTargetView* nullTarget = nullptr;
    context->OMSetRenderTargets(1, &nullTarget, nullptr);
}

void StereoCompositor::drawOverlay(
    ID3D11DeviceContext* context,
    const std::uint8_t* bgra,
    UINT imageWidth,
    UINT imageHeight,
    const RECT& destination,
    UINT canvasWidth,
    UINT canvasHeight) {
    const std::int64_t destinationWidth =
        static_cast<std::int64_t>(destination.right) - destination.left;
    const std::int64_t destinationHeight =
        static_cast<std::int64_t>(destination.bottom) - destination.top;
    if (!context || !bgra || !m_outputRtv || canvasWidth == 0 || canvasHeight == 0
        || destination.left < 0 || destination.top < 0
        || destinationWidth <= 0 || destinationHeight <= 0
        || static_cast<std::uint64_t>(destination.right) > canvasWidth
        || static_cast<std::uint64_t>(destination.bottom) > canvasHeight) {
        throw std::invalid_argument("Stereo compositor overlay input is invalid");
    }
    ensureOverlay(imageWidth, imageHeight);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    checkHr(context->Map(
                m_overlayTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
            "Map(overlay)");
    const std::size_t sourcePitch = static_cast<std::size_t>(imageWidth) * 4;
    for (UINT row = 0; row < imageHeight; ++row) {
        std::memcpy(
            static_cast<std::uint8_t*>(mapped.pData)
                + static_cast<std::size_t>(row) * mapped.RowPitch,
            bgra + static_cast<std::size_t>(row) * sourcePitch,
            sourcePitch);
    }
    context->Unmap(m_overlayTexture.Get(), 0);

    ID3D11RenderTargetView* renderTarget = m_outputRtv.Get();
    context->OMSetRenderTargets(1, &renderTarget, nullptr);
    context->OMSetBlendState(
        m_alphaBlend.Get(), nullptr, (std::numeric_limits<UINT>::max)());
    context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(m_overlayPixelShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* overlay = m_overlaySrv.Get();
    context->PSSetShaderResources(0, 1, &overlay);
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);

    const UINT perEyeWidth = m_width / 2;
    const auto drawEye = [&](UINT eyeOffset) {
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(eyeOffset)
            + static_cast<float>(destination.left) * perEyeWidth / canvasWidth;
        viewport.TopLeftY =
            static_cast<float>(destination.top) * m_height / canvasHeight;
        viewport.Width =
            static_cast<float>(destinationWidth) * perEyeWidth / canvasWidth;
        viewport.Height =
            static_cast<float>(destinationHeight) * m_height / canvasHeight;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->Draw(3, 0);
    };
    drawEye(0);
    drawEye(perEyeWidth);

    ID3D11ShaderResourceView* nullInput = nullptr;
    context->PSSetShaderResources(0, 1, &nullInput);
    context->OMSetBlendState(nullptr, nullptr, (std::numeric_limits<UINT>::max)());
    ID3D11RenderTargetView* nullTarget = nullptr;
    context->OMSetRenderTargets(1, &nullTarget, nullptr);
}

void StereoCompositor::blitLeftEye(
    ID3D11DeviceContext* context, UINT clientWidth, UINT clientHeight) {
    if (!context || !m_outputSrv || clientWidth == 0 || clientHeight == 0) {
        throw std::invalid_argument("Stereo compositor preview input is invalid");
    }

    ComPtr<ID3D11RenderTargetView> renderTarget;
    context->OMGetRenderTargets(1, renderTarget.GetAddressOf(), nullptr);
    if (!renderTarget) {
        throw std::invalid_argument("Stereo compositor preview requires a bound target");
    }
    D3D11_RENDER_TARGET_VIEW_DESC targetViewDesc{};
    renderTarget->GetDesc(&targetViewDesc);
    if (targetViewDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        || targetViewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) {
        throw std::invalid_argument(
            "Stereo compositor preview requires an RGBA8 sRGB 2D target");
    }
    ComPtr<ID3D11Resource> targetResource;
    renderTarget->GetResource(&targetResource);
    ComPtr<ID3D11Texture2D> targetTexture;
    checkHr(targetResource.As(&targetTexture), "QueryInterface(preview target)");
    D3D11_TEXTURE2D_DESC targetTextureDesc{};
    targetTexture->GetDesc(&targetTextureDesc);
    if (targetTextureDesc.Width != clientWidth
        || targetTextureDesc.Height != clientHeight) {
        throw std::invalid_argument(
            "Stereo compositor preview dimensions do not match the target");
    }
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTargetView(renderTarget.Get(), clearColor);

    const UINT perEyeWidth = m_width / 2;
    const float sourceAspect =
        static_cast<float>(perEyeWidth) / static_cast<float>(m_height);
    const float clientAspect =
        static_cast<float>(clientWidth) / static_cast<float>(clientHeight);
    float viewportWidth = static_cast<float>(clientWidth);
    float viewportHeight = static_cast<float>(clientHeight);
    if (clientAspect > sourceAspect) {
        viewportWidth = viewportHeight * sourceAspect;
    } else {
        viewportHeight = viewportWidth / sourceAspect;
    }

    ShaderParameters parameters{
        {0.0f, 0.0f, 0.5f, 1.0f},
        {
            0.5f / m_width,
            0.5f / m_height,
            (static_cast<float>(perEyeWidth) - 0.5f) / m_width,
            (static_cast<float>(m_height) - 0.5f) / m_height,
        },
    };
    context->UpdateSubresource(m_constants.Get(), 0, nullptr, &parameters, 0, 0);
    context->OMSetBlendState(nullptr, nullptr, (std::numeric_limits<UINT>::max)());
    context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* input = m_outputSrv.Get();
    context->PSSetShaderResources(0, 1, &input);
    ID3D11SamplerState* sampler = m_sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* constants = m_constants.Get();
    context->PSSetConstantBuffers(0, 1, &constants);

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = (static_cast<float>(clientWidth) - viewportWidth) * 0.5f;
    viewport.TopLeftY = (static_cast<float>(clientHeight) - viewportHeight) * 0.5f;
    viewport.Width = viewportWidth;
    viewport.Height = viewportHeight;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* nullInput = nullptr;
    context->PSSetShaderResources(0, 1, &nullInput);
}

ID3D11ShaderResourceView* StereoCompositor::outputSrv() const noexcept {
    return m_outputSrv.Get();
}

UINT StereoCompositor::width() const noexcept {
    return m_width;
}

UINT StereoCompositor::height() const noexcept {
    return m_height;
}

} // namespace odyssey
