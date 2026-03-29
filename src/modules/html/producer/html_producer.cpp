/*
 * Copyright 2013 Sveriges Television AB http://casparcg.com/
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
 * Author: Robert Nagy, ronag89@gmail.com
 */

#include "html_producer.h"

#include <core/video_format.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>
#include <core/frame/frame_transform.h>
#include <core/frame/geometry.h>
#include <core/frame/pixel_format.h>
#include <core/monitor/monitor.h>
#include <core/producer/frame_producer.h>

#include <common/assert.h>
#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/future.h>
#include <common/os/filesystem.h>
#include <common/timer.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/regex.hpp>

#include <tbb/concurrent_queue.h>
#include <tbb/parallel_for.h>

#include <mutex>

#pragma warning(push)
#pragma warning(disable : 4458)
#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_devtools_message_observer.h>
#include <include/cef_registration.h>
#include <include/cef_render_handler.h>
#include <include/cef_values.h>
#pragma warning(pop)

#include <optional>
#include <queue>
#include <utility>

#include "../util.h"

namespace caspar { namespace html {

class html_client
    : public CefClient
    , public CefRenderHandler
    , public CefLifeSpanHandler
    , public CefLoadHandler
    , public CefDisplayHandler
    , public CefDevToolsMessageObserver
{
    std::wstring                        url_;
    spl::shared_ptr<diagnostics::graph> graph_;
    core::monitor::state                state_;
    mutable std::mutex                  state_mutex_;
    caspar::timer                       tick_timer_;
    caspar::timer                       frame_timer_;
    caspar::timer                       paint_timer_;
    caspar::timer                       test_timer_;

    spl::shared_ptr<core::frame_factory>                        frame_factory_;
    core::video_format_desc                                     format_desc_;
    bool                                                        gpu_enabled_;
    tbb::concurrent_queue<std::wstring>                         javascript_before_load_;
    std::atomic<bool>                                           loaded_;
    std::atomic<bool>                                           not_found_;
    std::queue<std::pair<std::int_least64_t, core::draw_frame>> frames_;
    mutable std::mutex                                          frames_mutex_;
    const size_t                                                frames_max_size_ = 4;
    std::atomic<bool>                                           closing_;

    core::draw_frame   last_frame_;
    std::int_least64_t last_frame_time_;

    CefRefPtr<CefBrowser> browser_;

#ifdef WIN32
    std::shared_ptr<accelerator::d3d::d3d_device> const d3d_device_;
    std::shared_ptr<accelerator::d3d::d3d_texture2d>    d3d_shared_buffer_;
#endif

    // --- CDP virtual time (deterministic frame-by-frame rendering) -----------
    // Enabled via configuration.html.enable-virtual-time.
    // Uses CEF's built-in ExecuteDevToolsMethod / CefDevToolsMessageObserver to
    // drive Emulation.setVirtualTimePolicy without an external WebSocket.
    //
    // Threading model
    // ───────────────
    // vtc_thread_         dedicated ticker; blocks on budget expiry (fine,
    //                     it has its own thread).  Stays LOOK_AHEAD frames
    //                     ahead of the consumer so receive() is never blocked.
    // receive()           increments vtc_consumed_count_ and kicks the ticker.
    //                     Never waits for anything; returns still(last_frame_)
    //                     exactly like the non-VTC path when the buffer is empty.
    // OnDevToolsEvent     notifies vtc_budget_cv_ when virtualTimeBudgetExpired
    //                     arrives on TID_UI, unblocking the ticker thread.
    bool              vtc_enabled_ = false;
    std::atomic<bool> vtc_active_{false};
    // Rational frame duration (µs):  dur = vtc_frame_dur_num_us_ / vtc_frame_dur_den_
    uint64_t vtc_frame_dur_num_us_ = 0;
    uint64_t vtc_frame_dur_den_    = 1;
    // Ticks dispatched by the ticker thread (ticker-thread only, no sync needed)
    uint64_t vtc_frame_count_ = 0;
    // Frames "consumed" by receive() – incremented on every tick-eligible call,
    // whether or not a paint frame was available.  Lets the ticker know how far
    // the consumer has advanced even for static (no-paint) pages.
    std::atomic<uint64_t> vtc_consumed_count_{0};
    // Ticker-activation / consumer-catch-up CV (guarded by vtc_trigger_mutex_)
    std::mutex              vtc_trigger_mutex_;
    std::condition_variable vtc_trigger_cv_;
    // Budget-expiry CV: TID_UI → ticker thread  (guarded by vtc_budget_mutex_)
    std::mutex              vtc_budget_mutex_;
    std::condition_variable vtc_budget_cv_;
    bool                    vtc_budget_expired_ = false;
    // Paint-arrival CV: OnPaint → ticker thread (guarded by vtc_paint_mutex_).
    // The ticker waits for OnPaint between ticks; otherwise the next
    // setVirtualTimePolicy lands before the in-flight compositor frame has been
    // delivered to the host and Chromium discards it.  A real-world timeout
    // lets static pages (no visual changes -> no OnPaint) keep ticking.
    std::mutex              vtc_paint_mutex_;
    std::condition_variable vtc_paint_cv_;
    bool                    vtc_paint_received_ = false;
    // Diagnostic counters (ticker thread or TID_UI only).
    std::atomic<uint64_t>      vtc_paint_count_{0};
    std::atomic<uint64_t>      vtc_budget_expired_count_{0};
    std::thread                vtc_thread_;
    CefRefPtr<CefRegistration> vtc_registration_; // keeps observer alive

  public:
    html_client(spl::shared_ptr<core::frame_factory>       frame_factory,
                const spl::shared_ptr<diagnostics::graph>& graph,
                core::video_format_desc                    format_desc,
                bool                                       gpu_enabled,
                bool                                       shared_texture_enable,
                bool                                       vtc_enabled,
                std::wstring                               url)
        : url_(std::move(url))
        , graph_(graph)
        , frame_factory_(std::move(frame_factory))
        , format_desc_(std::move(format_desc))
        , gpu_enabled_(gpu_enabled)
        , shared_texture_enable_(shared_texture_enable)
#ifdef WIN32
        , d3d_device_(accelerator::d3d::d3d_device::get_device())
#endif
        , vtc_enabled_(vtc_enabled)
        // Rational frame duration in µs: duration * 1e6 / time_scale.
        // Using format_desc_.duration / time_scale rather than fps avoids
        // floating-point approximation for NTSC drop-frame rates.
        , vtc_frame_dur_num_us_(static_cast<uint64_t>(format_desc_.duration) * 1'000'000ULL)
        , vtc_frame_dur_den_(static_cast<uint64_t>(format_desc_.time_scale))
    {
        graph_->set_color("browser-tick-time", diagnostics::color(0.1f, 1.0f, 0.1f));
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.1f, 0.1f));
        graph_->set_color("overload", diagnostics::color(0.6f, 0.6f, 0.3f));
        graph_->set_color("buffered-frames", diagnostics::color(0.2f, 0.9f, 0.9f));
        graph_->set_text(print());
        diagnostics::register_graph(graph_);

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_["file/path"] = u8(url_);
        }

