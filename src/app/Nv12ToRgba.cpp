#include "Nv12ToRgba.h"

#include <d3dcompiler.h>
#include <stdexcept>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace odyssey {

namespace {

// Fullscreen triangle VS using SV_VertexID. No vertex buffer, no IA layout.
static const char* kVsHlsl = R"(
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut main(uint vid : SV_VertexID) {
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

// PS samples the NV12 staging texture via two SRVs (Y plane = R8_UNORM,
// UV plane = R8G8_UNORM at half size) and returns BT.709 limited-range
// decoded RGB. cbUvScale.xy = visibleWidth/codedWidth.
static const char* kPsHlsl = R"(
Texture2D<float>  tY  : register(t0);
Texture2D<float2> tUV : register(t1);
SamplerState      samp: register(s0);

cbuffer Params : register(b0) {
    float2 uvScale;
    float2 _pad;
};

struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float3 yuv709(float y, float u, float v) {
    y = (y - 16.0/255.0) * (255.0/219.0);
    u = u - 128.0/255.0;
    v = v - 128.0/255.0;
    float3 rgb;
    rgb.r = y + 1.5748 * v;
    rgb.g = y - 0.1873 * u - 0.4681 * v;
    rgb.b = y + 1.8556 * u;
    return saturate(rgb);
}

float4 main(VsOut i) : SV_Target {
    float2 uv = i.uv * uvScale;
    float  y  = tY.Sample (samp, uv).r;
    float2 c  = tUV.Sample(samp, uv).rg;
    return float4(yuv709(y, c.r, c.g), 1.0);
}
)";

struct CbParams {
    float uvScale[2];
    float _pad[2];
};

static ID3DBlob* compile(const char* src, const char* entry, const char* target) {
    ID3DBlob* code{}; ID3DBlob* err{};
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, target,
                            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                            0, &code, &err);
    if (FAILED(hr)) {
        std::string msg = "D3DCompile failed: ";
        if (err) { msg.append((const char*)err->GetBufferPointer(), err->GetBufferSize()); err->Release(); }
        throw std::runtime_error(msg);
    }
    return code;
}

} // namespace

Nv12ToRgba::Nv12ToRgba(ID3D11Device* device, UINT logicalWidth, UINT logicalHeight)
    : m_width(logicalWidth), m_height(logicalHeight), m_device(device)
{
    compileShaders(device);
    createTargets(device);
}

Nv12ToRgba::~Nv12ToRgba() {
    if (m_uvPlaneSrv) m_uvPlaneSrv->Release();
    if (m_yPlaneSrv)  m_yPlaneSrv->Release();
    if (m_nv12Tex)    m_nv12Tex->Release();
    if (m_outSrv)     m_outSrv->Release();
    if (m_outRtv)     m_outRtv->Release();
    if (m_outTex)     m_outTex->Release();
    if (m_cb)         m_cb->Release();
    if (m_sampler)    m_sampler->Release();
    if (m_ps)         m_ps->Release();
    if (m_vs)         m_vs->Release();
}

void Nv12ToRgba::compileShaders(ID3D11Device* device) {
    ID3DBlob* vs = compile(kVsHlsl, "main", "vs_5_0");
    ID3DBlob* ps = compile(kPsHlsl, "main", "ps_5_0");

    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs))) {
        vs->Release(); ps->Release();
        throw std::runtime_error("CreateVertexShader failed");
    }
    if (FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps))) {
        vs->Release(); ps->Release();
        throw std::runtime_error("CreatePixelShader failed");
    }
    vs->Release(); ps->Release();

    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &m_sampler)))
        throw std::runtime_error("CreateSamplerState failed");

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(CbParams);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &m_cb)))
        throw std::runtime_error("CreateBuffer(cb) failed");
}

void Nv12ToRgba::createTargets(ID3D11Device* device) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width      = m_width;
    td.Height     = m_height;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc = {1, 0};
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_outTex)))
        throw std::runtime_error("CreateTexture2D(out) failed");

    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateRenderTargetView(m_outTex, &rv, &m_outRtv)))
        throw std::runtime_error("CreateRenderTargetView(out) failed");

    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format              = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(m_outTex, &sv, &m_outSrv)))
        throw std::runtime_error("CreateShaderResourceView(out) failed");
}

