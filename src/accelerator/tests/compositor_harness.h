#pragma once

#include <image.h>

#include <common/bit_depth.h>

#include <core/frame/draw_frame.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace caspar { namespace test {

enum class backend
{
    opengl,
    vulkan,
};

std::string to_string(backend b);

// The backend can't be created here: no device, or a build without ENABLE_VULKAN.
struct compositor_unavailable : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct ycbcr_options
{
    int               chroma_width_divisor  = 2;                       // 1: 4:4:4, 2: 4:2:2 or 4:2:0
    int               chroma_height_divisor = 1;                       // 2: 4:2:0
    common::bit_depth depth                 = common::bit_depth::bit8; // or bit10
    core::color_space color_space           = core::color_space::bt709;
};

// Mixes frames on one accelerator backend the way a channel's mixer does, and reads the result
// back, so tests can compare backends with each other and with known answers.
//
// Build each layer with the harness's frame functions (frames are uploaded by the backend that will
// mix them), set its transform, and render() the layers, bottom first. Rendering is synchronous and
// gives back the channel's BGRA output as an image.
class compositor_harness
{
  public:
    // Throws compositor_unavailable when the backend can't be created.
    explicit compositor_harness(backend             b,
                                const std::wstring& format = L"1080p5000",
                                common::bit_depth   depth  = common::bit_depth::bit8);
    ~compositor_harness();

    compositor_harness(const compositor_harness&)            = delete;
    compositor_harness& operator=(const compositor_harness&) = delete;

    backend                        which() const { return backend_; }
    const core::video_format_desc& format() const { return format_; }

    // A frame showing `picture` (any size; the mixer scales it to the channel), as BGRA.
    core::draw_frame bgra_frame(const image& picture);

    // The same picture converted to limited-range Y'CbCr, as a producer decoding video would send it.
    core::draw_frame ycbcr_frame(const image& picture, const ycbcr_options& options = {});

    // Mixes `layers`, bottom first, and returns the channel's output.
    image render(const std::vector<core::draw_frame>& layers);

  private:
    struct impl;
    backend                 backend_;
    core::video_format_desc format_;
    std::unique_ptr<impl>   impl_;
};

}} // namespace caspar::test