        loaded_    = false;
        not_found_ = false;
        closing_   = false;

        if (vtc_enabled_)
            vtc_thread_ = std::thread(&html_client::vtc_ticker_proc, this);
    }

    void reload()
    {
        html::begin_invoke([=] {
            if (browser_ != nullptr)
                browser_->Reload();
        });
    }

    void close()
    {
        closing_ = true;
        vtc_active_.store(false, std::memory_order_release);
        // Wake the ticker thread so it can see closing_ and exit.
        vtc_trigger_cv_.notify_all();
        {
            std::lock_guard<std::mutex> lock(vtc_budget_mutex_);
            vtc_budget_expired_ = true;
        }
        vtc_budget_cv_.notify_all();
        {
            std::lock_guard<std::mutex> lock(vtc_paint_mutex_);
            vtc_paint_received_ = true;
        }
        vtc_paint_cv_.notify_all();

        if (vtc_thread_.joinable())
            vtc_thread_.join();

        html::invoke([this] {
            vtc_registration_ = nullptr; // unregister DevTools observer
            if (browser_ != nullptr) {
                browser_->GetHost()->CloseBrowser(true);
            }
        });
    }

    bool try_pop(const core::video_field field)
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);

        if (!frames_.empty()) {
            /*
             * CEF in gpu-enabled mode only sends frames when something changes, and interlaced channels
             * consume two frames in a short time span.
             * This can interact poorly and cause the second
             * field of an animation repeat the first.
             * If there is a single field in the buffer, it may
             * want delaying to avoid this stutter.
             * The hazard here is that sometimes animations will
             * start a field later than intended.
             *
             * When virtual time is active the frame was explicitly requested
             * by a Tick(), so the "too young" heuristic must not apply.
             */
            if (!vtc_active_ && field == core::video_field::a && frames_.size() == 1) {
                auto now_time = now();

                // Make sure there has been a gap before this pop, of at least a couple of frames
                auto follows_gap_in_frames = (now_time - last_frame_time_) > 100;

                // Check if the sole buffered frame is too young to have a partner field generated (with a tolerance)
                auto time_per_frame           = (1000 * 1.5) / format_desc_.fps;
                auto front_frame_is_too_young = (now_time - frames_.front().first) < time_per_frame;

                if (follows_gap_in_frames && front_frame_is_too_young) {
                    return false;
                }
            }

            last_frame_time_ = frames_.front().first;
            last_frame_      = std::move(frames_.front().second);
            frames_.pop();

            graph_->set_value("buffered-frames", (double)frames_.size() / frames_max_size_);

            return true;
        }

        return false;
    }

    core::draw_frame receive(const core::video_field field)
    {
        // Signal the ticker thread that the consumer has advanced one frame.
        // Skip field B on interlaced – both fields share the same CEF frame.
        // This is purely a counter increment; receive() never blocks.
        if (vtc_active_.load(std::memory_order_acquire) && field != core::video_field::b) {
            vtc_consumed_count_.fetch_add(1, std::memory_order_release);
            vtc_trigger_cv_.notify_one();
        }

        if (!try_pop(field)) {
            graph_->set_tag(diagnostics::tag_severity::SILENT, "late-frame");
        }

        return last_frame_;
    }

    core::draw_frame last_frame() const { return last_frame_; }

    bool is_ready() const
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        return !frames_.empty() || last_frame_;
    }

    void execute_javascript(const std::wstring& javascript)
    {
        if (!loaded_) {
            javascript_before_load_.push(javascript);
        } else {
            execute_queued_javascript();
            do_execute_javascript(javascript);
        }
    }

    bool OnBeforePopup(CefRefPtr<CefBrowser>          browser,
                       CefRefPtr<CefFrame>            frame,
                       int                            popup_id,
                       const CefString&               target_url,
                       const CefString&               target_frame_name,
                       WindowOpenDisposition          target_disposition,
                       bool                           user_gesture,
                       const CefPopupFeatures&        popupFeatures,
                       CefWindowInfo&                 windowInfo,
                       CefRefPtr<CefClient>&          client,
                       CefBrowserSettings&            settings,
                       CefRefPtr<CefDictionaryValue>& dict,
                       bool*                          no_javascript_access) override
    {
        // This blocks popup windows from opening, as they dont make sense and hit an exception in get_browser_host upon
        // closing
        return true;
    }

    CefRefPtr<CefBrowserHost> get_browser_host() const
    {
        if (browser_ != nullptr)
            return browser_->GetHost();
        return nullptr;
    }

    core::monitor::state state() const
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }

  private:
    std::int_least64_t now()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();
    }

    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        rect = CefRect(0, 0, format_desc_.square_width, format_desc_.square_height);
    }

    void OnPaint(CefRefPtr<CefBrowser> browser,
                 PaintElementType      type,
                 const RectList&       dirtyRects,
                 const void*           buffer,
                 int                   width,
                 int                   height) override
    {
        if (shared_texture_enable_ || closing_ || not_found_ || !loaded_)
            return;

        graph_->set_value("browser-tick-time", paint_timer_.elapsed() * format_desc_.fps * 0.5);
        paint_timer_.restart();
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        if (type != PET_VIEW)
            return;

        core::pixel_format_desc pixel_desc(core::pixel_format::bgra);
        pixel_desc.planes.emplace_back(width, height, 4);

        core::mutable_frame frame = frame_factory_->create_frame(this, pixel_desc);
        char*               src   = (char*)buffer;
        char*               dst   = reinterpret_cast<char*>(frame.image_data(0).begin());
        test_timer_.restart();

#ifdef WIN32
        if (gpu_enabled_) {
            int chunksize = height * width;
            tbb::parallel_for(0, 4, [&](int y) { std::memcpy(dst + y * chunksize, src + y * chunksize, chunksize); });
        } else {
            std::memcpy(dst, src, width * height * 4);
        }
#else
        // On my one test linux machine, doing a single memcpy doesn't have the same cost as windows,
        // making using tbb excessive
        std::memcpy(dst, src, width * height * 4);
#endif

        graph_->set_value("memcpy", test_timer_.elapsed() * format_desc_.fps * 0.5 * 5);

        {
            std::lock_guard<std::mutex> lock(frames_mutex_);

            frames_.push(std::make_pair(now(), core::draw_frame(std::move(frame))));
            while (frames_.size() > 4) {
                frames_.pop();
                graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
            }
            graph_->set_value("buffered-frames", (double)frames_.size() / frames_max_size_);
        }

        // Unblock the VTC ticker so it can send the next tick.
        if (vtc_active_.load(std::memory_order_acquire)) {
            const uint64_t n = vtc_paint_count_.fetch_add(1, std::memory_order_release) + 1;
            {
                std::lock_guard<std::mutex> lock(vtc_paint_mutex_);
                vtc_paint_received_ = true;
            }
            vtc_paint_cv_.notify_one();
            if (n <= 5 || n % 60 == 0)
                CASPAR_LOG(info) << print() << L" [vtc] OnPaint #" << n;
        }
    }

