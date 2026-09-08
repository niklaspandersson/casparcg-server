/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: CasparCG Team
 */

#include "screen_consumer_vk.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// macOS: Use Grand Central Dispatch to marshal GLFW operations to main thread
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#include <vulkan/vulkan_metal.h>
#endif

#include <common/array.h>
#include <common/diagnostics/graph.h>
#include <common/future.h>
#include <common/log.h>
#include <common/memory.h>
#include <common/param.h>
#include <common/timer.h>
#include <common/utf.h>

#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/frame/geometry.h>
#include <core/video_format.h>

#include "vk_util/device.h"
#include "vk_util/swapchain.h"
#include "vk_util/render_pipeline.h"
#include "vk_util/texture.h"
#include "vk_util/buffer.h"

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

namespace caspar { namespace screen {

namespace vk = caspar::accelerator::vk;

enum class stretch
{
    none,
    uniform,
    fill,
    uniform_to_fill
};

struct configuration
{
    enum class aspect_ratio
    {
        aspect_4_3 = 0,
        aspect_16_9,
        aspect_invalid,
    };

    enum class colour_spaces
    {
        RGB               = 0,
        datavideo_full    = 1,
        datavideo_limited = 2
    };

    std::wstring    name          = L"Screen consumer";
    int             screen_index  = 0;
    int             screen_x      = 0;
    int             screen_y      = 0;
    int             screen_width  = 0;
    int             screen_height = 0;
    screen::stretch stretch       = screen::stretch::fill;
    bool            windowed      = true;
    bool            key_only      = false;
    bool            sbs_key       = false;
    aspect_ratio    aspect        = aspect_ratio::aspect_invalid;
    bool            vsync         = false;
    bool            interactive   = true;
    bool            borderless    = false;
    bool            always_on_top = false;
    colour_spaces   colour_space  = colour_spaces::RGB;
    bool            high_bitdepth = false;
};

struct screen_consumer_vk
{
    const configuration     config_;
    core::video_format_desc format_desc_;
    int                     channel_index_;

    int screen_width_  = format_desc_.width;
    int screen_height_ = format_desc_.height;
    int square_width_  = format_desc_.square_width;
    int square_height_ = format_desc_.square_height;
    int screen_x_      = 0;
    int screen_y_      = 0;

    GLFWwindow* window_ = nullptr;

    // Vulkan objects
    std::shared_ptr<vk::device>          vk_device_;
    std::unique_ptr<vk::swapchain>       swapchain_;
    std::unique_ptr<vk::render_pipeline> render_pipeline_;

    // Double-buffered frame textures
    std::shared_ptr<vk::texture>                   frame_texture_;
    std::shared_ptr<caspar::accelerator::vk::buffer> staging_buffer_;

    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       tick_timer_;

    tbb::concurrent_bounded_queue<core::const_frame> frame_buffer_;

    std::atomic<bool> is_running_{true};
    std::atomic<bool> needs_resize_{false};
    std::atomic<bool> first_frame_presented_{false};
    std::thread       thread_;

    screen_consumer_vk(const screen_consumer_vk&)            = delete;
    screen_consumer_vk& operator=(const screen_consumer_vk&) = delete;

  public:
    screen_consumer_vk(const configuration& config, const core::video_format_desc& format_desc, int channel_index)
        : config_(config)
        , format_desc_(format_desc)
        , channel_index_(channel_index)
    {
        if (format_desc_.format == core::video_format::ntsc &&
            config_.aspect == configuration::aspect_ratio::aspect_4_3) {
            // Use default values which are 4:3.
        } else {
            if (config_.aspect == configuration::aspect_ratio::aspect_16_9) {
                square_width_ = format_desc.height * 16 / 9;
            } else if (config_.aspect == configuration::aspect_ratio::aspect_4_3) {
                square_width_ = format_desc.height * 4 / 3;
            }
        }

        frame_buffer_.set_capacity(1);

        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_text(print());
        diagnostics::register_graph(graph_);

        // Calculate window size
        if (config.windowed) {
            screen_x_ = config.screen_x;
            screen_y_ = config.screen_y;

            if (config.screen_width > 0 && config.screen_height > 0) {
                screen_width_  = config.screen_width;
                screen_height_ = config.screen_height;
            } else if (config.screen_width > 0) {
                screen_width_  = config.screen_width;
                screen_height_ = square_height_ * config.screen_width / square_width_;
            } else if (config.screen_height > 0) {
                screen_height_ = config.screen_height;
                screen_width_  = square_width_ * config.screen_height / square_height_;
            } else {
                screen_width_  = square_width_;
                screen_height_ = square_height_;
            }
        }

        // SBS key requires double width
        if (config_.sbs_key) {
            screen_width_ *= 2;
        }

        thread_ = std::thread([this] {
            try {
                run();
            } catch (tbb::user_abort&) {
                // Do nothing
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
                is_running_ = false;
            }
        });
    }

