#include "fake_frame_factory.h"

#include <common/except.h>

#include <core/frame/pixel_format.h>

#include <vector>

namespace caspar { namespace test {

fake_frame_factory::fake_frame_factory()
    : counters_(std::make_shared<counters>())
{
}

core::mutable_frame fake_frame_factory::create_frame(const void* tag, const core::pixel_format_desc& desc)
{
    return create_frame(tag, desc, common::bit_depth::bit8);
}

core::mutable_frame
fake_frame_factory::create_frame(const void* tag, const core::pixel_format_desc& desc, common::bit_depth depth)
{
    ++counters_->created;
    ++counters_->alive;

    // Every plane holds the token; the frame counts as alive until the last plane is released.
    auto token = std::shared_ptr<void>(nullptr, [shared = counters_](void*) { --shared->alive; });

    // Sized like the accelerators' create_frame, so producers see the same buffers.
    std::vector<array<std::uint8_t>> image_data;
    for (auto& plane : desc.planes) {
        auto size  = static_cast<std::size_t>(plane.size) * (depth == common::bit_depth::bit8 ? 1 : 2);
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(size);
        auto data  = bytes->data();
        image_data.emplace_back(data, size, std::make_pair(std::move(bytes), token));
    }

    return core::mutable_frame(tag, std::move(image_data), array<std::int32_t>{}, desc);
}

#ifdef WIN32
core::const_frame fake_frame_factory::import_d3d_texture(const void*,
                                                         const std::shared_ptr<accelerator::d3d::d3d_texture2d>&,
                                                         core::pixel_format,
                                                         common::bit_depth)
{
    CASPAR_THROW_EXCEPTION(not_supported() << msg_info("fake_frame_factory has no d3d textures"));
}
#endif

int fake_frame_factory::frames_created() const { return counters_->created; }
int fake_frame_factory::frames_alive() const { return counters_->alive; }

}} // namespace caspar::test