#ifdef WIN32
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>          browser,
                            PaintElementType               type,
                            const RectList&                dirtyRects,
                            const CefAcceleratedPaintInfo& info) override
    {
        try {
            if (!shared_texture_enable_ || closing_ || not_found_ || !loaded_)
                return;

            graph_->set_value("browser-tick-time", paint_timer_.elapsed() * format_desc_.fps * 0.5);
            paint_timer_.restart();
            CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

            if (type != PET_VIEW)
                return;

            if (d3d_shared_buffer_) {
                if (info.shared_texture_handle != d3d_shared_buffer_->share_handle())
                    d3d_shared_buffer_.reset();
            }

            if (!d3d_shared_buffer_) {
                d3d_shared_buffer_ = d3d_device_->open_shared_texture(info.shared_texture_handle);
                if (!d3d_shared_buffer_)
                    CASPAR_LOG(error) << print() << L" could not open shared texture!";
            }

            if (d3d_shared_buffer_) {
                core::pixel_format format = core::pixel_format::invalid;
                if (d3d_shared_buffer_->format() == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    format = core::pixel_format::bgra;
                } else if (d3d_shared_buffer_->format() == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    format = core::pixel_format::rgba;
                }

                if (format != core::pixel_format::invalid) {
                    auto frame =
                        frame_factory_->import_d3d_texture(this, d3d_shared_buffer_, format, common::bit_depth::bit8);
                    core::draw_frame dframe(std::move(frame));

                    {
                        std::lock_guard<std::mutex> lock(frames_mutex_);

                        frames_.push(presentation_frame(std::move(dframe)));
                        while (frames_.size() > 4) {
                            frames_.pop();
                            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
                        }
                        graph_->set_value("buffered-frames", (double)frames_.size() / frames_max_size_);
                    }

                    // Unblock the VTC ticker so it can send the next tick.
                    if (vtc_active_.load(std::memory_order_acquire)) {
                        const uint64_t n = vtc_paint_count_.fetch_add(1, std::memory_order_release) + 1;
                        {
                            std::lock_guard<std::mutex> lock(vtc_paint_mutex_);
                            vtc_paint_received_ = true;
                        }
                        vtc_paint_cv_.notify_one();
                        if (n <= 5 || n % 60 == 0)
                            CASPAR_LOG(info) << print() << L" [vtc] OnAcceleratedPaint #" << n;
                    }
                }
            }
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
    }
#endif

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        browser_ = std::move(browser);

        vtc_registration_ = browser_->GetHost()->AddDevToolsMessageObserver(this);

        // The Page domain must be enabled before any Page.* events are dispatched.
        if (browser_->GetHost()->ExecuteDevToolsMethod(0, "Page.enable", nullptr) == 0)
            CASPAR_LOG(error) << print() << L" failed to enable Page domain";

        // Enable Page lifecycle events so we receive Page.lifecycleEvent (including firstContentfulPaint).
        auto params = CefDictionaryValue::Create();
        params->SetBool("enabled", true);
        if (browser_->GetHost()->ExecuteDevToolsMethod(0, "Page.setLifecycleEventsEnabled", params) == 0)
            CASPAR_LOG(error) << print() << L" failed to enable Page lifecycle events";
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        browser_ = nullptr;
    }

    bool DoClose(CefRefPtr<CefBrowser> browser) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        return false;
    }

    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t    level,
                          const CefString&      message,
                          const CefString&      source,
                          int                   line) override
    {
        if (level == cef_log_severity_t::LOGSEVERITY_DEBUG)
            CASPAR_LOG(debug) << print() << L" Log: " << message.ToWString();
        else if (level == cef_log_severity_t::LOGSEVERITY_WARNING)
            CASPAR_LOG(warning) << print() << L" Log: " << message.ToWString();
        else if (level == cef_log_severity_t::LOGSEVERITY_ERROR)
            CASPAR_LOG(error) << print() << L" Log: " << message.ToWString();
        else if (level == cef_log_severity_t::LOGSEVERITY_FATAL)
            CASPAR_LOG(fatal) << print() << L" Log: " << message.ToWString();
        else
            CASPAR_LOG(info) << print() << L" Log: " << message.ToWString();
        return true;
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

    void OnLoadError(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame>   frame,
                     ErrorCode             errorCode,
                     const CefString&      errorText,
                     const CefString&      failedUrl) override
    {
        not_found_ = true;
        CASPAR_LOG(warning) << "[html_producer] " << errorText.ToString() << " while loading url: \""
                            << failedUrl.ToString() << "\"";

        // Stop producing if the page fails to load
        {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            frames_.push(std::make_pair(now(), core::draw_frame{}));
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_ = {};
        }
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override
    {
        if (not_found_)
            return;

        execute_queued_javascript();

        if (vtc_enabled_) {
            CASPAR_ASSERT(CefCurrentlyOn(TID_UI));
            // Reset frame counter so tick budgets restart from t=0 of the page.
            vtc_frame_count_ = 0;

            // Pause the browser's virtual clock.  From this point every frame
            // must be explicitly advanced by a Tick() in receive().
            auto params = CefDictionaryValue::Create();
            params->SetString("policy", "pause");
            browser->GetHost()->ExecuteDevToolsMethod(0, "Emulation.setVirtualTimePolicy", params);

            vtc_active_.store(true, std::memory_order_release);
            vtc_trigger_cv_.notify_all(); // wake ticker thread
            CASPAR_LOG(info) << print() << L" [vtc] OnLoadEnd – virtual time paused, ticker armed (loaded_="
                             << (loaded_ ? L"true" : L"false") << L")";
        }
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>        browser,
                                  CefRefPtr<CefFrame>          frame,
                                  CefProcessId                 source_process,
                                  CefRefPtr<CefProcessMessage> message) override
    {
        auto name = message->GetName().ToString();

        if (name == REMOVE_MESSAGE_NAME) {
            // TODO fully remove producer
            this->close();

            {
                std::lock_guard<std::mutex> lock(frames_mutex_);
                frames_.push(std::make_pair(now(), core::draw_frame::empty()));
            }

            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_ = {};
            }

            return true;
        }
        if (name == LOG_MESSAGE_NAME) {
            auto args     = message->GetArgumentList();
            auto severity = static_cast<boost::log::trivial::severity_level>(args->GetInt(0));
            auto msg      = args->GetString(1).ToWString();

            BOOST_LOG_SEV(log::logger::get(), severity) << print() << L" [renderer_process] " << msg;
        }

        return false;
    }

    void do_execute_javascript(const std::wstring& javascript)
    {
        html::begin_invoke([=] {
            if (browser_ != nullptr)
                browser_->GetMainFrame()->ExecuteJavaScript(
                    u8(javascript).c_str(), browser_->GetMainFrame()->GetURL(), 0);
        });
    }

    void execute_queued_javascript()
    {
        std::wstring javascript;

        while (javascript_before_load_.try_pop(javascript))
            do_execute_javascript(javascript);
    }

    std::wstring print() const
    {
        return L"html[" + url_ + L"]" + L" " + std::to_wstring(format_desc_.square_width) + L" " +
               std::to_wstring(format_desc_.square_height) + L" " + std::to_wstring(format_desc_.fps);
    }

    // --- CDP virtual time: ticker thread ------------------------------------

    // Runs on vtc_thread_.  Stays LOOK_AHEAD frames ahead of the channel
    // frame-pump (receive) by watching vtc_consumed_count_.
    //
    // Design invariant: vtc_frame_count_ <= vtc_consumed_count_ + LOOK_AHEAD
    //
    // For animated pages: each tick produces an OnPaint; the buffer fills up
    // to LOOK_AHEAD frames and the channel pump drains it smoothly.
    //
    // For static pages: ticks fire but no OnPaint arrives; the buffer stays
    // empty and receive() returns still(last_frame_) with zero delay.
    void vtc_ticker_proc()
    {
        constexpr uint64_t LOOK_AHEAD = 2; // frames to stay ahead of consumer

        // Wait for OnLoadEnd to arm virtual time before doing anything.
        {
            std::unique_lock<std::mutex> lock(vtc_trigger_mutex_);
            vtc_trigger_cv_.wait(lock, [this] { return vtc_active_.load(std::memory_order_acquire) || closing_; });
        }

        while (!closing_) {
            // ── Throttle: don't get more than LOOK_AHEAD ticks ahead ──────
            {
                std::unique_lock<std::mutex> lock(vtc_trigger_mutex_);
                vtc_trigger_cv_.wait(lock, [&] {
                    const uint64_t consumed = vtc_consumed_count_.load(std::memory_order_acquire);
                    return vtc_frame_count_ < consumed + LOOK_AHEAD || !vtc_active_.load(std::memory_order_acquire) ||
                           closing_;
                });
            }
            if (closing_ || !vtc_active_.load(std::memory_order_acquire))
                break;

            // ── Send one tick ──────────────────────────────────────────────
            const double budget_ms = vtc_next_budget_ms();

            {
                std::lock_guard<std::mutex> lock(vtc_budget_mutex_);
                vtc_budget_expired_ = false;
            }
            {
                std::lock_guard<std::mutex> lock(vtc_paint_mutex_);
                vtc_paint_received_ = false;
            }

            const uint64_t tick_n = vtc_frame_count_ + 1;
            if (tick_n <= 5 || tick_n % 60 == 0)
                CASPAR_LOG(info) << print() << L" [vtc] tick #" << tick_n << L" budget=" << budget_ms << L"ms";

            html::begin_invoke([this, budget_ms] {
                if (browser_ == nullptr || closing_)
                    return;
                auto params = CefDictionaryValue::Create();
                params->SetString("policy", "pauseIfNetworkFetchesPending");
                params->SetDouble("budget", budget_ms);
                browser_->GetHost()->ExecuteDevToolsMethod(0, "Emulation.setVirtualTimePolicy", params);
            });

            // ── Wait for virtualTimeBudgetExpired from TID_UI ──────────────
            // Bounded so a hung page (e.g. unresolved network fetch under
            // pauseIfNetworkFetchesPending) cannot wedge the ticker forever.
            bool budget_ok = false;
            {
                std::unique_lock<std::mutex> lock(vtc_budget_mutex_);
                budget_ok = vtc_budget_cv_.wait_for(
                    lock, std::chrono::seconds(2), [this] { return vtc_budget_expired_ || closing_; });
            }
            if (closing_)
                break;
            if (!budget_ok) {
                CASPAR_LOG(warning) << print() << L" [vtc] tick #" << tick_n
                                    << L" budget wait TIMED OUT (no virtualTimeBudgetExpired)";
                // Still advance frame count so we don't get stuck repeating the same tick.
                ++vtc_frame_count_;
                continue;
            }

            // ── Wait for OnPaint to consume the in-flight compositor frame ─
            // Without this, the next setVirtualTimePolicy lands before the
            // frame has been delivered to the host and Chromium discards it.
            // Static pages produce no paint, so timeout lets us keep ticking.
            bool paint_ok = false;
            {
                std::unique_lock<std::mutex> lock(vtc_paint_mutex_);
                paint_ok = vtc_paint_cv_.wait_for(
                    lock, std::chrono::milliseconds(250), [this] { return vtc_paint_received_ || closing_; });
            }
            if (closing_)
                break;
            if (!paint_ok && (tick_n <= 5 || tick_n % 60 == 0))
                CASPAR_LOG(info) << print() << L" [vtc] tick #" << tick_n << L" paint wait timed out (no OnPaint)";

            ++vtc_frame_count_;
        }
    }

    // --- CDP virtual time helpers -------------------------------------------

    // Cumulative virtual time at the end of frame N (microseconds).
    // The Bresenham-style accumulation ensures individual per-frame budgets
    // sum to exactly the correct total, even for NTSC drop-frame rates where
    // naive float arithmetic would accumulate error.
    uint64_t vtc_cumulative_us(uint64_t frame_idx) const
    {
        // frame_idx * num / den  (µs).
        // For typical values (frame_idx < 10^6, num < 10^9) the product fits
        // comfortably in uint64_t without 128-bit arithmetic.
        return (frame_idx * vtc_frame_dur_num_us_) / vtc_frame_dur_den_;
    }

    // Budget (ms) for the next tick – advances from vtc_frame_count_ to vtc_frame_count_+1.
    double vtc_next_budget_ms() const
    {
        const uint64_t us = vtc_cumulative_us(vtc_frame_count_ + 1) - vtc_cumulative_us(vtc_frame_count_);
        return static_cast<double>(us) / 1000.0;
    }

    // --- CefDevToolsMessageObserver -----------------------------------------

    // Called on TID_UI for every event pushed by the browser (unsolicited).
    void OnDevToolsEvent(CefRefPtr<CefBrowser> /*browser*/,
                         const CefString& method,
                         const void*      message,
                         size_t           message_size) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));
        if (method == "Emulation.virtualTimeBudgetExpired") {
            // Unblock the ticker thread so it can evaluate whether to send
            // the next tick.  OnPaint may or may not follow (static pages
            // never paint); the ticker does not wait for it.
            {
                std::lock_guard<std::mutex> lock(vtc_budget_mutex_);
                vtc_budget_expired_ = true;
            }
            vtc_budget_cv_.notify_one();
            const uint64_t n = vtc_budget_expired_count_.fetch_add(1, std::memory_order_release) + 1;
            if (n <= 5 || n % 60 == 0)
                CASPAR_LOG(info) << print() << L" [vtc] virtualTimeBudgetExpired #" << n;
        } else if (method == "Page.lifecycleEvent") {
            // Parse the event name from the JSON payload to find firstContentfulPaint.
            // The payload is: {"frameId":"...","loaderId":"...","name":"firstContentfulPaint","timestamp":...}
            // We do a simple substring search to avoid pulling in a JSON parser.
            const std::string payload(static_cast<const char*>(message), message_size);
            if (payload.find("\"firstContentfulPaint\"") != std::string::npos) {
                loaded_ = true;
                CASPAR_LOG(info) << print() << L" firstContentfulPaint – frame capture started";
            }
        }
    }

    // Called on TID_UI with the result of each ExecuteDevToolsMethod call.
    void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser,
                                int                   message_id,
                                bool                  success,
                                const void*           message,
                                size_t                message_size) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));
    }

    IMPLEMENT_REFCOUNTING(html_client);
};

