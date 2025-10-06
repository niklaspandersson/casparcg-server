#include "accelerator.h"

#if !defined(APPLE)
#include "ogl/image/image_mixer.h"
#include "ogl/util/device.h"
#endif

#include "vulkan/image/image_mixer.h"
#include "vulkan/util/device.h"

#include <boost/property_tree/ptree.hpp>

#include <common/bit_depth.h>
#include <common/except.h>

#include <core/mixer/image/image_mixer.h>

#include <memory>
#include <mutex>
#include <utility>

namespace caspar { namespace accelerator {

struct accelerator::impl
{
#if !defined(APPLE)
    std::shared_ptr<ogl::device>        ogl_device_;
#endif

    std::shared_ptr<vulkan::device>     vulkan_device_;
    const core::video_format_repository format_repository_;
    accelerator_backend                 backend_;

    impl(const core::video_format_repository format_repository)
        : format_repository_(format_repository)
        , backend_(accelerator_backend::invalid)
    {
    }

    void set_backend(accelerator_backend backend) {
        if (backend_ != accelerator_backend::invalid) {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Accelerator backend already set"));
        }

#if defined(APPLE)
        if (backend != accelerator_backend::vulkan) {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Vulkan is the only supported accelerator backend on apple hardware"));
        }
#endif

        backend_ = backend;
    }


    std::unique_ptr<core::image_mixer> create_image_mixer(int channel_id, common::bit_depth depth)
    {
        // This just makes sure the device is created
        get_device();

#if !defined(APPLE)
        if (backend_ == accelerator_backend::opengl) {
            return std::make_unique<ogl::image_mixer>(
                spl::make_shared_ptr(ogl_device_), channel_id, format_repository_.get_max_video_format_size(), depth);
        }
#endif

        return std::make_unique<vulkan::image_mixer>(
            spl::make_shared_ptr(vulkan_device_), channel_id, format_repository_.get_max_video_format_size(), depth);
    }

    std::shared_ptr<accelerator_device> get_device()
    {
        if (backend_ == accelerator_backend::invalid) {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Accelerator backend not set"));
        }

#if !defined(APPLE)
        if (backend_ == accelerator_backend::opengl) {
            if (!ogl_device_) {
                ogl_device_ = std::make_shared<ogl::device>();
            }

            return ogl_device_;
        }
#endif

        if (!vulkan_device_) {
            vulkan_device_ = std::make_shared<vulkan::device>();
        }

        return vulkan_device_;
    }
};

accelerator::accelerator(const core::video_format_repository format_repository)
    : impl_(std::make_unique<impl>(format_repository))
{
}

accelerator::~accelerator() {}

void accelerator::set_backend(accelerator_backend backend) { impl_->set_backend(backend); }

std::unique_ptr<core::image_mixer> accelerator::create_image_mixer(const int channel_id, common::bit_depth depth)
{
    return impl_->create_image_mixer(channel_id, depth);
}

std::shared_ptr<accelerator_device> accelerator::get_device() const
{
    return impl_->get_device();
}

}} // namespace caspar::accelerator