    ~screen_consumer_vk()
    {
        is_running_ = false;
        frame_buffer_.abort();
        thread_.join();
    }

    void run()
    {
        // macOS: Initialize GLFW on main thread via GCD
        __block bool init_success = false;
        __block GLFWwindow* created_window = nullptr;
        __block int final_width = screen_width_;
        __block int final_height = screen_height_;

        // Capture config values for use in block
        const bool windowed = config_.windowed;
        const int screen_index = config_.screen_index;
        const bool borderless = config_.borderless;
        const bool interactive = config_.interactive;
        const bool always_on_top = config_.always_on_top;
        const int screen_x = screen_x_;
        const int screen_y = screen_y_;
        std::string window_title = u8(print());

        dispatch_sync(dispatch_get_main_queue(), ^{
            // Initialize GLFW
            if (!glfwInit()) {
                CASPAR_LOG(error) << "Failed to initialize GLFW";
                init_success = false;
                return;
            }

            // Tell GLFW not to create OpenGL context
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, windowed ? GLFW_TRUE : GLFW_FALSE);

            if (borderless) {
                glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            }

            // Create window
            GLFWmonitor* monitor = nullptr;
            if (!windowed) {
                monitor = glfwGetPrimaryMonitor();
                if (screen_index > 0) {
                    int monitorCount;
                    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
                    if (screen_index < monitorCount) {
                        monitor = monitors[screen_index];
                    }
                }

                const GLFWvidmode* mode = glfwGetVideoMode(monitor);
                final_width = mode->width;
                final_height = mode->height;
            }

            created_window = glfwCreateWindow(final_width, final_height, window_title.c_str(), monitor, nullptr);
            if (!created_window) {
                glfwTerminate();
                CASPAR_LOG(error) << "Failed to create GLFW window";
                init_success = false;
                return;
            }

            // Position window
            if (windowed) {
                glfwSetWindowPos(created_window, screen_x, screen_y);
            }

            // Hide cursor if non-interactive
            if (!interactive) {
                glfwSetInputMode(created_window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
            }

            // Set always on top
            if (always_on_top) {
                glfwSetWindowAttrib(created_window, GLFW_FLOATING, GLFW_TRUE);
            }

            init_success = true;
        });

        if (!init_success || !created_window) {
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Failed to initialize GLFW window on main thread"));
        }

        window_ = created_window;
        screen_width_ = final_width;
        screen_height_ = final_height;

        // Set resize callback and show window - must be done on main thread
        dispatch_sync(dispatch_get_main_queue(), ^{
            glfwSetWindowUserPointer(window_, this);
            glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* win, int width, int height) {
                auto* self = static_cast<screen_consumer_vk*>(glfwGetWindowUserPointer(win));
                self->needs_resize_ = true;
            });

            // Make window visible and key BEFORE creating Vulkan surface
            // This ensures the layer hierarchy is fully initialized on macOS
            glfwShowWindow(window_);
            NSWindow* nsWindow = glfwGetCocoaWindow(window_);
            [nsWindow makeKeyAndOrderFront:nil];

            // Ensure the window is fully laid out
            [[nsWindow contentView] setNeedsDisplay:YES];
            [[nsWindow contentView] displayIfNeeded];
        });

        // Small delay to ensure window is fully visible
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Initialize Vulkan
        vk_device_ = std::make_shared<vk::device>();
        auto handles = vk_device_->get_handles();

#ifdef __APPLE__
        // macOS: Manually create CAMetalLayer and Vulkan surface
        // This ensures we control exactly which layer MoltenVK renders to
        __block void* created_surface = nullptr;