class html_producer : public core::frame_producer
{
    core::video_format_desc             format_desc_;
    const std::wstring                  url_;
    spl::shared_ptr<diagnostics::graph> graph_;

    CefRefPtr<html_client> client_;

  public:
    html_producer(const spl::shared_ptr<core::frame_factory>& frame_factory,
                  const core::video_format_desc&              format_desc,
                  const std::wstring&                         url)
        : format_desc_(format_desc)
        , url_(url)
    {
        html::invoke([&] {
            const bool enable_gpu = env::properties().get(L"configuration.html.enable-gpu", false);
            const bool vtc        = env::properties().get(L"configuration.html.enable-virtual-time", false);

            client_ = new html_client(frame_factory, graph_, format_desc, enable_gpu, false, vtc, url_);

            CefWindowInfo window_info;
            window_info.bounds.width                 = format_desc.square_width;
            window_info.bounds.height                = format_desc.square_height;
            window_info.windowless_rendering_enabled = true;

            CefBrowserSettings browser_settings;
            browser_settings.webgl = enable_gpu ? cef_state_t::STATE_ENABLED : cef_state_t::STATE_DISABLED;
            double fps             = format_desc.fps;
            browser_settings.windowless_frame_rate = int(ceil(fps));
            CefBrowserHost::CreateBrowser(window_info, client_.get(), url, browser_settings, nullptr, nullptr);
        });
    }

