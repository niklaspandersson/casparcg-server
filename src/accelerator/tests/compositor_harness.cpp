#include "compositor_harness.h"

#include <accelerator/accelerator.h>

#include <common/diagnostics/graph.h>

#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>
#include <core/mixer/image/image_mixer.h>
#include <core/mixer/mixer.h>

#include <algorithm>
#include <cmath>

namespace caspar { namespace test {

std::string to_string(backend b) { return b == backend::opengl ? "opengl" : "vulkan"; }

struct compositor_harness::impl
{
    core::video_format_repository      formats;
    accelerator::accelerator           accelerator{formats};
    spl::shared_ptr<core::image_mixer> image_mixer;
    std::unique_ptr<core::mixer>       mixer;

    impl(backend b, common::bit_depth depth)
        : image_mixer([&] {
            try {
#ifdef ENABLE_VULKAN
                accelerator.set_backend(b == backend::opengl ? accelerator::accelerator_backend::opengl
                                                             : accelerator::accelerator_backend::vulkan);
#else
                if (b == backend::vulkan)
                    throw compositor_unavailable("built without ENABLE_VULKAN");
                accelerator.set_backend(accelerator::accelerator_backend::opengl);
#endif
                return spl::make_shared_ptr(
                    std::shared_ptr<core::image_mixer>(accelerator.create_image_mixer(1, depth)));
            } catch (compositor_unavailable&) {
                throw;
            } catch (std::exception& e) {
                throw compositor_unavailable(to_string(b) + ": " + e.what());
            }
        }())
        , mixer(std::make_unique<core::mixer>(1, spl::make_shared<diagnostics::graph>(), image_mixer))
    {
    }
};

compositor_harness::compositor_harness(backend b, const std::wstring& format, common::bit_depth depth)
    : backend_(b)
{
    core::video_format_repository formats;
    format_ = formats.find(format);
    if (format_.format == core::video_format::invalid)
        throw std::invalid_argument("compositor_harness: unknown format");
    impl_ = std::make_unique<impl>(b, depth);
}

compositor_harness::~compositor_harness() = default;

core::draw_frame compositor_harness::bgra_frame(const image& picture)
{
    core::pixel_format_desc desc(core::pixel_format::bgra);
    desc.planes.emplace_back(picture.width, picture.height, 4);

    auto frame  = impl_->image_mixer->create_frame(this, desc);
    auto pixels = frame.image_data(0).data();
    for (int y = 0; y < picture.height; ++y) {
        for (int x = 0; x < picture.width; ++x) {
            auto in  = picture.at(x, y);
            auto out = pixels + (static_cast<std::size_t>(y) * picture.width + x) * 4;
            auto to8 = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
            out[0]   = to8(in[2]);
            out[1]   = to8(in[1]);
            out[2]   = to8(in[0]);
            out[3]   = to8(in[3]);
        }
    }
    return core::draw_frame(core::const_frame(std::move(frame)));
}

core::draw_frame compositor_harness::ycbcr_frame(const image& picture, const ycbcr_options& options)
{
    const bool   ten_bit = options.depth != common::bit_depth::bit8;
    const double kr      = options.color_space == core::color_space::bt601 ? 0.299 : 0.2126;
    const double kb      = options.color_space == core::color_space::bt601 ? 0.114 : 0.0722;
    const double scale   = ten_bit ? 4 : 1; // 10-bit codes are 8-bit codes times 4

    const int cw = picture.width / options.chroma_width_divisor;
    const int ch = picture.height / options.chroma_height_divisor;

    core::pixel_format_desc desc(core::pixel_format::ycbcr, options.color_space);
    desc.planes.emplace_back(picture.width, picture.height, 1, options.depth);
    desc.planes.emplace_back(cw, ch, 1, options.depth);
    desc.planes.emplace_back(cw, ch, 1, options.depth);

    auto frame = impl_->image_mixer->create_frame(this, desc, options.depth);

    auto store = [&](int plane, int x, int y, int width, double code) {
        auto value = static_cast<int>(std::lround(code * scale));
        auto data  = frame.image_data(plane).data();
        auto n     = static_cast<std::size_t>(y) * width + x;
        if (ten_bit)
            reinterpret_cast<std::uint16_t*>(data)[n] = static_cast<std::uint16_t>(value);
        else
            data[n] = static_cast<std::uint8_t>(value);
    };
    auto luma = [&](const float* p) { return kr * p[0] + (1 - kr - kb) * p[1] + kb * p[2]; };

    for (int y = 0; y < picture.height; ++y) {
        for (int x = 0; x < picture.width; ++x)
            store(0, x, y, picture.width, 16 + 219 * luma(picture.at(x, y)));
    }
    // Chroma from the average of the pixels each sample covers.
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            double cb = 0, cr = 0;
            int    count = 0;
            for (int dy = 0; dy < options.chroma_height_divisor; ++dy) {
                for (int dx = 0; dx < options.chroma_width_divisor; ++dx) {
                    auto p = picture.at(x * options.chroma_width_divisor + dx, y * options.chroma_height_divisor + dy);
                    auto l = luma(p);
                    cb += (p[2] - l) / (2 * (1 - kb));
                    cr += (p[0] - l) / (2 * (1 - kr));
                    ++count;
                }
            }
            store(1, x, y, cw, 128 + 224 * cb / count);
            store(2, x, y, cw, 128 + 224 * cr / count);
        }
    }
    return core::draw_frame(core::const_frame(std::move(frame)));
}

image compositor_harness::render(const std::vector<core::draw_frame>& layers)
{
    // The mixer hands back each frame a field or two later; a static scene can just be repeated.
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto frame = (*impl_->mixer)(layers, format_, format_.audio_cadence[0], true);
        if (frame)
            return image::from_frame(frame);
    }
    throw std::runtime_error("compositor_harness::render: the mixer returned no frame");
}

}} // namespace caspar::test