void Nv12ToRgba::ensureStaging(UINT codedW, UINT codedH) {
    if (m_nv12Tex && codedW == m_stagingW && codedH == m_stagingH) return;

    if (m_uvPlaneSrv) { m_uvPlaneSrv->Release(); m_uvPlaneSrv = nullptr; }
    if (m_yPlaneSrv)  { m_yPlaneSrv->Release();  m_yPlaneSrv  = nullptr; }
    if (m_nv12Tex)    { m_nv12Tex->Release();    m_nv12Tex    = nullptr; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width      = codedW;
    td.Height     = codedH;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_NV12;
    td.SampleDesc = {1, 0};
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_nv12Tex)))
        throw std::runtime_error("CreateTexture2D(nv12 staging) failed");

    D3D11_SHADER_RESOURCE_VIEW_DESC y{};
    y.Format              = DXGI_FORMAT_R8_UNORM;
    y.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    y.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateShaderResourceView(m_nv12Tex, &y, &m_yPlaneSrv)))
        throw std::runtime_error("CreateShaderResourceView(Y) failed");

    D3D11_SHADER_RESOURCE_VIEW_DESC uv{};
    uv.Format              = DXGI_FORMAT_R8G8_UNORM;
    uv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    uv.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateShaderResourceView(m_nv12Tex, &uv, &m_uvPlaneSrv)))
        throw std::runtime_error("CreateShaderResourceView(UV) failed");

    m_stagingW = codedW;
    m_stagingH = codedH;
}

void Nv12ToRgba::convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* coded, UINT slice) {
    D3D11_TEXTURE2D_DESC cd{};
    coded->GetDesc(&cd);

    ensureStaging(cd.Width, cd.Height);

    ctx->CopySubresourceRegion(m_nv12Tex, 0, 0, 0, 0, coded, slice, nullptr);

    // Update CB with visible-rect UV scale. coded width/height match the
    // staging texture, so the scale = 1.0 in the common case; we still send
    // it so the shader handles padded coded sizes the same way for any
    // future codec that aligns differently.
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        CbParams p{};
        p.uvScale[0] = (cd.Width  > 0) ? float(m_width)  / float(cd.Width)  : 1.0f;
        p.uvScale[1] = (cd.Height > 0) ? float(m_height) / float(cd.Height) : 1.0f;
        memcpy(m.pData, &p, sizeof(p));
        ctx->Unmap(m_cb, 0);
    }

    ID3D11ShaderResourceView* srvs[2] = { m_yPlaneSrv, m_uvPlaneSrv };
    ID3D11Buffer*             cbs[1]  = { m_cb };
    ID3D11SamplerState*       samps[1]= { m_sampler };

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_vs, nullptr, 0);
    ctx->PSSetShader(m_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, samps);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    D3D11_VIEWPORT vp{};
    vp.Width    = (FLOAT)m_width;
    vp.Height   = (FLOAT)m_height;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &m_outRtv, nullptr);

    ctx->Draw(3, 0);

#if 0  // diagnostic — enable to dump info-queue messages on first frame
    static int callIdx = 0;
    if (callIdx++ == 0) {
        ID3D11InfoQueue* iq{};
        if (SUCCEEDED(m_device->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&iq))) {
            UINT64 cnt = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < cnt; ++i) {
                SIZE_T sz = 0;
                iq->GetMessage(i, nullptr, &sz);
                std::vector<char> buf(sz);
                D3D11_MESSAGE* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
                if (SUCCEEDED(iq->GetMessage(i, msg, &sz))) {
                    OutputDebugStringA(msg->pDescription);
                    OutputDebugStringA("\n");
                }
            }
            iq->ClearStoredMessages();
            iq->Release();
        }
    }
#endif

    ID3D11ShaderResourceView* nullSrvs[2]{};
    ctx->PSSetShaderResources(0, 2, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
}

} // namespace odyssey