        dispatch_sync(dispatch_get_main_queue(), ^{
            NSWindow* nsWindow = glfwGetCocoaWindow(window_);
            NSView* contentView = [nsWindow contentView];

            // Ensure view wants to be layer-backed
            [contentView setWantsLayer:YES];

            // Create and configure CAMetalLayer
            CAMetalLayer* metalLayer = [CAMetalLayer layer];
            metalLayer.device = MTLCreateSystemDefaultDevice();
            metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            metalLayer.framebufferOnly = NO;
            metalLayer.contentsScale = [nsWindow backingScaleFactor];

            // Set the layer on the view - this makes the view the layer's delegate
            [contentView setLayer:metalLayer];

            // Set drawable size to match window backing size (Retina-aware)
            NSRect backingBounds = [contentView convertRectToBacking:contentView.bounds];
            metalLayer.drawableSize = CGSizeMake(backingBounds.size.width, backingBounds.size.height);

            CASPAR_LOG(info) << L"[vk::screen] Configured CAMetalLayer:"
                              << L" drawableSize=" << metalLayer.drawableSize.width << L"x" << metalLayer.drawableSize.height
                              << L" contentsScale=" << metalLayer.contentsScale
                              << L" device=" << (metalLayer.device ? L"set" : L"none");

            // Create Vulkan surface using VK_EXT_metal_surface
            VkMetalSurfaceCreateInfoEXT surfaceCreateInfo{};
            surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
            surfaceCreateInfo.pLayer = (__bridge const CAMetalLayer*)metalLayer;

            auto vkCreateMetalSurfaceEXT = (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(
                static_cast<VkInstance>(handles.instance), "vkCreateMetalSurfaceEXT");

            if (vkCreateMetalSurfaceEXT) {
                VkSurfaceKHR surface = VK_NULL_HANDLE;
                VkResult result = vkCreateMetalSurfaceEXT(static_cast<VkInstance>(handles.instance),
                                                          &surfaceCreateInfo, nullptr, &surface);
                if (result == VK_SUCCESS) {
                    created_surface = surface;
                    CASPAR_LOG(info) << L"[vk::screen] Created Vulkan surface via VK_EXT_metal_surface";
                } else {
                    CASPAR_LOG(error) << L"[vk::screen] vkCreateMetalSurfaceEXT failed: " << result;
                }
            } else {
                CASPAR_LOG(warning) << L"[vk::screen] vkCreateMetalSurfaceEXT not available, falling back to GLFW";
            }
        });

        swapchain_ = std::make_unique<vk::swapchain>(handles.instance,
                                                      handles.physical_device,
                                                      handles.device,
                                                      handles.queue,
                                                      handles.queue_family_index,
                                                      window_,
                                                      config_.vsync,
                                                      created_surface);
#else
        swapchain_ = std::make_unique<vk::swapchain>(handles.instance,
                                                      handles.physical_device,
                                                      handles.device,
                                                      handles.queue,
                                                      handles.queue_family_index,
                                                      window_,
                                                      config_.vsync);
#endif

        render_pipeline_ = std::make_unique<vk::render_pipeline>(
            handles.device, handles.physical_device, handles.command_pool, handles.queue, *swapchain_);

        // Create frame texture and staging buffer
        auto size_multiplier = config_.high_bitdepth ? 2 : 1;
        auto depth           = config_.high_bitdepth ? common::bit_depth::bit16 : common::bit_depth::bit8;
        frame_texture_       = vk_device_->create_texture(format_desc_.width, format_desc_.height, 4, depth);
        staging_buffer_      = std::make_shared<vk::buffer>(
            handles.device, handles.physical_device, format_desc_.size * size_multiplier, true);

        // Clear texture to black on initialization to avoid showing uninitialized memory
        frame_texture_->clear();

        if (config_.vsync) {
            CASPAR_LOG(info) << print() << " Enabled vsync.";
        }

        if (config_.colour_space == configuration::colour_spaces::datavideo_full ||
            config_.colour_space == configuration::colour_spaces::datavideo_limited) {
            CASPAR_LOG(info) << print() << " Enabled colours conversion for DataVideo TC-100/TC-200 "
                             << (config_.colour_space == configuration::colour_spaces::datavideo_full ? "(Full Range)."
                                                                                                       : "(Limited Range).");
        }

        // Main render loop
        while (is_running_) {
            tick();
        }

        // Cleanup Vulkan first (while window still exists)
        vk_device_->dispatch_sync([this] {
            render_pipeline_.reset();
            swapchain_.reset();
            frame_texture_.reset();
            staging_buffer_.reset();
        });
        vk_device_.reset();

        // Cleanup GLFW on main thread
        // Use dispatch_async with semaphore to avoid deadlock during shutdown
        // (if main thread is blocked, dispatch_sync would hang forever)
        dispatch_semaphore_t cleanup_done = dispatch_semaphore_create(0);
        GLFWwindow* win_to_destroy = window_;
        window_ = nullptr;

        dispatch_async(dispatch_get_main_queue(), ^{
            if (win_to_destroy) {
                glfwDestroyWindow(win_to_destroy);
            }
            glfwTerminate();
            dispatch_semaphore_signal(cleanup_done);
        });

        // Wait up to 1 second for cleanup, then proceed anyway to avoid hang
        dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC);
        if (dispatch_semaphore_wait(cleanup_done, timeout) != 0) {
            CASPAR_LOG(warning) << print() << L" GLFW cleanup timed out - main thread may be blocked";
        }
    }