    ~html_producer() override
    {
        if (client_ != nullptr)
            client_->close();
    }

    // frame_producer

    std::wstring name() const override { return L"html"; }

    core::draw_frame receive_impl(const core::video_field field, int nb_samples) override
    {
        if (client_ != nullptr) {
            return client_->receive(field);
        }

        return core::draw_frame::empty();
    }

    core::draw_frame first_frame(const core::video_field field) override { return receive_impl(field, 0); }

    bool is_ready() override
    {
        if (client_ != nullptr) {
            return client_->is_ready();
        }
        return false;
    }

    core::draw_frame last_frame(const core::video_field field) override
    {
        if (client_ != nullptr) {
            return client_->last_frame();
        }

        return core::draw_frame::empty();
    }

    std::future<std::wstring> call(const std::vector<std::wstring>& params) override
    {
        if (client_ == nullptr)
            return make_ready_future(std::wstring());

        auto javascript = params.at(0);

        if (javascript == L"RELOAD") {
            client_->reload();
        } else {
            client_->execute_javascript(javascript);
        }

        return make_ready_future(std::wstring());
    }

    std::wstring print() const override { return L"html[" + url_ + L"]"; }

    core::monitor::state state() const override
    {
        if (client_ != nullptr) {
            return client_->state();
        }

        static const core::monitor::state empty;
        return empty;
    }
};

