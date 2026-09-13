#include "app/Nv12ToRgba.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

using Microsoft::WRL::ComPtr;

namespace {

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        av_frame_free(&frame);
    }
};

using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct Rgba {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

class Nv12ToRgbaTest : public testing::Test {
protected:
    void SetUp() override {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
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

    FramePtr makeFrame(AVPixelFormat format, int width, int height) {
        FramePtr frame(av_frame_alloc());
        EXPECT_NE(frame, nullptr);
        frame->format = format;
        frame->width = width;
        frame->height = height;
        frame->color_range = AVCOL_RANGE_MPEG;
        frame->colorspace = AVCOL_SPC_BT709;
        frame->color_primaries = AVCOL_PRI_BT709;
        frame->color_trc = AVCOL_TRC_BT709;
        EXPECT_GE(av_frame_get_buffer(frame.get(), 32), 0);
        return frame;
    }

    FramePtr makeHardwareNv12(
        const AVFrame* software, ComPtr<ID3D11Texture2D>& texture) {
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(software->width) * software->height * 3 / 2);
        for (int row = 0; row < software->height; ++row) {
            std::copy_n(
                software->data[0] + row * software->linesize[0], software->width,
                pixels.data() + static_cast<std::size_t>(row) * software->width);
        }
        for (int row = 0; row < software->height / 2; ++row) {
            std::copy_n(
                software->data[1] + row * software->linesize[1], software->width,
                pixels.data() + static_cast<std::size_t>(software->width)
                    * (software->height + row));
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(software->width);
        desc.Height = static_cast<UINT>(software->height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = pixels.data();
        initial.SysMemPitch = static_cast<UINT>(software->width);
        initial.SysMemSlicePitch = static_cast<UINT>(pixels.size());
        EXPECT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, &initial, &texture));

        FramePtr hardware(av_frame_alloc());
        EXPECT_NE(hardware, nullptr);
        hardware->format = AV_PIX_FMT_D3D11;
        hardware->width = software->width;
        hardware->height = software->height;
        hardware->color_range = software->color_range;
        hardware->colorspace = software->colorspace;
        hardware->color_primaries = software->color_primaries;
        hardware->color_trc = software->color_trc;
        hardware->data[0] = reinterpret_cast<std::uint8_t*>(texture.Get());
        hardware->data[1] = nullptr;
        return hardware;
    }

    void fill420(AVFrame* frame, int y, int u, int v) {
        ASSERT_GE(av_frame_make_writable(frame), 0);
        const bool tenBit = frame->format == AV_PIX_FMT_YUV420P10LE
            || frame->format == AV_PIX_FMT_P010LE;
        const bool semiplanar = frame->format == AV_PIX_FMT_NV12
            || frame->format == AV_PIX_FMT_P010LE;
        for (int row = 0; row < frame->height; ++row) {
            if (tenBit) {
                auto* values = reinterpret_cast<std::uint16_t*>(
                    frame->data[0] + row * frame->linesize[0]);
                const std::uint16_t stored = static_cast<std::uint16_t>(
                    frame->format == AV_PIX_FMT_P010LE ? y << 6 : y);
                std::fill(values, values + frame->width, stored);
            } else {
                std::fill_n(frame->data[0] + row * frame->linesize[0], frame->width,
                            static_cast<std::uint8_t>(y));
            }
        }
        for (int row = 0; row < frame->height / 2; ++row) {
            if (semiplanar && tenBit) {
                auto* values = reinterpret_cast<std::uint16_t*>(
                    frame->data[1] + row * frame->linesize[1]);
                for (int x = 0; x < frame->width / 2; ++x) {
                    values[x * 2] = static_cast<std::uint16_t>(u << 6);
                    values[x * 2 + 1] = static_cast<std::uint16_t>(v << 6);
                }
            } else if (semiplanar) {
                auto* values = frame->data[1] + row * frame->linesize[1];
                for (int x = 0; x < frame->width / 2; ++x) {
                    values[x * 2] = static_cast<std::uint8_t>(u);
                    values[x * 2 + 1] = static_cast<std::uint8_t>(v);
                }
            } else if (tenBit) {
                auto* uValues = reinterpret_cast<std::uint16_t*>(
                    frame->data[1] + row * frame->linesize[1]);
                auto* vValues = reinterpret_cast<std::uint16_t*>(
                    frame->data[2] + row * frame->linesize[2]);
                std::fill_n(uValues, frame->width / 2, static_cast<std::uint16_t>(u));
                std::fill_n(vValues, frame->width / 2, static_cast<std::uint16_t>(v));
            } else {
                std::fill_n(frame->data[1] + row * frame->linesize[1], frame->width / 2,
                            static_cast<std::uint8_t>(u));
                std::fill_n(frame->data[2] + row * frame->linesize[2], frame->width / 2,
                            static_cast<std::uint8_t>(v));
            }
        }
    }

    std::vector<Rgba> readOutput(odyssey::Nv12ToRgba& converter) {
        ComPtr<ID3D11Resource> resource;
        converter.outputSrv()->GetResource(&resource);
        ComPtr<ID3D11Texture2D> output;
        EXPECT_HRESULT_SUCCEEDED(resource.As(&output));
        D3D11_TEXTURE2D_DESC desc{};
        output->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        EXPECT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        EXPECT_HRESULT_SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<Rgba> pixels(converter.width() * converter.height());
        for (UINT y = 0; y < converter.height(); ++y) {
            const auto* row = reinterpret_cast<const Rgba*>(
                static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch);
            std::copy_n(row, converter.width(), pixels.data() + y * converter.width());
        }
        context->Unmap(staging.Get(), 0);
        return pixels;
    }

    Rgba convertConstant(AVPixelFormat format, int y, int u, int v) {
        auto frame = makeFrame(format, 4, 2);
        fill420(frame.get(), y, u, v);
        odyssey::Nv12ToRgba converter(device.Get(), 4, 2);
        converter.convert(context.Get(), frame.get(), odyssey::StereoMode::FullSbs);
        return readOutput(converter)[0];
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

TEST_F(Nv12ToRgbaTest, ExpandsLimitedRangeBlackWhiteAndChroma) {
    const Rgba black = convertConstant(AV_PIX_FMT_YUV420P, 16, 128, 128);
    const Rgba white = convertConstant(AV_PIX_FMT_YUV420P, 235, 128, 128);
    const Rgba red709 = convertConstant(AV_PIX_FMT_YUV420P, 81, 90, 240);
    EXPECT_NEAR(black.r, 0, 2);
    EXPECT_NEAR(black.g, 0, 2);
    EXPECT_NEAR(black.b, 0, 2);
    EXPECT_NEAR(white.r, 255, 2);
    EXPECT_NEAR(white.g, 255, 2);
    EXPECT_NEAR(white.b, 255, 2);
    EXPECT_NEAR(red709.r, 255, 3);
    EXPECT_NEAR(red709.g, 24, 4);
    EXPECT_NEAR(red709.b, 0, 3);
}

TEST_F(Nv12ToRgbaTest, UsesRequestedSupportedYuvMatrices) {
    const std::array<std::pair<AVColorSpace, int>, 3> cases{{
        {AVCOL_SPC_SMPTE170M, 0},
        {AVCOL_SPC_BT709, 24},
        {AVCOL_SPC_BT2020_NCL, 9},
    }};
    for (const auto& [colorSpace, expectedGreen] : cases) {
        auto frame = makeFrame(AV_PIX_FMT_YUV420P, 4, 2);
        frame->colorspace = colorSpace;
        fill420(frame.get(), 81, 90, 240);
        odyssey::Nv12ToRgba converter(device.Get(), 4, 2);
        converter.convert(context.Get(), frame.get(), odyssey::StereoMode::FullSbs);
        const Rgba pixel = readOutput(converter)[0];
        EXPECT_NEAR(pixel.r, 255, 3);
        EXPECT_NEAR(pixel.g, expectedGreen, 4);
        EXPECT_NEAR(pixel.b, 0, 3);
    }
}

TEST_F(Nv12ToRgbaTest, UsesFullRangeAndResolutionBasedUnknownMatrixFallback) {
    auto fullRange = makeFrame(AV_PIX_FMT_YUV420P, 4, 2);
    fullRange->color_range = AVCOL_RANGE_JPEG;
    fill420(fullRange.get(), 0, 128, 128);
    odyssey::Nv12ToRgba fullRangeConverter(device.Get(), 4, 2);
    fullRangeConverter.convert(
        context.Get(), fullRange.get(), odyssey::StereoMode::FullSbs);
    const Rgba black = readOutput(fullRangeConverter)[0];
    EXPECT_NEAR(black.r, 0, 2);

    auto sd = makeFrame(AV_PIX_FMT_YUV420P, 4, 480);
    sd->colorspace = AVCOL_SPC_UNSPECIFIED;
    fill420(sd.get(), 81, 90, 240);
    odyssey::Nv12ToRgba sdConverter(device.Get(), 4, 480);
    sdConverter.convert(context.Get(), sd.get(), odyssey::StereoMode::FullSbs);

    auto hd = makeFrame(AV_PIX_FMT_YUV420P, 4, 720);
    hd->colorspace = AVCOL_SPC_UNSPECIFIED;
    fill420(hd.get(), 81, 90, 240);
    odyssey::Nv12ToRgba hdConverter(device.Get(), 4, 720);
    hdConverter.convert(context.Get(), hd.get(), odyssey::StereoMode::FullSbs);

    EXPECT_NEAR(readOutput(sdConverter)[0].g, 0, 4);
    EXPECT_NEAR(readOutput(hdConverter)[0].g, 24, 4);
}

TEST_F(Nv12ToRgbaTest, KeepsOppositeEyeChromaOutOfBoundaryPixels) {
    auto frame = makeFrame(AV_PIX_FMT_YUV420P, 8, 2);
    fill420(frame.get(), 128, 128, 128);
    auto* u = frame->data[1];
    auto* v = frame->data[2];
    u[0] = 16; u[1] = 16; u[2] = 240; u[3] = 240;
    v[0] = 240; v[1] = 240; v[2] = 16; v[3] = 16;

    odyssey::Nv12ToRgba converter(device.Get(), 8, 2);
    converter.convert(context.Get(), frame.get(), odyssey::StereoMode::FullSbs);
    const auto pixels = readOutput(converter);
    EXPECT_NEAR(pixels[0].r, pixels[3].r, 1);
    EXPECT_NEAR(pixels[0].b, pixels[3].b, 1);
    EXPECT_NEAR(pixels[4].r, pixels[7].r, 1);
    EXPECT_NEAR(pixels[4].b, pixels[7].b, 1);
    EXPECT_GT(std::abs(static_cast<int>(pixels[3].r) - pixels[4].r), 40);
}

TEST_F(Nv12ToRgbaTest, MonoConversionDoesNotClampAtTheMidline) {
    auto frame = makeFrame(AV_PIX_FMT_YUV420P, 6, 2);
    fill420(frame.get(), 128, 128, 128);
    auto* u = frame->data[1];
    u[0] = 64; u[1] = 128; u[2] = 192;

    odyssey::Nv12ToRgba converter(device.Get(), 6, 2);
    converter.convert(context.Get(), frame.get(), odyssey::StereoMode::Mono2D);
    const auto pixels = readOutput(converter);
    EXPECT_LT(pixels[1].b, pixels[2].b);
    EXPECT_EQ(pixels[2].b, pixels[3].b);
    EXPECT_LT(pixels[3].b, pixels[4].b);
}

TEST_F(Nv12ToRgbaTest, ConvertsSoftwareNv12AndTenBit420) {
    for (const AVPixelFormat format : {
             AV_PIX_FMT_NV12,
             AV_PIX_FMT_YUV420P10LE,
             AV_PIX_FMT_P010LE,
         }) {
        const bool tenBit = format == AV_PIX_FMT_YUV420P10LE || format == AV_PIX_FMT_P010LE;
        const Rgba gray = convertConstant(format, tenBit ? 502 : 126, tenBit ? 512 : 128,
                                          tenBit ? 512 : 128);
        EXPECT_NEAR(gray.r, 128, 4);
        EXPECT_NEAR(gray.g, 128, 4);
        EXPECT_NEAR(gray.b, 128, 4);
    }
}

TEST_F(Nv12ToRgbaTest, KeepsTenBitSoftwareEyesDistinctAtTheSeam) {
    for (const AVPixelFormat format : {AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE}) {
        auto frame = makeFrame(format, 8, 2);
        fill420(frame.get(), 324, 360, 960);
        const int shift = format == AV_PIX_FMT_P010LE ? 6 : 0;
        for (int row = 0; row < frame->height; ++row) {
            auto* y = reinterpret_cast<std::uint16_t*>(
                frame->data[0] + row * frame->linesize[0]);
            std::fill_n(y + 4, 4, static_cast<std::uint16_t>(164 << shift));
        }
        if (format == AV_PIX_FMT_P010LE) {
            auto* uv = reinterpret_cast<std::uint16_t*>(frame->data[1]);
            uv[4] = static_cast<std::uint16_t>(960 << 6);
            uv[5] = static_cast<std::uint16_t>(440 << 6);
            uv[6] = static_cast<std::uint16_t>(960 << 6);
            uv[7] = static_cast<std::uint16_t>(440 << 6);
        } else {
            auto* u = reinterpret_cast<std::uint16_t*>(frame->data[1]);
            auto* v = reinterpret_cast<std::uint16_t*>(frame->data[2]);
            u[2] = 960; u[3] = 960;
            v[2] = 440; v[3] = 440;
        }

        odyssey::Nv12ToRgba converter(device.Get(), 8, 2);
        converter.convert(context.Get(), frame.get(), odyssey::StereoMode::FullSbs);
        const auto pixels = readOutput(converter);
        EXPECT_GT(pixels[3].r, 220);
        EXPECT_LT(pixels[3].b, 40);
        EXPECT_GT(pixels[4].b, 220);
        EXPECT_LT(pixels[4].r, 40);
    }
}

TEST_F(Nv12ToRgbaTest, SoftwareP010MatchesPlanarTenBitMatricesAndRanges) {
    for (const AVColorSpace colorSpace : {
             AVCOL_SPC_SMPTE170M,
             AVCOL_SPC_BT709,
         }) {
        for (const AVColorRange range : {AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG}) {
            auto planar = makeFrame(AV_PIX_FMT_YUV420P10LE, 4, 2);
            planar->colorspace = colorSpace;
            planar->color_range = range;
            fill420(planar.get(), 400, 300, 800);
            odyssey::Nv12ToRgba planarConverter(device.Get(), 4, 2);
            planarConverter.convert(
                context.Get(), planar.get(), odyssey::StereoMode::FullSbs);
            const Rgba expected = readOutput(planarConverter)[0];

            auto p010 = makeFrame(AV_PIX_FMT_P010LE, 4, 2);
            p010->colorspace = colorSpace;
            p010->color_range = range;
            fill420(p010.get(), 400, 300, 800);
            odyssey::Nv12ToRgba p010Converter(device.Get(), 4, 2);
            p010Converter.convert(
                context.Get(), p010.get(), odyssey::StereoMode::FullSbs);
            const Rgba actual = readOutput(p010Converter)[0];

            EXPECT_NEAR(actual.r, expected.r, 4);
            EXPECT_NEAR(actual.g, expected.g, 4);
            EXPECT_NEAR(actual.b, expected.b, 4);
        }
    }
}

TEST_F(Nv12ToRgbaTest, HardwareNv12MatchesSoftwareWithoutCrossEyeChroma) {
    auto software = makeFrame(AV_PIX_FMT_NV12, 8, 2);
    fill420(software.get(), 81, 90, 240);
    for (int row = 0; row < software->height; ++row) {
        std::fill_n(software->data[0] + row * software->linesize[0] + 4, 4, 41);
    }
    auto* uv = software->data[1];
    uv[4] = 240; uv[5] = 110; uv[6] = 240; uv[7] = 110;

    odyssey::Nv12ToRgba softwareConverter(device.Get(), 8, 2);
    softwareConverter.convert(
        context.Get(), software.get(), odyssey::StereoMode::FullSbs);
    const auto expected = readOutput(softwareConverter);

    ComPtr<ID3D11Texture2D> texture;
    auto hardware = makeHardwareNv12(software.get(), texture);
    odyssey::Nv12ToRgba hardwareConverter(device.Get(), 8, 2);
    hardwareConverter.convert(
        context.Get(), hardware.get(), odyssey::StereoMode::FullSbs);
    const auto actual = readOutput(hardwareConverter);

    for (std::size_t index = 0; index < actual.size(); ++index) {
        EXPECT_NEAR(actual[index].r, expected[index].r, 4);
        EXPECT_NEAR(actual[index].g, expected[index].g, 4);
        EXPECT_NEAR(actual[index].b, expected[index].b, 4);
    }
    EXPECT_NEAR(actual[0].r, actual[3].r, 1);
    EXPECT_NEAR(actual[4].b, actual[7].b, 1);
    EXPECT_GT(std::abs(static_cast<int>(actual[3].r) - actual[4].r), 100);
}

TEST_F(Nv12ToRgbaTest, HardwareNv12MatchesSoftwareMatricesAndRanges) {
    for (const AVColorSpace colorSpace : {
             AVCOL_SPC_SMPTE170M,
             AVCOL_SPC_BT709,
             AVCOL_SPC_BT2020_NCL,
             AVCOL_SPC_UNSPECIFIED,
         }) {
        for (const AVColorRange range : {AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG}) {
            auto software = makeFrame(AV_PIX_FMT_NV12, 4, 2);
            software->colorspace = colorSpace;
            software->color_range = range;
            fill420(software.get(), range == AVCOL_RANGE_JPEG ? 64 : 81, 90, 200);

            odyssey::Nv12ToRgba softwareConverter(device.Get(), 4, 2);
            softwareConverter.convert(
                context.Get(), software.get(), odyssey::StereoMode::FullSbs);
            const Rgba expected = readOutput(softwareConverter)[0];

            ComPtr<ID3D11Texture2D> texture;
            auto hardware = makeHardwareNv12(software.get(), texture);
            odyssey::Nv12ToRgba hardwareConverter(device.Get(), 4, 2);
            hardwareConverter.convert(
                context.Get(), hardware.get(), odyssey::StereoMode::FullSbs);
            const Rgba actual = readOutput(hardwareConverter)[0];
            EXPECT_NEAR(actual.r, expected.r, 4);
            EXPECT_NEAR(actual.g, expected.g, 4);
            EXPECT_NEAR(actual.b, expected.b, 4);
        }
    }
}

TEST_F(Nv12ToRgbaTest, OutputSrvUsesTheExistingSrgbContract) {
    odyssey::Nv12ToRgba converter(device.Get(), 4, 2);
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    converter.outputSrv()->GetDesc(&desc);
    EXPECT_EQ(desc.Format, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    EXPECT_EQ(desc.ViewDimension, D3D11_SRV_DIMENSION_TEXTURE2D);
}

TEST_F(Nv12ToRgbaTest, RejectsPqAndHlgWithoutToneMapping) {
    for (const AVColorTransferCharacteristic transfer : {
             AVCOL_TRC_SMPTE2084,
             AVCOL_TRC_ARIB_STD_B67,
         }) {
        auto frame = makeFrame(AV_PIX_FMT_YUV420P10LE, 4, 2);
        frame->color_trc = transfer;
        fill420(frame.get(), 64, 512, 512);
        odyssey::Nv12ToRgba converter(device.Get(), 4, 2);
        EXPECT_THROW(
            converter.convert(context.Get(), frame.get(), odyssey::StereoMode::FullSbs),
            std::runtime_error);
    }
}

TEST_F(Nv12ToRgbaTest, RejectsWideGamutPrimariesAndUnsupportedMatrix) {
    auto wideGamut = makeFrame(AV_PIX_FMT_YUV420P10LE, 4, 2);
    wideGamut->color_primaries = AVCOL_PRI_BT2020;
    wideGamut->colorspace = AVCOL_SPC_BT2020_NCL;
    fill420(wideGamut.get(), 502, 512, 512);
    odyssey::Nv12ToRgba converter(device.Get(), 4, 2);
    EXPECT_THROW(
        converter.convert(context.Get(), wideGamut.get(), odyssey::StereoMode::FullSbs),
        std::runtime_error);

    auto unsupportedMatrix = makeFrame(AV_PIX_FMT_YUV420P, 4, 2);
    unsupportedMatrix->colorspace = AVCOL_SPC_FCC;
    fill420(unsupportedMatrix.get(), 126, 128, 128);
    EXPECT_THROW(
        converter.convert(context.Get(), unsupportedMatrix.get(), odyssey::StereoMode::FullSbs),
        std::runtime_error);
}

TEST_F(Nv12ToRgbaTest, RejectsSbsEyeWidthsMisalignedToChromaSamples) {
    auto frame = makeFrame(AV_PIX_FMT_YUV420P, 6, 2);
    fill420(frame.get(), 128, 128, 128);
    odyssey::Nv12ToRgba converter(device.Get(), 6, 2);
    EXPECT_THROW(
        converter.convert(context.Get(), frame.get(), odyssey::StereoMode::FullSbs),
        std::invalid_argument);
}

} // namespace
