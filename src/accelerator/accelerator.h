#pragma once

#include <common/bit_depth.h>

#include <core/frame/pixel_format.h>
#include <core/mixer/mixer.h>
#include <core/video_format.h>

#include <boost/property_tree/ptree_fwd.hpp>

#include <future>
#include <memory>
#include <string>
#include <vector>

namespace caspar { namespace accelerator {

class accelerator_device
{
  public:
    virtual boost::property_tree::wptree info() const = 0;
    virtual std::future<void>            gc()         = 0;
};

enum class accelerator_backend
{
    invalid = 0,
    opengl,
#ifdef ENABLE_VULKAN
    vulkan,
#endif
};

#ifdef ENABLE_VULKAN
// Describes Vulkan extensions and features a module/producer wishes to enable.
// Call accelerator::add_vulkan_requirements() before the device is first used
// (i.e. before any call to create_image_mixer or get_device).  The device is
// created lazily, so requirements registered during module pre-init are always
// applied in time.
struct vulkan_device_requirements
{
    // Extensions that should be enabled when present on the physical device.
    // Missing extensions are silently skipped; the module is responsible for
    // checking availability via gpu_accelerator::has_extension() at runtime.
    std::vector<std::string> optional_extensions;
};
#endif // ENABLE_VULKAN

class accelerator
{
  public:
    explicit accelerator(const core::video_format_repository format_repository);
    accelerator(accelerator&) = delete;
    ~accelerator();

    accelerator& operator=(accelerator&) = delete;

    void set_backend(accelerator_backend backend);

#ifdef ENABLE_VULKAN
    // Register Vulkan extension/feature requirements before the device is created.
    // Must be called before the first create_image_mixer() or get_device() call.
    void add_vulkan_requirements(vulkan_device_requirements reqs);
#endif

    std::unique_ptr<caspar::core::image_mixer> create_image_mixer(int channel_id, common::bit_depth depth);

    std::shared_ptr<accelerator_device> get_device() const;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}} // namespace caspar::accelerator