    bool poll()
    {
        // macOS: Poll events on main thread via GCD
        // Use dispatch_async with timeout to avoid deadlock during shutdown
        if (!is_running_) {
            return true;
        }

        __block bool should_close = false;
        GLFWwindow* win = window_;
        dispatch_semaphore_t poll_done = dispatch_semaphore_create(0);

        dispatch_async(dispatch_get_main_queue(), ^{
            glfwPollEvents();
            if (glfwWindowShouldClose(win)) {
                should_close = true;
            }
            dispatch_semaphore_signal(poll_done);
        });

        // Wait up to 100ms for poll to complete (short timeout since this is called frequently)
        dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC);
        if (dispatch_semaphore_wait(poll_done, timeout) != 0) {
            // Timeout - main thread may be blocked, exit gracefully
            CASPAR_LOG(warning) << L"[vk::screen] poll() timed out waiting for main thread";
            is_running_ = false;
            return true;
        }

        if (should_close) {
            CASPAR_LOG(info) << L"[vk::screen] Window close requested, stopping consumer";
            is_running_ = false;
            return true;
        }

        return false;
    }

    void handle_resize()
    {
        if (!needs_resize_ || !is_running_) {
            return;
        }
        needs_resize_ = false;

        // Get framebuffer size on main thread with timeout to avoid shutdown deadlock
        __block int width = 0;
        __block int height = 0;
        GLFWwindow* win = window_;
        dispatch_semaphore_t resize_done = dispatch_semaphore_create(0);

        dispatch_async(dispatch_get_main_queue(), ^{
            glfwGetFramebufferSize(win, &width, &height);
            dispatch_semaphore_signal(resize_done);
        });

        dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC);
        if (dispatch_semaphore_wait(resize_done, timeout) != 0) {
            // Timeout - main thread may be blocked
            return;
        }

        if (width == 0 || height == 0) {
            return;
        }

        screen_width_  = width;
        screen_height_ = height;

        swapchain_->recreate();
        render_pipeline_->recreate_framebuffers();

        CASPAR_LOG(info) << print() << L" Resized to " << width << L"x" << height;
    }

    void tick()
    {
        core::const_frame in_frame;

        while (!frame_buffer_.try_pop(in_frame) && is_running_) {
            poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (!in_frame) {
            return;
        }

        poll();
        handle_resize();

        // Wait for previous frame to complete
        swapchain_->wait_for_fence();

        // Acquire next swapchain image
        uint32_t imageIndex = swapchain_->acquire_next_image();
        if (imageIndex == std::numeric_limits<uint32_t>::max()) {
            // Swapchain out of date, recreate
            needs_resize_ = true;
            handle_resize();
            return;
        }

        swapchain_->reset_fence();

        // Upload frame data to texture
        auto size_multiplier = config_.high_bitdepth ? 2 : 1;
        auto frame_data = in_frame.image_data(0);
        auto expected_size = static_cast<size_t>(format_desc_.size * size_multiplier);

        // Validate frame data size
        if (frame_data.size() < expected_size) {
            CASPAR_LOG(warning) << print() << L" Frame data size mismatch: " << frame_data.size() << L" < " << expected_size;
            return;
        }

        std::memcpy(staging_buffer_->data(), frame_data.begin(), expected_size);


        frame_texture_->copy_from(*staging_buffer_);

        // Calculate aspect ratio and positioning
        auto params = calculate_render_params();

        // Render
        render_pipeline_->render(*frame_texture_,
                                 imageIndex,
                                 params,
                                 swapchain_->image_available_semaphore(),
                                 swapchain_->render_finished_semaphore(),
                                 swapchain_->in_flight_fence());

        // Present
        if (!swapchain_->present(imageIndex)) {
            // Swapchain out of date
            needs_resize_ = true;
        }

        swapchain_->next_frame();

#ifdef __APPLE__
        // macOS workaround: First frame may not display until window is moved/resized
        // (GLFW bug on macOS 10.14+). Trigger a window position change to force content display.
        if (!first_frame_presented_.exchange(true)) {
            GLFWwindow* win = window_;
            dispatch_async(dispatch_get_main_queue(), ^{
                int x, y;
                glfwGetWindowPos(win, &x, &y);
                // Move window by 1 pixel and back to trigger redraw
                glfwSetWindowPos(win, x + 1, y);
                glfwSetWindowPos(win, x, y);
                CASPAR_LOG(info) << L"[vk::screen] Triggered window position change to force initial content display";
            });
        }
#endif

        graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
        tick_timer_.restart();
    }

    vk::screen_push_constants calculate_render_params()
    {
        vk::screen_push_constants params{};

        // Calculate target ratio based on stretch mode
        float target_width  = 1.0f;
        float target_height = 1.0f;

        if (config_.stretch == screen::stretch::none) {
            target_width = static_cast<float>(config_.sbs_key ? square_width_ * 2 : square_width_) /
                           static_cast<float>(screen_width_);
            target_height = static_cast<float>(square_height_) / static_cast<float>(screen_height_);
        } else if (config_.stretch == screen::stretch::uniform) {
            float aspect = static_cast<float>(config_.sbs_key ? square_width_ * 2 : square_width_) /
                           static_cast<float>(square_height_);
            target_width =
                std::min(1.0f, static_cast<float>(screen_height_) * aspect / static_cast<float>(screen_width_));
            target_height =
                static_cast<float>(screen_width_ * target_width) / static_cast<float>(screen_height_ * aspect);
        } else if (config_.stretch == screen::stretch::uniform_to_fill) {
            float wr = static_cast<float>(config_.sbs_key ? square_width_ * 2 : square_width_) /
                       static_cast<float>(screen_width_);
            float hr    = static_cast<float>(square_height_) / static_cast<float>(screen_height_);
            float r_inv = 1.0f / std::min(wr, hr);
            target_width  = wr * r_inv;
            target_height = hr * r_inv;
        }
        // For fill, target_width and target_height remain 1.0

        params.pos_scale[0]  = target_width;
        params.pos_scale[1]  = target_height;
        params.pos_offset[0] = 0.0f;
        params.pos_offset[1] = 0.0f;
        params.tex_scale[0]  = 1.0f;
        params.tex_scale[1]  = 1.0f;
        params.tex_offset[0] = 0.0f;
        params.tex_offset[1] = 0.0f;
        params.key_only      = config_.key_only ? 1 : 0;
        params.colour_space  = static_cast<int32_t>(config_.colour_space);
        params.window_width  = screen_width_;

        return params;
    }

    std::future<bool> send(core::video_field field, const core::const_frame& frame)
    {
        if (!frame_buffer_.try_push(frame)) {
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
        }
        return make_ready_future(is_running_.load());
    }

    std::wstring channel_and_format() const
    {
        return L"[" + std::to_wstring(channel_index_) + L"|" + format_desc_.name + L"]";
    }

    std::wstring print() const { return config_.name + L" " + channel_and_format(); }
};

