#pragma once

#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>

#include <atomic>
#include <memory>

namespace caspar { namespace test {

// A frame_factory that hands out frames in plain host memory, so producers run without an
// accelerator. It counts the frames it has created and how many of them are still alive (any plane
// still referenced), which lets a test check that nothing holds on to frames after teardown.
class fake_frame_factory final : public core::frame_factory
{
  public:
    fake_frame_factory();

    core::mutable_frame create_frame(const void* tag, const core::pixel_format_desc& desc) override;
    core::mutable_frame
    create_frame(const void* tag, const core::pixel_format_desc& desc, common::bit_depth depth) override;

#ifdef WIN32
    core::const_frame import_d3d_texture(const void*                                             tag,
                                         const std::shared_ptr<accelerator::d3d::d3d_texture2d>& d3d_texture,
                                         core::pixel_format                                      format,
                                         common::bit_depth                                       depth) override;
#endif

    int frames_created() const;
    int frames_alive() const;

  private:
    struct counters
    {
        std::atomic<int> created{0};
        std::atomic<int> alive{0};
    };

    // Shared with the frames, which may outlive the factory.
    std::shared_ptr<counters> counters_;
};

}} // namespace caspar::test
