#include "Nv12ToRgba.h"

#include <d3dcompiler.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace odyssey {
namespace {

constexpr std::size_t kSwscaleTailPadding = 64;

const char* kVertexShaderSource = R"(
struct Output { float4 position : SV_Position; };
Output main(uint vertexId : SV_VertexID) {
    Output output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

const char* kPixelShaderSource = R"(
Texture2D<float> yPlane : register(t0);
Texture2D<float2> uvPlane : register(t1);
SamplerState linearClamp : register(s0);

cbuffer Parameters : register(b0) {
    float4 geometry;
    float4 range;
    float4 coefficients;
};

float4 main(float4 position : SV_Position) : SV_Target {
    uint2 pixel = uint2(position.xy);
    uint yWidth, yHeight;
    uint uvWidth, uvHeight;
    yPlane.GetDimensions(yWidth, yHeight);
    uvPlane.GetDimensions(uvWidth, uvHeight);

    float y = (yPlane.Load(int3(pixel, 0)) - range.x) * range.y;
    float2 uv = (float2(pixel) + 0.5) / float2(yWidth, yHeight);

    float eyeStart = 0.0;
    float eyeEnd = geometry.x;
    if (geometry.w > 0.5) {
        float eyeIndex = pixel.x >= geometry.z ? 1.0 : 0.0;
        eyeStart = eyeIndex * geometry.z;
        eyeEnd = eyeStart + geometry.z;
    }
    float2 minimumUv = float2((eyeStart * 0.5 + 0.5) / uvWidth, 0.5 / uvHeight);
    float2 maximumUv = float2(((eyeEnd * 0.5 - 1.0) + 0.5) / uvWidth,
                              ((geometry.y * 0.5 - 1.0) + 0.5) / uvHeight);
    uv = clamp(uv, minimumUv, maximumUv);

    float2 chroma = (uvPlane.SampleLevel(linearClamp, uv, 0) - range.z) * range.w;
    float3 rgb;
    rgb.r = y + coefficients.x * chroma.y;
    rgb.g = y + coefficients.y * chroma.x + coefficients.z * chroma.y;
    rgb.b = y + coefficients.w * chroma.x;
    return float4(saturate(rgb), 1.0);
}
)";

struct ShaderParameters {
    float geometry[4];
    float range[4];
    float coefficients[4];
};

Microsoft::WRL::ComPtr<ID3DBlob> compileShader(
    const char* source, const char* entry, const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
        &errors);
    if (FAILED(result)) {
        std::string message = "D3DCompile failed";
        if (errors) {
            message.append(": ");
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return code;
}

void checkHr(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

std::runtime_error ffmpegError(const char* operation, int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(error, message.data(), message.size());
    return std::runtime_error(std::string(operation) + " failed: " + message.data());
}

bool isSupportedFormat(AVPixelFormat format) {
    return format == AV_PIX_FMT_YUV420P
        || format == AV_PIX_FMT_NV12
        || format == AV_PIX_FMT_YUV420P10LE
        || format == AV_PIX_FMT_P010LE;
}

bool hasValidSoftwarePlanes(const AVFrame* frame, AVPixelFormat format) {
    const int width = frame->width;
    if (!frame->data[0] || !frame->data[1]) {
        return false;
    }
    switch (format) {
    case AV_PIX_FMT_YUV420P:
        return frame->data[2] && frame->linesize[0] >= width
            && frame->linesize[1] >= width / 2 && frame->linesize[2] >= width / 2;
    case AV_PIX_FMT_NV12:
        return frame->linesize[0] >= width && frame->linesize[1] >= width;
    case AV_PIX_FMT_YUV420P10LE:
        return frame->data[2] && frame->linesize[0] >= width * 2
            && frame->linesize[1] >= width && frame->linesize[2] >= width;
    case AV_PIX_FMT_P010LE:
        return frame->linesize[0] >= width * 2 && frame->linesize[1] >= width * 2;
    default:
        return false;
    }
}

int swsColorSpace(AVColorSpace colorSpace, UINT height) {
    switch (colorSpace) {
    case AVCOL_SPC_BT709:
        return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return SWS_CS_ITU601;
    case AVCOL_SPC_BT2020_NCL:
        return SWS_CS_BT2020;
    case AVCOL_SPC_UNSPECIFIED:
    case AVCOL_SPC_RESERVED:
        // Unspecified SD video conventionally uses BT.601; HD uses BT.709.
        return height <= 576 ? SWS_CS_ITU601 : SWS_CS_ITU709;
    default:
        throw std::runtime_error("YUV colorspace matrix is unsupported");
    }
}

bool hasSupportedSdrTransfer(AVColorTransferCharacteristic transfer) {
    return transfer == AVCOL_TRC_UNSPECIFIED
        || transfer == AVCOL_TRC_RESERVED0
        || transfer == AVCOL_TRC_BT709
        || transfer == AVCOL_TRC_GAMMA22
        || transfer == AVCOL_TRC_GAMMA28
        || transfer == AVCOL_TRC_SMPTE170M
        || transfer == AVCOL_TRC_IEC61966_2_1;
}

bool hasSupportedSdrPrimaries(AVColorPrimaries primaries) {
    return primaries == AVCOL_PRI_UNSPECIFIED
        || primaries == AVCOL_PRI_RESERVED0
        || primaries == AVCOL_PRI_BT709
        || primaries == AVCOL_PRI_BT470M
        || primaries == AVCOL_PRI_BT470BG
        || primaries == AVCOL_PRI_SMPTE170M;
}

} // namespace

void Nv12ToRgba::FrameDeleter::operator()(AVFrame* frame) const noexcept {
    av_frame_free(&frame);
}

void Nv12ToRgba::SwsDeleter::operator()(SwsContext* context) const noexcept {
    sws_freeContext(context);
}

Nv12ToRgba::Nv12ToRgba(ID3D11Device* device, UINT logicalWidth, UINT logicalHeight)
    : m_device(device), m_width(logicalWidth), m_height(logicalHeight) {
    if (!device || logicalWidth == 0 || logicalHeight == 0) {
        throw std::invalid_argument("Nv12ToRgba requires a device and non-zero dimensions");
    }
    if (logicalWidth > (std::numeric_limits<UINT>::max)() / 4
        || static_cast<std::size_t>(logicalWidth) * logicalHeight
            > (std::numeric_limits<std::size_t>::max)() / 4) {
        throw std::invalid_argument("Nv12ToRgba dimensions are too large");
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = logicalWidth;
    textureDesc.Height = logicalHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture;
    checkHr(device->CreateTexture2D(&textureDesc, nullptr, &outputTexture),
            "CreateTexture2D(RGBA output)");

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputRtv;
    checkHr(device->CreateRenderTargetView(outputTexture.Get(), &rtvDesc, &outputRtv),
            "CreateRenderTargetView(RGBA output)");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputSrv;
    checkHr(device->CreateShaderResourceView(outputTexture.Get(), &srvDesc, &outputSrv),
            "CreateShaderResourceView(RGBA output)");

    const auto vertexCode = compileShader(kVertexShaderSource, "main", "vs_5_0");
    const auto pixelCode = compileShader(kPixelShaderSource, "main", "ps_5_0");
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    checkHr(device->CreateVertexShader(
                vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
                &vertexShader),
            "CreateVertexShader(NV12 conversion)");
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    checkHr(device->CreatePixelShader(
                pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr,
                &pixelShader),
            "CreatePixelShader(NV12 conversion)");

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    checkHr(device->CreateSamplerState(&samplerDesc, &sampler),
            "CreateSamplerState(NV12 conversion)");

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(ShaderParameters);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    checkHr(device->CreateBuffer(&bufferDesc, nullptr, &constants),
            "CreateBuffer(NV12 conversion constants)");

    std::unique_ptr<AVFrame, FrameDeleter> transferFrame(av_frame_alloc());
    if (!transferFrame) {
        throw std::runtime_error("av_frame_alloc failed for hardware transfer");
    }

    m_rgba.resize(static_cast<std::size_t>(logicalWidth) * logicalHeight * 4);
    m_outputTexture = std::move(outputTexture);
    m_outputRtv = std::move(outputRtv);
    m_outputSrv = std::move(outputSrv);
    m_vertexShader = std::move(vertexShader);
    m_pixelShader = std::move(pixelShader);
    m_sampler = std::move(sampler);
    m_constants = std::move(constants);
    m_transferFrame = std::move(transferFrame);
}

Nv12ToRgba::~Nv12ToRgba() = default;

void Nv12ToRgba::ensureYuvTexture(
    UINT codedWidth, UINT codedHeight, DXGI_FORMAT format) {
    if (m_yuvTexture && codedWidth == m_stagingWidth && codedHeight == m_stagingHeight
        && format == m_stagingFormat) {
        return;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = codedWidth;
    textureDesc.Height = codedHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    checkHr(m_device->CreateTexture2D(&textureDesc, nullptr, &texture),
            "CreateTexture2D(YUV upload)");

    D3D11_SHADER_RESOURCE_VIEW_DESC yDesc{};
    yDesc.Format = format == DXGI_FORMAT_P010
        ? DXGI_FORMAT_R16_UNORM
        : DXGI_FORMAT_R8_UNORM;
    yDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    yDesc.Texture2D.MipLevels = 1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yPlane;
    checkHr(m_device->CreateShaderResourceView(texture.Get(), &yDesc, &yPlane),
            "CreateShaderResourceView(YUV Y plane)");

    D3D11_SHADER_RESOURCE_VIEW_DESC uvDesc{};
    uvDesc.Format = format == DXGI_FORMAT_P010
        ? DXGI_FORMAT_R16G16_UNORM
        : DXGI_FORMAT_R8G8_UNORM;
    uvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uvDesc.Texture2D.MipLevels = 1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uvPlane;
    checkHr(m_device->CreateShaderResourceView(texture.Get(), &uvDesc, &uvPlane),
            "CreateShaderResourceView(YUV UV plane)");

    m_yuvTexture = std::move(texture);
    m_yPlaneSrv = std::move(yPlane);
    m_uvPlaneSrv = std::move(uvPlane);
    m_stagingWidth = codedWidth;
    m_stagingHeight = codedHeight;
    m_stagingFormat = format;
}

bool Nv12ToRgba::convertHardwareNv12(
    ID3D11DeviceContext* context,
    const AVFrame* frame,
    StereoMode mode,
    int colorSpace,
    int sourceRange) {
    if (frame->format != AV_PIX_FMT_D3D11 || !frame->data[0]) {
        return false;
    }

    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const UINT slice = static_cast<UINT>(reinterpret_cast<std::uintptr_t>(frame->data[1]));
    D3D11_TEXTURE2D_DESC sourceDesc{};
    texture->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_NV12) {
        return false;
    }
    if (slice >= sourceDesc.ArraySize || sourceDesc.Width < m_width
        || sourceDesc.Height < m_height || (sourceDesc.Width % 2) != 0
        || (sourceDesc.Height % 2) != 0) {
        throw std::invalid_argument("D3D11 NV12 frame has invalid coded geometry");
    }

    ensureYuvTexture(sourceDesc.Width, sourceDesc.Height, DXGI_FORMAT_NV12);
    context->CopySubresourceRegion(
        m_yuvTexture.Get(), 0, 0, 0, 0, texture, slice, nullptr);

    renderYuv(context, mode, colorSpace, sourceRange, 8);
    return true;
}

bool Nv12ToRgba::convertSoftwareSemiPlanar(
    ID3D11DeviceContext* context,
    const AVFrame* frame,
    StereoMode mode,
    int colorSpace,
    int sourceRange) {
    const auto format = static_cast<AVPixelFormat>(frame->format);
    if (format != AV_PIX_FMT_NV12 && format != AV_PIX_FMT_P010LE) {
        return false;
    }

    const UINT bytesPerComponent = format == AV_PIX_FMT_P010LE ? 2u : 1u;
    const UINT rowPitch = m_width * bytesPerComponent;
    const std::size_t yBytes = static_cast<std::size_t>(rowPitch) * m_height;
    const std::size_t uvBytes = static_cast<std::size_t>(rowPitch) * (m_height / 2);
    m_yuvUpload.resize(yBytes + uvBytes);
    for (UINT row = 0; row < m_height; ++row) {
        std::memcpy(
            m_yuvUpload.data() + static_cast<std::size_t>(row) * rowPitch,
            frame->data[0] + static_cast<std::size_t>(row) * frame->linesize[0],
            rowPitch);
    }
    for (UINT row = 0; row < m_height / 2; ++row) {
        std::memcpy(
            m_yuvUpload.data() + yBytes + static_cast<std::size_t>(row) * rowPitch,
            frame->data[1] + static_cast<std::size_t>(row) * frame->linesize[1],
            rowPitch);
    }

    const DXGI_FORMAT textureFormat = format == AV_PIX_FMT_P010LE
        ? DXGI_FORMAT_P010
        : DXGI_FORMAT_NV12;
    ensureYuvTexture(m_width, m_height, textureFormat);
    context->UpdateSubresource(
        m_yuvTexture.Get(), 0, nullptr, m_yuvUpload.data(), rowPitch,
        static_cast<UINT>(m_yuvUpload.size()));
    renderYuv(
        context, mode, colorSpace, sourceRange,
        format == AV_PIX_FMT_P010LE ? 10u : 8u);
    return true;
}

void Nv12ToRgba::renderYuv(
    ID3D11DeviceContext* context,
    StereoMode mode,
    int colorSpace,
    int sourceRange,
    unsigned bitDepth) {

    float kr = 0.2126f;
    float kb = 0.0722f;
    if (colorSpace == SWS_CS_ITU601) {
        kr = 0.2990f;
        kb = 0.1140f;
    } else if (colorSpace == SWS_CS_BT2020) {
        kr = 0.2627f;
        kb = 0.0593f;
    }
    const float kg = 1.0f - kr - kb;
    const bool sideBySide = mode != StereoMode::Mono2D;
    ShaderParameters parameters{};
    parameters.geometry[0] = static_cast<float>(m_width);
    parameters.geometry[1] = static_cast<float>(m_height);
    parameters.geometry[2] = static_cast<float>(sideBySide ? m_width / 2 : m_width);
    parameters.geometry[3] = sideBySide ? 1.0f : 0.0f;
    if (bitDepth == 10) {
        constexpr float denominator = 65535.0f;
        constexpr float storedScale = 64.0f;
        parameters.range[0] = sourceRange != 0
            ? 0.0f
            : 64.0f * storedScale / denominator;
        parameters.range[1] = sourceRange != 0
            ? denominator / (1023.0f * storedScale)
            : denominator / (876.0f * storedScale);
        parameters.range[2] = 512.0f * storedScale / denominator;
        parameters.range[3] = sourceRange != 0
            ? denominator / (1023.0f * storedScale)
            : denominator / (896.0f * storedScale);
    } else {
        parameters.range[0] = sourceRange != 0 ? 0.0f : 16.0f / 255.0f;
        parameters.range[1] = sourceRange != 0 ? 1.0f : 255.0f / 219.0f;
        parameters.range[2] = 128.0f / 255.0f;
        parameters.range[3] = sourceRange != 0 ? 1.0f : 255.0f / 224.0f;
    }
    parameters.coefficients[0] = 2.0f * (1.0f - kr);
    parameters.coefficients[1] = -2.0f * kb * (1.0f - kb) / kg;
    parameters.coefficients[2] = -2.0f * kr * (1.0f - kr) / kg;
    parameters.coefficients[3] = 2.0f * (1.0f - kb);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    checkHr(context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
            "Map(NV12 conversion constants)");
    std::memcpy(mapped.pData, &parameters, sizeof(parameters));
    context->Unmap(m_constants.Get(), 0);

    ID3D11ShaderResourceView* resources[] = {m_yPlaneSrv.Get(), m_uvPlaneSrv.Get()};
    ID3D11SamplerState* samplers[] = {m_sampler.Get()};
    ID3D11Buffer* constants[] = {m_constants.Get()};
    ID3D11RenderTargetView* renderTarget = m_outputRtv.Get();
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 2, resources);
    context->PSSetSamplers(0, 1, samplers);
    context->PSSetConstantBuffers(0, 1, constants);
    context->RSSetState(nullptr);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffU);
    context->OMSetDepthStencilState(nullptr, 0);
    context->OMSetRenderTargets(1, &renderTarget, nullptr);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* nullResources[2]{};
    context->PSSetShaderResources(0, 2, nullResources);
    renderTarget = nullptr;
    context->OMSetRenderTargets(1, &renderTarget, nullptr);
}

const AVFrame* Nv12ToRgba::softwareFrame(const AVFrame* frame) {
    const auto format = static_cast<AVPixelFormat>(frame->format);
    if (format != AV_PIX_FMT_D3D11) {
        return frame;
    }
    if (!frame->hw_frames_ctx || !frame->hw_frames_ctx->data) {
        throw std::invalid_argument("D3D11 frame has no hardware frame context");
    }

    const auto* hardwareFrames =
        reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    const AVPixelFormat transferFormat = hardwareFrames->sw_format;
    if (!isSupportedFormat(transferFormat)) {
        throw std::runtime_error("D3D11 frame transfer format is unsupported");
    }

    const UINT width = static_cast<UINT>(frame->width);
    const UINT height = static_cast<UINT>(frame->height);
    if (m_transferFormat != transferFormat || m_transferWidth != width
        || m_transferHeight != height) {
        av_frame_unref(m_transferFrame.get());
        m_transferFrame->format = transferFormat;
        m_transferFrame->width = frame->width;
        m_transferFrame->height = frame->height;
        const int bufferResult = av_frame_get_buffer(m_transferFrame.get(), 32);
        if (bufferResult < 0) {
            throw ffmpegError("av_frame_get_buffer", bufferResult);
        }
        m_transferFormat = transferFormat;
        m_transferWidth = width;
        m_transferHeight = height;
    }

    const int writableResult = av_frame_make_writable(m_transferFrame.get());
    if (writableResult < 0) {
        throw ffmpegError("av_frame_make_writable", writableResult);
    }
    const int transferResult = av_hwframe_transfer_data(m_transferFrame.get(), frame, 0);
    if (transferResult < 0) {
        throw ffmpegError("av_hwframe_transfer_data", transferResult);
    }
    return m_transferFrame.get();
}

void Nv12ToRgba::configureScaler(
    int pixelFormat, UINT eyeWidth, int colorSpace, int sourceRange) {
    if (m_scaler && m_scalerFormat == pixelFormat && m_scalerEyeWidth == eyeWidth
        && m_scalerColorSpace == colorSpace && m_scalerSourceRange == sourceRange) {
        return;
    }

    std::unique_ptr<SwsContext, SwsDeleter> scaler(sws_getContext(
        static_cast<int>(eyeWidth), static_cast<int>(m_height),
        static_cast<AVPixelFormat>(pixelFormat), static_cast<int>(eyeWidth),
        static_cast<int>(m_height), AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
        nullptr));
    if (!scaler) {
        throw std::runtime_error("sws_getContext failed for YUV conversion");
    }

    const int* coefficients = sws_getCoefficients(colorSpace);
    const int colorResult = sws_setColorspaceDetails(
        scaler.get(), coefficients, sourceRange, coefficients, 1, 0, 1 << 16, 1 << 16);
    if (colorResult < 0) {
        throw ffmpegError("sws_setColorspaceDetails", colorResult);
    }

    m_scaler = std::move(scaler);
    m_scalerFormat = pixelFormat;
    m_scalerEyeWidth = eyeWidth;
    m_scalerColorSpace = colorSpace;
    m_scalerSourceRange = sourceRange;
    m_eyeStride = (eyeWidth * 4 + 31) & ~31U;
    // libswscale's vectorized writers can touch a padded tail on narrow rows.
    m_eyeRgba.resize(
        static_cast<std::size_t>(m_eyeStride) * m_height + kSwscaleTailPadding);
}

void Nv12ToRgba::convertEye(const AVFrame* frame, UINT sourceX, UINT destinationX) {
    std::array<const std::uint8_t*, 4> source{
        frame->data[0], frame->data[1], frame->data[2], frame->data[3]};
    const auto format = static_cast<AVPixelFormat>(frame->format);
    switch (format) {
    case AV_PIX_FMT_YUV420P:
        source[0] += sourceX;
        source[1] += sourceX / 2;
        source[2] += sourceX / 2;
        break;
    case AV_PIX_FMT_NV12:
        source[0] += sourceX;
        source[1] += sourceX;
        break;
    case AV_PIX_FMT_YUV420P10LE:
        source[0] += sourceX * 2;
        source[1] += sourceX;
        source[2] += sourceX;
        break;
    case AV_PIX_FMT_P010LE:
        source[0] += sourceX * 2;
        source[1] += sourceX * 2;
        break;
    default:
        throw std::runtime_error("Unsupported software pixel format");
    }

    std::array<std::uint8_t*, 4> destination{m_eyeRgba.data(), nullptr, nullptr, nullptr};
    const std::array<int, 4> destinationStride{static_cast<int>(m_eyeStride), 0, 0, 0};
    const int convertedRows = sws_scale(
        m_scaler.get(), source.data(), frame->linesize, 0, static_cast<int>(m_height),
        destination.data(), destinationStride.data());
    if (convertedRows != static_cast<int>(m_height)) {
        throw std::runtime_error("sws_scale did not convert the complete frame");
    }
    for (UINT row = 0; row < m_height; ++row) {
        std::memcpy(
            m_rgba.data() + (static_cast<std::size_t>(row) * m_width + destinationX) * 4,
            m_eyeRgba.data() + static_cast<std::size_t>(row) * m_eyeStride,
            static_cast<std::size_t>(m_scalerEyeWidth) * 4);
    }
}

void Nv12ToRgba::convert(
    ID3D11DeviceContext* context, const AVFrame* frame, StereoMode mode) {
    if (!context || !frame) {
        throw std::invalid_argument("Nv12ToRgba::convert requires a context and frame");
    }
    if (frame->width <= 0 || frame->height <= 0
        || static_cast<UINT>(frame->width) != m_width
        || static_cast<UINT>(frame->height) != m_height) {
        throw std::invalid_argument("Frame dimensions do not match the converter");
    }
    if (frame->color_trc == AVCOL_TRC_SMPTE2084
        || frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        throw std::runtime_error("PQ and HLG video require tone mapping, which is unsupported");
    }
    if (!hasSupportedSdrTransfer(frame->color_trc)) {
        throw std::runtime_error("Video transfer characteristic is unsupported");
    }
    if (!hasSupportedSdrPrimaries(frame->color_primaries)) {
        throw std::runtime_error(
            "Video primaries require gamut conversion, which is unsupported");
    }
    if ((m_width % 2) != 0 || (m_height % 2) != 0) {
        throw std::invalid_argument("4:2:0 frames require even width and height");
    }

    const bool sideBySide = mode != StereoMode::Mono2D;
    if (sideBySide && (m_width % 4) != 0) {
        throw std::invalid_argument(
            "Side-by-side 4:2:0 frames require chroma-aligned eye widths");
    }
    const UINT eyeWidth = sideBySide ? m_width / 2 : m_width;
    const int colorSpace = swsColorSpace(frame->colorspace, m_height);
    const int sourceRange = frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    if (convertHardwareNv12(context, frame, mode, colorSpace, sourceRange)) {
        return;
    }

    const AVFrame* source = softwareFrame(frame);
    const auto format = static_cast<AVPixelFormat>(source->format);
    if (!isSupportedFormat(format)) {
        throw std::runtime_error("Unsupported software pixel format");
    }
    if (!hasValidSoftwarePlanes(source, format)) {
        throw std::invalid_argument("Software frame has invalid plane storage");
    }
    if (convertSoftwareSemiPlanar(
            context, source, mode, colorSpace, sourceRange)) {
        return;
    }
    configureScaler(source->format, eyeWidth, colorSpace, sourceRange);

    convertEye(source, 0, 0);
    if (sideBySide) {
        convertEye(source, eyeWidth, eyeWidth);
    }
    context->UpdateSubresource(
        m_outputTexture.Get(), 0, nullptr, m_rgba.data(), m_width * 4, 0);
}

} // namespace odyssey
