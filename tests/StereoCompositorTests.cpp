#include "app/StereoCompositor.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Rgba {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

struct Bgra {
    std::uint8_t b;
    std::uint8_t g;
    std::uint8_t r;
    std::uint8_t a;
};

class StereoCompositorTest : public testing::Test {
protected:
    void SetUp() override {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0,
        };
        ASSERT_HRESULT_SUCCEEDED(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context));
    }

    ComPtr<ID3D11ShaderResourceView> createInput(
        UINT width,
        UINT height,
        const std::vector<Rgba>& pixels) {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = width * sizeof(Rgba);

        ComPtr<ID3D11Texture2D> texture;
        EXPECT_HRESULT_SUCCEEDED(device->CreateTexture2D(&textureDesc, &data, &texture));

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        ComPtr<ID3D11ShaderResourceView> srv;
        EXPECT_HRESULT_SUCCEEDED(device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv));
        return srv;
    }

    std::vector<Rgba> readOutput(ID3D11ShaderResourceView* srv, UINT width, UINT height) {
        ComPtr<ID3D11Resource> resource;
        srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> output;
        EXPECT_HRESULT_SUCCEEDED(resource.As(&output));

        D3D11_TEXTURE2D_DESC stagingDesc{};
        output->GetDesc(&stagingDesc);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> staging;
        EXPECT_HRESULT_SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging));
        context->CopyResource(staging.Get(), output.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        EXPECT_HRESULT_SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));

        std::vector<Rgba> pixels(width * height);
        for (UINT y = 0; y < height; ++y) {
            const auto* row = static_cast<const Rgba*>(static_cast<const void*>(
                static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch));
            for (UINT x = 0; x < width; ++x) {
                pixels[y * width + x] = row[x];
            }
        }
        context->Unmap(staging.Get(), 0);
        return pixels;
    }

    void createTarget(
        UINT width,
        UINT height,
        ComPtr<ID3D11RenderTargetView>& rtv,
        ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        ASSERT_HRESULT_SUCCEEDED(
            device->CreateTexture2D(&textureDesc, nullptr, &texture));

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ASSERT_HRESULT_SUCCEEDED(
            device->CreateRenderTargetView(texture.Get(), &rtvDesc, &rtv));

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        ASSERT_HRESULT_SUCCEEDED(
            device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv));
    }

    static void expectNear(const Rgba& actual, const Rgba& expected) {
        EXPECT_NEAR(actual.r, expected.r, 1);
        EXPECT_NEAR(actual.g, expected.g, 1);
        EXPECT_NEAR(actual.b, expected.b, 1);
        EXPECT_EQ(actual.a, expected.a);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

TEST_F(StereoCompositorTest, PreservesEncodedColorsAndKeepsEachEyeInsideItsBounds) {
    constexpr UINT sourceWidth = 4;
    constexpr UINT sourceHeight = 2;
    constexpr UINT perEyeWidth = 4;
    constexpr UINT perEyeHeight = 4;
    const Rgba firstEye{64, 128, 191, 255};
    const Rgba secondEye{191, 128, 64, 255};
    const Rgba black{0, 0, 0, 255};
    const std::vector<Rgba> source{
        firstEye, firstEye, secondEye, secondEye,
        firstEye, firstEye, secondEye, secondEye,
    };
    const odyssey::NormalizedRect destination{0.0, 0.25, 1.0, 0.75};
    const odyssey::StereoLayout swapped{
        {{0.5, 0.0, 1.0, 1.0}, destination},
        {{0.0, 0.0, 0.5, 1.0}, destination},
    };

    odyssey::StereoCompositor compositor(device.Get());
    const auto input = createInput(sourceWidth, sourceHeight, source);
    compositor.compose(
        context.Get(),
        input.Get(),
        sourceWidth,
        sourceHeight,
        swapped,
        perEyeWidth,
        perEyeHeight);

    ASSERT_NE(compositor.outputSrv(), nullptr);
    EXPECT_EQ(compositor.width(), 2 * perEyeWidth);
    EXPECT_EQ(compositor.height(), perEyeHeight);
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    compositor.outputSrv()->GetDesc(&srvDesc);
    EXPECT_EQ(srvDesc.Format, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

    const auto output = readOutput(compositor.outputSrv(), compositor.width(), compositor.height());
    for (UINT x = 0; x < compositor.width(); ++x) {
        expectNear(output[x], black);
        expectNear(output[3 * compositor.width() + x], black);
    }
    for (UINT y = 1; y <= 2; ++y) {
        for (UINT x = 0; x < perEyeWidth; ++x) {
            expectNear(output[y * compositor.width() + x], secondEye);
            expectNear(output[y * compositor.width() + perEyeWidth + x], firstEye);
        }
    }
}

TEST_F(StereoCompositorTest, DrawsTheSameStraightAlphaOverlayIntoBothViews) {
    constexpr UINT perEyeWidth = 4;
    constexpr UINT perEyeHeight = 4;
    const Rgba video{40, 90, 140, 255};
    const std::vector<Rgba> source(8, video);
    const odyssey::StereoLayout layout{
        {{0.0, 0.0, 0.5, 1.0}, {0.0, 0.0, 1.0, 1.0}},
        {{0.5, 0.0, 1.0, 1.0}, {0.0, 0.0, 1.0, 1.0}},
    };

    odyssey::StereoCompositor compositor(device.Get());
    const auto input = createInput(4, 2, source);
    compositor.compose(
        context.Get(), input.Get(), 4, 2, layout, perEyeWidth, perEyeHeight);

    const Bgra transparent{10, 20, 240, 0};
    const RECT fullCanvas{0, 0, 4, 4};
    compositor.drawOverlay(
        context.Get(), reinterpret_cast<const std::uint8_t*>(&transparent),
        1, 1, fullCanvas, 4, 4);
    auto output = readOutput(
        compositor.outputSrv(), compositor.width(), compositor.height());
    for (const Rgba& pixel : output) {
        expectNear(pixel, video);
    }

    const Bgra overlay{30, 100, 200, 255};
    const Rgba expectedOverlay{200, 100, 30, 255};
    const RECT destination{1, 1, 3, 3};
    compositor.drawOverlay(
        context.Get(), reinterpret_cast<const std::uint8_t*>(&overlay),
        1, 1, destination, 4, 4);
    output = readOutput(
        compositor.outputSrv(), compositor.width(), compositor.height());
    for (UINT eye = 0; eye < 2; ++eye) {
        for (UINT y = 0; y < perEyeHeight; ++y) {
            for (UINT x = 0; x < perEyeWidth; ++x) {
                const Rgba expected = x >= 1 && x < 3 && y >= 1 && y < 3
                    ? expectedOverlay
                    : video;
                expectNear(
                    output[y * compositor.width() + eye * perEyeWidth + x],
                    expected);
            }
        }
    }
}

TEST_F(StereoCompositorTest, BlitsOnlyTheLeftEyeWithAspectPreserved) {
    constexpr UINT perEyeWidth = 4;
    constexpr UINT perEyeHeight = 4;
    const Rgba left{180, 70, 20, 255};
    const Rgba right{20, 70, 180, 255};
    const Rgba black{0, 0, 0, 255};
    const std::vector<Rgba> source{
        left, left, right, right,
        left, left, right, right,
    };
    const odyssey::StereoLayout layout{
        {{0.0, 0.0, 0.5, 1.0}, {0.0, 0.0, 1.0, 1.0}},
        {{0.5, 0.0, 1.0, 1.0}, {0.0, 0.0, 1.0, 1.0}},
    };

    odyssey::StereoCompositor compositor(device.Get());
    const auto input = createInput(4, 2, source);
    compositor.compose(
        context.Get(), input.Get(), 4, 2, layout, perEyeWidth, perEyeHeight);

    ComPtr<ID3D11RenderTargetView> targetRtv;
    ComPtr<ID3D11ShaderResourceView> targetSrv;
    createTarget(8, 4, targetRtv, targetSrv);
    ID3D11RenderTargetView* target = targetRtv.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    compositor.blitLeftEye(context.Get(), 8, 4);

    const auto output = readOutput(targetSrv.Get(), 8, 4);
    for (UINT y = 0; y < 4; ++y) {
        for (UINT x = 0; x < 8; ++x) {
            expectNear(output[y * 8 + x], x >= 2 && x < 6 ? left : black);
        }
    }
}

TEST_F(StereoCompositorTest, BlendsStraightAlphaOverlaysInLinearSrgbSpace) {
    odyssey::StereoCompositor compositor(device.Get());
    compositor.clearViews(context.Get(), 2, 2);
    const RECT canvas{0, 0, 2, 2};
    const Bgra halfRed{0, 0, 255, 128};
    const Bgra halfBlue{255, 0, 0, 128};
    compositor.drawOverlay(
        context.Get(), reinterpret_cast<const std::uint8_t*>(&halfRed),
        1, 1, canvas, 2, 2);
    compositor.drawOverlay(
        context.Get(), reinterpret_cast<const std::uint8_t*>(&halfBlue),
        1, 1, canvas, 2, 2);

    const auto output = readOutput(
        compositor.outputSrv(), compositor.width(), compositor.height());
    const Rgba expected{137, 0, 188, 255};
    for (const Rgba& pixel : output) {
        EXPECT_NEAR(pixel.r, expected.r, 2);
        EXPECT_NEAR(pixel.g, expected.g, 2);
        EXPECT_NEAR(pixel.b, expected.b, 2);
        EXPECT_EQ(pixel.a, expected.a);
    }
}

} // namespace