spl::shared_ptr<core::frame_producer> create_cg_producer(const core::frame_producer_dependencies& dependencies,
                                                         const std::vector<std::wstring>&         params)
{
    const auto html_prefix    = boost::iequals(params.at(0), L"[HTML]");
    const auto param_url      = html_prefix ? params.at(1) : params.at(0);
    const auto filename       = env::template_folder() + param_url + L".html";
    const auto found_filename = find_case_insensitive(filename);
    const auto http_prefix =
        boost::algorithm::istarts_with(param_url, L"http:") || boost::algorithm::istarts_with(param_url, L"https:");

    if (!found_filename && !http_prefix && !html_prefix)
        return core::frame_producer::empty();

    const auto url = found_filename ? L"file://" + *found_filename : param_url;

    std::optional<int> width;
    std::optional<int> height;
    {
        auto u8_url = u8(url);

        boost::smatch what;
        if (boost::regex_search(u8_url, what, boost::regex("width=([0-9]+)"))) {
            width = std::stoi(what[1].str());
        }

        if (boost::regex_search(u8_url, what, boost::regex("height=([0-9]+)"))) {
            height = std::stoi(what[1].str());
        }
    }

    auto format_desc = dependencies.format_desc;
    if (width && height) {
        format_desc.width         = *width;
        format_desc.square_width  = *width;
        format_desc.height        = *height;
        format_desc.square_height = *height;
    }

    return spl::make_shared<html_producer>(dependencies.frame_factory, format_desc, url);
}

spl::shared_ptr<core::frame_producer> create_producer(const core::frame_producer_dependencies& dependencies,
                                                      const std::vector<std::wstring>&         params)
{
    return create_cg_producer(dependencies, params);
}

}} // namespace caspar::html
