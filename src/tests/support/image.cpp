#include "image.h"

#include <core/frame/pixel_format.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace caspar { namespace test {

image::image(int width, int height)
    : width(width)
    , height(height)
    , rgba(static_cast<std::size_t>(width) * height * 4)
{
}

image image::from_frame(const core::const_frame& frame)
{
    auto& desc = frame.pixel_format_desc();
    if (desc.format != core::pixel_format::bgra || desc.planes.empty())
        throw std::invalid_argument("image::from_frame: expected a BGRA mixer frame");

    auto& plane = desc.planes[0];
    image result(plane.width, plane.height);
    auto  data   = frame.image_data(0).data();
    bool  deep   = plane.depth != common::bit_depth::bit8;
    float scale  = deep ? 65535.0f : 255.0f;
    auto  sample = [&](std::size_t n) -> float {
        return deep ? (data[n * 2] | data[n * 2 + 1] << 8) / scale : data[n] / scale;
    };

    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            auto n   = (static_cast<std::size_t>(y) * plane.width + x) * 4;
            auto out = result.at(x, y);
            out[0]   = sample(n + 2); // B G R A -> R G B A
            out[1]   = sample(n + 1);
            out[2]   = sample(n + 0);
            out[3]   = sample(n + 3);
        }
    }
    return result;
}

void image::write_png(const std::filesystem::path& path, bool sixteen_bit) const
{
    auto codec     = avcodec_find_encoder(AV_CODEC_ID_PNG);
    auto ctx       = std::shared_ptr<AVCodecContext>(avcodec_alloc_context3(codec),
                                               [](AVCodecContext* c) { avcodec_free_context(&c); });
    ctx->width     = width;
    ctx->height    = height;
    ctx->pix_fmt   = sixteen_bit ? AV_PIX_FMT_RGBA64BE : AV_PIX_FMT_RGBA;
    ctx->time_base = {1, 25};
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
        throw std::runtime_error("image::write_png: can't open the PNG encoder");

    auto frame    = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
    frame->width  = width;
    frame->height = height;
    frame->format = ctx->pix_fmt;
    av_frame_get_buffer(frame.get(), 0);

    for (int y = 0; y < height; ++y) {
        auto row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
        for (int x = 0; x < width * 4; ++x) {
            float v = std::clamp(rgba[static_cast<std::size_t>(y) * width * 4 + x], 0.0f, 1.0f);
            if (sixteen_bit) {
                auto s         = static_cast<std::uint16_t>(std::lround(v * 65535));
                row[x * 2]     = static_cast<std::uint8_t>(s >> 8);
                row[x * 2 + 1] = static_cast<std::uint8_t>(s & 0xFF);
            } else {
                row[x] = static_cast<std::uint8_t>(std::lround(v * 255));
            }
        }
    }

    auto packet = std::shared_ptr<AVPacket>(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    if (avcodec_send_frame(ctx.get(), frame.get()) < 0 || avcodec_receive_packet(ctx.get(), packet.get()) < 0)
        throw std::runtime_error("image::write_png: encoding failed");

    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(packet->data), packet->size);
}

std::string image_difference::describe() const
{
    std::ostringstream text;
    text << "max " << max * 255 << "/255, mean " << mean * 255 << "/255, PSNR " << psnr << " dB, " << pixels_over
         << " pixels over the tolerance, worst at (" << worst_x << ", " << worst_y << ")";
    return text.str();
}

image_difference compare(const image& a, const image& b, double tolerance)
{
    if (a.width != b.width || a.height != b.height)
        throw std::invalid_argument("compare: the images differ in size");

    image_difference result;
    double           sum = 0, squares = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            double pixel_max = 0;
            for (int c = 0; c < 4; ++c) {
                double d = std::abs(static_cast<double>(a.at(x, y)[c]) - b.at(x, y)[c]);
                sum += d;
                squares += d * d;
                pixel_max = std::max(pixel_max, d);
            }
            if (pixel_max > result.max) {
                result.max     = pixel_max;
                result.worst_x = x;
                result.worst_y = y;
            }
            if (pixel_max > tolerance + 1e-6)
                ++result.pixels_over;
        }
    }
    double count = static_cast<double>(a.rgba.size());
    result.mean  = sum / count;
    double mse   = squares / count;
    result.psnr  = mse == 0 ? std::numeric_limits<double>::infinity() : 10 * std::log10(1.0 / mse);
    return result;
}

image difference_image(const image& a, const image& b, float gain)
{
    image result(a.width, a.height);
    for (std::size_t n = 0; n < result.rgba.size(); ++n)
        result.rgba[n] = n % 4 == 3 ? 1.0f : std::min(1.0f, std::abs(a.rgba[n] - b.rgba[n]) * gain);
    return result;
}

}} // namespace caspar::test