struct screen_consumer_proxy_vk : public core::frame_consumer
{
    const configuration                 config_;
    std::unique_ptr<screen_consumer_vk> consumer_;

  public:
    explicit screen_consumer_proxy_vk(configuration config)
        : config_(std::move(config))
    {
    }

    // frame_consumer

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        consumer_.reset();
        consumer_ = std::make_unique<screen_consumer_vk>(config_, format_desc, channel_info.index);
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        return consumer_->send(field, frame);
    }

    std::wstring print() const override { return consumer_ ? consumer_->print() : L"[screen_consumer_vk]"; }

    std::wstring name() const override { return L"screen"; }

    bool has_synchronization_clock() const override { return false; }

    int index() const override { return 600 + (config_.key_only ? 10 : 0) + config_.screen_index; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["screen/name"]          = config_.name;
        state["screen/index"]         = config_.screen_index;
        state["screen/key_only"]      = config_.key_only;
        state["screen/always_on_top"] = config_.always_on_top;
        return state;
    }
};

spl::shared_ptr<core::frame_consumer> create_consumer_vk(const std::vector<std::wstring>&     params,
                                                          const core::video_format_repository& format_repository,
                                                          const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                                          const core::channel_info& channel_info)
{
    if (params.empty() || !boost::iequals(params.at(0), L"SCREEN")) {
        return core::frame_consumer::empty();
    }

    configuration config;

    config.high_bitdepth = (channel_info.depth != common::bit_depth::bit8);

    if (params.size() > 1) {
        try {
            config.screen_index = std::stoi(params.at(1));
        } catch (...) {
        }
    }

    config.windowed    = !contains_param(L"FULLSCREEN", params);
    config.key_only    = contains_param(L"KEY_ONLY", params);
    config.sbs_key     = contains_param(L"SBS_KEY", params);
    config.interactive = !contains_param(L"NON_INTERACTIVE", params);
    config.borderless  = contains_param(L"BORDERLESS", params);

    if (contains_param(L"NAME", params)) {
        config.name = get_param(L"NAME", params);
    }

    if (contains_param(L"X", params)) {
        config.screen_x = get_param(L"X", params, 0);
    }
    if (contains_param(L"Y", params)) {
        config.screen_y = get_param(L"Y", params, 0);
    }
    if (contains_param(L"WIDTH", params)) {
        config.screen_width = get_param(L"WIDTH", params, 0);
    }
    if (contains_param(L"HEIGHT", params)) {
        config.screen_height = get_param(L"HEIGHT", params, 0);
    }

    if (config.sbs_key && config.key_only) {
        CASPAR_LOG(warning) << L" Key-only not supported with configuration of side-by-side fill and key. Ignored.";
        config.key_only = false;
    }

    return spl::make_shared<screen_consumer_proxy_vk>(config);
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer_vk(const boost::property_tree::wptree&                      ptree,
                                  const core::video_format_repository&                     format_repository,
                                  const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                  const core::channel_info&                                channel_info)
{
    configuration config;

    config.high_bitdepth = (channel_info.depth != common::bit_depth::bit8);

    config.name          = ptree.get(L"name", config.name);
    config.screen_index  = ptree.get(L"device", config.screen_index + 1) - 1;
    config.screen_x      = ptree.get(L"x", config.screen_x);
    config.screen_y      = ptree.get(L"y", config.screen_y);
    config.screen_width  = ptree.get(L"width", config.screen_width);
    config.screen_height = ptree.get(L"height", config.screen_height);
    config.windowed      = ptree.get(L"windowed", config.windowed);
    config.key_only      = ptree.get(L"key-only", config.key_only);
    config.sbs_key       = ptree.get(L"sbs-key", config.sbs_key);
    config.vsync         = ptree.get(L"vsync", config.vsync);
    config.interactive   = ptree.get(L"interactive", config.interactive);
    config.borderless    = ptree.get(L"borderless", config.borderless);
    config.always_on_top = ptree.get(L"always-on-top", config.always_on_top);

    auto colour_space_value = ptree.get(L"colour-space", L"RGB");
    config.colour_space     = configuration::colour_spaces::RGB;
    if (colour_space_value == L"datavideo-full")
        config.colour_space = configuration::colour_spaces::datavideo_full;
    else if (colour_space_value == L"datavideo-limited")
        config.colour_space = configuration::colour_spaces::datavideo_limited;

    if (config.sbs_key && config.key_only) {
        CASPAR_LOG(warning) << L" Key-only not supported with configuration of side-by-side fill and key. Ignored.";
        config.key_only = false;
    }

    if ((config.colour_space == configuration::colour_spaces::datavideo_full ||
         config.colour_space == configuration::colour_spaces::datavideo_limited) &&
        config.sbs_key) {
        CASPAR_LOG(warning) << L" Side-by-side fill and key not supported for DataVideo TC100/TC200. Ignored.";
        config.sbs_key = false;
    }

    if ((config.colour_space == configuration::colour_spaces::datavideo_full ||
         config.colour_space == configuration::colour_spaces::datavideo_limited) &&
        config.key_only) {
        CASPAR_LOG(warning) << L" Key only not supported for DataVideo TC100/TC200. Ignored.";
        config.key_only = false;
    }

    auto stretch_str = ptree.get(L"stretch", L"fill");
    if (stretch_str == L"none") {
        config.stretch = screen::stretch::none;
    } else if (stretch_str == L"uniform") {
        config.stretch = screen::stretch::uniform;
    } else if (stretch_str == L"uniform_to_fill") {
        config.stretch = screen::stretch::uniform_to_fill;
    }

    auto aspect_str = ptree.get(L"aspect-ratio", L"default");
    if (aspect_str == L"16:9") {
        config.aspect = configuration::aspect_ratio::aspect_16_9;
    } else if (aspect_str == L"4:3") {
        config.aspect = configuration::aspect_ratio::aspect_4_3;
    }

    return spl::make_shared<screen_consumer_proxy_vk>(config);
}

}} // namespace caspar::screen
