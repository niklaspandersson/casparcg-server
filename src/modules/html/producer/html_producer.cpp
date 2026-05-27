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

#if defined(__GNUC__) && __GNUC__ == 14
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/regex.hpp>
#if defined(__GNUC__) && __GNUC__ == 14
#pragma GCC diagnostic pop
#endif

#include <tbb/concurrent_queue.h>
#include <tbb/parallel_for.h>

#include <mutex>

#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_devtools_message_observer.h>
#include <include/cef_registration.h>
#include <include/cef_render_handler.h>
#include <include/cef_values.h>

#include <chrono>
#include <condition_variable>
#include <optional>
#include <queue>
#include <utility>

#include <ffmpeg/util/audio_resampler.h>

#include "../html.h"
#include "../util.h"

#ifdef WIN32
#include <accelerator/d3d/d3d_device.h>
#include <accelerator/d3d/d3d_device_context.h>
#include <accelerator/d3d/d3d_texture2d.h>
#endif

namespace caspar { namespace html {

inline std::int_least64_t now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

struct presentation_frame
{
    std::int_least64_t timestamp;
    core::draw_frame   frame;

    explicit presentation_frame(core::draw_frame frame = {}, std::int_least64_t ts = now()) noexcept
        : timestamp(ts)
        , frame(std::move(frame))
    {
    }

    presentation_frame(presentation_frame&& other) noexcept
        : timestamp(other.timestamp)
        , frame(std::move(other.frame))
    {
    }

    presentation_frame(const presentation_frame&)            = delete;
    presentation_frame& operator=(const presentation_frame&) = delete;

    presentation_frame& operator=(presentation_frame&& rhs) noexcept
    {
        timestamp = rhs.timestamp;
        frame     = std::move(rhs.frame);
        return *this;
    }

    ~presentation_frame() {}
};

class html_client
    : public CefClient
    , public CefRenderHandler
    , public CefAudioHandler
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

    spl::shared_ptr<core::frame_factory> frame_factory_;
    core::video_format_desc              format_desc_;
    bool                                 gpu_enabled_;
    bool                                 shared_texture_enable_;
    bool                                 vtc_enabled_;
    bool                                 wait_for_fp_;
    tbb::concurrent_queue<std::wstring>  javascript_before_load_;
    std::atomic<bool>                    loaded_;
    std::atomic<bool>                    not_found_;
    std::queue<presentation_frame>       frames_;
    std::queue<presentation_frame>       audio_frames_;
    mutable std::mutex                   frames_mutex_;
    mutable std::mutex                   audio_frames_mutex_;
    const size_t                         frames_max_size_ = 4;
    std::atomic<bool>                    closing_;

    std::unique_ptr<ffmpeg::AudioResampler> audioResampler_;

    core::draw_frame   last_video_frame_;
    core::draw_frame   last_frame_;
    std::int_least64_t last_frame_time_;

    CefRefPtr<CefBrowser>      browser_;
    CefRefPtr<CefRegistration> cdp_registration_; // keeps DevTools observer alive

    // ── Virtual time / external BeginFrame state ────────────────────────────
    // Active when vtc_enabled_ is true. Drives one tick per channel pull:
    // each wait_for_frame() advances virtual time by one frame duration and
    // issues one HeadlessExperimental.beginFrame, then blocks until both the
    // virtualTimeBudgetExpired event and OnPaint have been observed for that
    // tick.
    // ready_to_render_ flips once the page is ready for pulled ticks. In vtc mode that
    // means both load and firstPaint have been observed (in either order) and virtual
    // time has been paused. In non-vtc mode it's just OnLoadEnd.
    std::atomic<bool>       first_paint_seen_{false};
    std::atomic<bool>       ready_to_render_{false};
    mutable std::mutex      ready_mutex_;
    std::condition_variable ready_cv_;

    std::mutex              tick_mutex_;
    std::condition_variable tick_cv_;
    bool                    tick_budget_expired_ = false;
    bool                    tick_paint_received_ = false;

    uint64_t frame_count_ = 0; // number of vtc ticks issued

    // ── Marker-sync (TID_UI only) ───────────────────────────────────────────
    // A unique per-tick color marker is stamped into the page's top-left corner
    // (window.__casparMark) so OnPaint can correlate a paint buffer to the
    // committed frame and discard stale / partially-composited paints (CEF
    // issue #4166). These are only touched on the CEF UI thread — tick()'s
    // dispatched lambda, the virtualTimeBudgetExpired handler and OnPaint all run
    // there — so they need no extra locking. The marker is overwritten before the
    // frame is queued so it never reaches the output.
    static constexpr int MARK_W              = 6; // marker block size (device px)
    static constexpr int MARK_H              = 6;
    static constexpr int MARKER_TOL          = 24;        // per-channel match tolerance
    static constexpr int SYNC_PUMP_MS        = 4;         // pacing between begin-frames (renderer breathing room)
    static constexpr int SYNC_TIMEOUT_MS     = 250;       // wall-clock budget to get the marker-matching paint
    uint8_t              expected_marker_[3] = {0, 0, 0}; // r,g,b expected this tick (TID_UI)
    std::atomic<int>     sync_retries_       = 0;         // begin-frames pumped this tick (diagnostics)
    std::atomic<int>     paints_this_tick_   = 0;         // OnPaint calls observed this tick (diagnostics)
    bool                 force_accept_       = false;     // give-up: accept next paint regardless of marker

#ifdef WIN32
    std::shared_ptr<accelerator::d3d::d3d_device> const d3d_device_;
    std::shared_ptr<accelerator::d3d::d3d_texture2d>    d3d_shared_buffer_;
#endif

  public:
    html_client(spl::shared_ptr<core::frame_factory>       frame_factory,
                const spl::shared_ptr<diagnostics::graph>& graph,
                core::video_format_desc                    format_desc,
                bool                                       gpu_enabled,
                bool                                       shared_texture_enable,
                bool                                       vtc_enabled,
                bool                                       wait_for_fp,
                std::wstring                               url)
        : url_(std::move(url))
        , graph_(graph)
        , frame_factory_(std::move(frame_factory))
        , format_desc_(std::move(format_desc))
        , gpu_enabled_(gpu_enabled)
        , shared_texture_enable_(shared_texture_enable)
        , vtc_enabled_(vtc_enabled)
        , wait_for_fp_(wait_for_fp)
#ifdef WIN32
        , d3d_device_(accelerator::d3d::d3d_device::get_device())
#endif
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
    }

    void reload()
    {
        html::begin_invoke([this] {
            if (browser_ != nullptr)
                browser_->Reload();
        });
    }

    void close()
    {
        closing_ = true;

        // Wake anything blocked in wait_for_frame / tick so it can observe closing_.
        ready_cv_.notify_all();
        tick_cv_.notify_all();

        html::invoke([this] {
            cdp_registration_ = nullptr; // unregister DevTools observer
            if (browser_ != nullptr) {
                browser_->GetHost()->CloseBrowser(true);
            }
        });
    }

    bool supports_deterministic_sync() const { return vtc_enabled_; }

    // Pull-based tick driver used by the deterministic channel.
    // Returns true once a frame is queued and ready for receive(). Each
    // successful call advances virtual time by one frame duration and issues
    // exactly one BeginFrame, so the channel and the page tick in lockstep.
    bool wait_for_frame(std::chrono::milliseconds timeout)
    {
        if (!vtc_enabled_) {
            return is_ready();
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;

        // Wait for first paint (which we treat as "page has produced at least
        // one frame and is ready for ticked rendering").
        {
            std::unique_lock<std::mutex> lock(ready_mutex_);
            if (!ready_cv_.wait_until(
                    lock, deadline, [&] { return ready_to_render_.load() || closing_ || not_found_; }))
                return false;
            if (closing_ || not_found_)
                return false;
        }

        // If this tick's frame is already queued (not yet popped), nothing to do.
        // Note: we must check the queue, not is_ready() — is_ready() also reports
        // true once last_frame_ is set, which persists after the first pop and would
        // suppress every subsequent tick, freezing the render on frame 1.
        {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            if (!frames_.empty())
                return true;
        }

        return tick(deadline);
    }

    bool try_pop(const core::video_field field)
    {
        bool                        result = false;
        std::lock_guard<std::mutex> lock(frames_mutex_);

        core::draw_frame audio_frame;
        uint64_t         audio_frame_timestamp = 0;

        {
            std::lock_guard<std::mutex> audio_lock(audio_frames_mutex_);
            if (!audio_frames_.empty()) {
                audio_frame_timestamp = audio_frames_.front().timestamp;
                audio_frame           = core::draw_frame(std::move(audio_frames_.front().frame));
                audio_frames_.pop();
            }
        }

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
             */
            if (!vtc_enabled_ && field == core::video_field::a && frames_.size() == 1) {
                auto now_time = now();

                // Make sure there has been a gap before this pop, of at least a couple of frames
                auto follows_gap_in_frames = (now_time - last_frame_time_) > 100;

                // Check if the sole buffered frame is too young to have a partner field generated (with a tolerance)
                auto time_per_frame           = (1000 * 1.5) / format_desc_.fps;
                auto front_frame_is_too_young = (now_time - frames_.front().timestamp) < time_per_frame;

                if (follows_gap_in_frames && front_frame_is_too_young) {
                    return false;
                }
            }

            last_frame_time_  = frames_.front().timestamp;
            last_video_frame_ = std::move(frames_.front().frame);
            last_frame_       = last_video_frame_;
            frames_.pop();

            graph_->set_value("buffered-frames", (double)frames_.size() / frames_max_size_);

            result = true;
        }

        if (audio_frame) {
            last_frame_time_ = audio_frame_timestamp;
            last_frame_      = core::draw_frame::over(last_video_frame_, audio_frame);
            result           = true;
        }

        return result;
    }

    core::draw_frame receive(const core::video_field field)
    {
        if (!try_pop(field)) {
            graph_->set_tag(diagnostics::tag_severity::SILENT, "late-frame");
            return core::draw_frame::still(last_frame_);
        } else {
            return last_frame_;
        }
    }

    core::draw_frame last_frame() const { return core::draw_frame::still(last_frame_); }

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
        if (shared_texture_enable_ || closing_ || not_found_)
            return;

        // Drop paints arriving before the page is ready (only when vtc or wait-for-fp
        // is enabled). Avoids the empty initial frame being captured as the first frame.
        if ((vtc_enabled_ || wait_for_fp_) && !ready_to_render_.load())
            return;

        graph_->set_value("browser-tick-time", paint_timer_.elapsed() * format_desc_.fps * 0.5);
        paint_timer_.restart();
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        if (type != PET_VIEW)
            return;

        // Marker-sync: in deterministic mode, accept a paint only if it carries THIS tick's
        // marker (CEF #4166 — stale/partial paints show an earlier marker). Crucially this
        // rejects leftover paints from previous ticks, so a tick can never be satisfied by an
        // old frame (which dropped/duplicated frames and made playback choppy). tick() drives
        // the begin-frame pumping; here we only judge and, on a match, queue the frame.
        if (vtc_enabled_) {
            ++paints_this_tick_;
            const auto* buf = static_cast<const unsigned char*>(buffer);
            int         rr = 0, gg = 0, bb = 0;
            bool        matched = false;
            const int   cx      = MARK_W / 2;
            const int   cy      = MARK_H / 2;
            if (buf != nullptr && cx < width && cy < height) {
                const size_t o = (static_cast<size_t>(cy) * width + cx) * 4;
                rr             = buf[o + 2];
                gg             = buf[o + 1];
                bb             = buf[o + 0];
                const int dr   = rr - expected_marker_[0];
                const int dg   = gg - expected_marker_[1];
                const int db   = bb - expected_marker_[2];
                matched        = dr <= MARKER_TOL && dr >= -MARKER_TOL && dg <= MARKER_TOL && dg >= -MARKER_TOL &&
                          db <= MARKER_TOL && db >= -MARKER_TOL;
            }

            bool accept;
            {
                std::lock_guard<std::mutex> lock(tick_mutex_);
                // Only once per tick, only after this tick's budget expired, only on a marker
                // match (or a forced give-up). OnPaint is serialized on TID_UI, so the next
                // paint sees tick_paint_received_ already set and is discarded.
                accept = tick_budget_expired_ && !tick_paint_received_ && (matched || force_accept_);
            }

            if (!accept)
                return; // discard stale / pre-budget / duplicate paint; tick() pumps the next
        }

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

        // Hide the corner marker: overwrite the marker block with the pixel just to its
        // right so it never reaches the output (a few-pixel corner smear at worst).
        if (vtc_enabled_ && width > MARK_W) {
            for (int y = 0; y < MARK_H && y < height; ++y) {
                char*       row = dst + static_cast<size_t>(y) * width * 4;
                const char* nbr = row + static_cast<size_t>(MARK_W) * 4;
                for (int x = 0; x < MARK_W; ++x)
                    std::memcpy(row + static_cast<size_t>(x) * 4, nbr, 4);
            }
        }

        {
            std::lock_guard<std::mutex> lock(frames_mutex_);

            core::draw_frame new_frame = core::draw_frame(std::move(frame));

            frames_.push(presentation_frame(std::move(new_frame)));
            while (frames_.size() > frames_max_size_) {
                frames_.pop();
                graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
            }
            graph_->set_value("buffered-frames", (double)frames_.size() / frames_max_size_);
        }

        notify_tick_paint();
    }

#ifdef WIN32
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>          browser,
                            PaintElementType               type,
                            const RectList&                dirtyRects,
                            const CefAcceleratedPaintInfo& info) override
    {
        try {
            if (!shared_texture_enable_ || closing_ || not_found_)
                return;

            if ((vtc_enabled_ || wait_for_fp_) && !ready_to_render_.load())
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
                }
            }

            notify_tick_paint();
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
    }
#endif

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        browser_ = std::move(browser);

        if (vtc_enabled_ || wait_for_fp_) {
            cdp_registration_ = browser_->GetHost()->AddDevToolsMessageObserver(this);

            // Page.enable + lifecycle events: needed for firstPaint detection.
            browser_->GetHost()->ExecuteDevToolsMethod(0, "Page.enable", nullptr);

            auto lc = CefDictionaryValue::Create();
            lc->SetBool("enabled", true);
            browser_->GetHost()->ExecuteDevToolsMethod(0, "Page.setLifecycleEventsEnabled", lc);

            // if (vtc_enabled_) {
            //     // Run the video-neutralizer at document-start on every document (before any page
            //     // script), so a <video> never enters the not-ready/playing state that stalls
            //     // compositing. See video_neutralizer_script().
            //     auto add = CefDictionaryValue::Create();
            //     add->SetString("source", video_neutralizer_script());
            //     browser_->GetHost()->ExecuteDevToolsMethod(0, "Page.addScriptToEvaluateOnNewDocument", add);
            // }
        }
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
        auto msg = log::replace_nonprintable_copy(message.ToWString(), L'?');
        if (level == cef_log_severity_t::LOGSEVERITY_DEBUG)
            CASPAR_LOG(debug) << print() << L" Log: " << msg;
        else if (level == cef_log_severity_t::LOGSEVERITY_WARNING)
            CASPAR_LOG(warning) << print() << L" Log: " << msg;
        else if (level == cef_log_severity_t::LOGSEVERITY_ERROR)
            CASPAR_LOG(error) << print() << L" Log: " << msg;
        else if (level == cef_log_severity_t::LOGSEVERITY_FATAL)
            CASPAR_LOG(fatal) << print() << L" Log: " << msg;
        else
            CASPAR_LOG(info) << print() << L" Log: " << msg;
        return true;
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }

    CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }

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
            frames_.push(presentation_frame());
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_ = {};
        }

        // Wake anything blocked in wait_for_frame so it can observe not_found_ and bail out.
        ready_cv_.notify_all();
        {
            std::lock_guard<std::mutex> lock(tick_mutex_);
        }
        tick_cv_.notify_all();
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override
    {
        if (not_found_)
            return;

        loaded_ = true;
        execute_queued_javascript();

        maybe_arm_ready();
    }

    // Called when either firstPaint or load has fired. Flips ready_to_render_ once
    // the page is in a state suitable for the channel to start pulling frames. In
    // vtc mode this also pauses the page's virtual clock so subsequent ticks own
    // the timeline.
    void maybe_arm_ready()
    {
        if (!loaded_.load())
            return;
        // In vtc mode the compositor only paints in response to a BeginFrame we
        // issue from tick(), and we don't tick until ready_to_render_ is set — so
        // gating readiness on firstPaint here would deadlock (firstPaint needs a
        // paint needs a BeginFrame needs ready). Arm on load alone; the first
        // tick's BeginFrame produces the first painted frame. The firstPaint gate
        // still applies in the non-vtc wait-for-fp case, where CEF paints on its
        // own and we only want to start once real content has been rendered.
        if (wait_for_fp_ && !vtc_enabled_ && !first_paint_seen_.load())
            return;
        if (ready_to_render_.load())
            return;

        if (vtc_enabled_) {
            CASPAR_ASSERT(CefCurrentlyOn(TID_UI));
            // Freeze the page's virtual clock. From here, every tick() advances time by
            // exactly one frame duration via a budgeted setVirtualTimePolicy + beginFrame.
            auto p = CefDictionaryValue::Create();
            p->SetString("policy", "pause");
            browser_->GetHost()->ExecuteDevToolsMethod(0, "Emulation.setVirtualTimePolicy", p);
        }

        ready_to_render_ = true;
        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
        }
        ready_cv_.notify_all();
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
                frames_.push(presentation_frame());
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
            auto msg      = log::replace_nonprintable_copy(args->GetString(1).ToWString(), L'?');

            BOOST_LOG_SEV(log::logger::get(), severity) << print() << L" [renderer_process] " << msg;
        }

        return false;
    }

    bool GetAudioParameters(CefRefPtr<CefBrowser> browser, CefAudioParameters& params) override
    {
        params.channel_layout    = CEF_CHANNEL_LAYOUT_7_1;
        params.sample_rate       = format_desc_.audio_sample_rate;
        params.frames_per_buffer = format_desc_.audio_cadence[0];
        return format_desc_.audio_cadence.size() == 1; // TODO - handle 59.94
    }

    void OnAudioStreamStarted(CefRefPtr<CefBrowser> browser, const CefAudioParameters& params, int channels) override
    {
        audioResampler_ = std::make_unique<ffmpeg::AudioResampler>(params.sample_rate, AV_SAMPLE_FMT_FLTP);
    }
    void OnAudioStreamPacket(CefRefPtr<CefBrowser> browser, const float** data, int samples, int64_t pts) override
    {
        if (!audioResampler_)
            return;

        auto audio       = audioResampler_->convert(samples, reinterpret_cast<const void**>(data));
        auto audio_frame = core::mutable_frame(this, {}, std::move(audio), core::pixel_format_desc());

        {
            std::lock_guard<std::mutex> lock(audio_frames_mutex_);
            while (audio_frames_.size() >= frames_max_size_) {
                audio_frames_.pop();
            }
            audio_frames_.push(presentation_frame(core::draw_frame(std::move(audio_frame))));
        }
    }
    void OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) override { audioResampler_ = nullptr; }
    void OnAudioStreamError(CefRefPtr<CefBrowser> browser, const CefString& message) override
    {
        CASPAR_LOG(info) << "[html_producer] OnAudioStreamError: \"" << message.ToString() << "\"";
        audioResampler_ = nullptr;
    }

    void do_execute_javascript(const std::wstring& javascript)
    {
        html::begin_invoke([this, javascript] {
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

    // ── Virtual time / BeginFrame helpers ──────────────────────────────────

    // Cumulative virtual time (µs) at the end of frame N. Computed from the
    // rational frame duration (`duration / time_scale`) rather than a per-frame
    // float increment so non-integer rates (NTSC) don't drift.
    uint64_t virtual_time_us_at(uint64_t frame_n) const
    {
        const uint64_t num = static_cast<uint64_t>(format_desc_.duration) * 1'000'000ULL;
        const uint64_t den = static_cast<uint64_t>(format_desc_.time_scale);
        return den > 0 ? (frame_n * num) / den : 0;
    }

    // Advance virtual time by one frame budget; the compositing BeginFrame is
    // issued from OnDevToolsEvent when virtualTimeBudgetExpired fires (so the clock
    // has reached and paused at this frame's timestamp before we paint). Blocks
    // until both that event and the resulting OnPaint are observed.
    // Runs on the channel thread; CEF calls are dispatched to TID_UI.
    bool tick(std::chrono::steady_clock::time_point deadline)
    {
        const uint64_t prev_us   = virtual_time_us_at(frame_count_);
        const uint64_t next_us   = virtual_time_us_at(frame_count_ + 1);
        const uint64_t budget_us = next_us - prev_us;

        const double budget_ms = static_cast<double>(budget_us) / 1000.0;

        ++frame_count_;

        {
            std::lock_guard<std::mutex> lock(tick_mutex_);
            tick_budget_expired_ = false;
            tick_paint_received_ = false;
            force_accept_        = false;
        }

        // Per-tick marker color, well-spread so consecutive ticks differ greatly in
        // every channel (a stale previous-tick marker can never fall inside tolerance).
        const uint8_t mr = static_cast<uint8_t>((frame_count_ * 97 + 13) & 0xFF);
        const uint8_t mg = static_cast<uint8_t>((frame_count_ * 149 + 71) & 0xFF);
        const uint8_t mb = static_cast<uint8_t>((frame_count_ * 211 + 167) & 0xFF);

        html::begin_invoke([this, budget_ms, mr, mg, mb] {
            if (closing_ || browser_ == nullptr)
                return;

            // Stamp this tick's corner marker and reset the per-tick retry count.
            // OnPaint reads the marker back to confirm the frame is the committed,
            // fully-composited one before accepting it (CEF #4166). The marker change
            // and this frame's animation work composite together on the BeginFrame
            // issued after virtualTimeBudgetExpired.
            expected_marker_[0] = mr;
            expected_marker_[1] = mg;
            expected_marker_[2] = mb;
            sync_retries_       = 0;
            paints_this_tick_   = 0;
            {
                // Set the marker via Runtime.evaluate on the SAME DevTools pipe as the
                // virtual-time advance below, so it is processed first and its DOM
                // mutation is committed by the budget's BeginMainFrame together with this
                // frame's content. (CefFrame::ExecuteJavaScript uses a separate IPC path
                // that races virtual time and lands the marker a frame or two late — which
                // made every tick read a stale marker.) The expression is self-contained,
                // so it also (re)creates the marker element if a template wiped the DOM.
                const std::string rgb =
                    "rgb(" + std::to_string(mr) + "," + std::to_string(mg) + "," + std::to_string(mb) + ")";
                const std::string expr = "(function(c){var m=document.getElementById('__caspar_mark');"
                                         "if(!m){m=document.createElement('div');m.id='__caspar_mark';"
                                         "m.style.cssText='position:fixed;left:0;top:0;width:6px;height:6px;"
                                         "margin:0;padding:0;border:0;z-index:2147483647;pointer-events:none';"
                                         "document.documentElement.appendChild(m);}m.style.background=c;})('" +
                                         rgb + "')";
                auto ev = CefDictionaryValue::Create();
                ev->SetString("expression", expr);
                browser_->GetHost()->ExecuteDevToolsMethod(0, "Runtime.evaluate", ev);
            }

            // Advance the page's virtual clock by exactly one frame duration.
            // Use pauseIfNetworkFetchesPending so an in-flight fetch defers the
            // tick rather than letting JS observe an inconsistent clock jump.
            // When the budget is consumed, Emulation.virtualTimeBudgetExpired fires
            // (handled in OnDevToolsEvent) and we composite the frame from there.
            auto vt = CefDictionaryValue::Create();
            vt->SetString("policy", "pauseIfNetworkFetchesPending");
            vt->SetDouble("budget", budget_ms);
            browser_->GetHost()->ExecuteDevToolsMethod(0, "Emulation.setVirtualTimePolicy", vt);
        });

        // Wait for the budget to expire: virtual time has advanced one frame and paused,
        // with this tick's content + marker committed.
        {
            std::unique_lock<std::mutex> lock(tick_mutex_);
            if (!tick_cv_.wait_until(lock, deadline, [&] { return tick_budget_expired_ || closing_ || not_found_; }))
                return false;
            if (closing_ || not_found_)
                return false;
        }

        // Pump begin-frames, paced with breathing room, until OnPaint delivers a frame
        // carrying this tick's marker. Back-to-back begin-frames just re-deliver the same
        // stale buffer; the renderer needs a few ms to composite the freshly-committed frame
        // (CEF #4166 — the matching paint arrived ~6ms after the budget). Pacing is wall-clock
        // only — virtual time stays paused — so the captured frame is deterministic; only the
        // number of discarded paints varies run to run.
        auto sync_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(SYNC_TIMEOUT_MS);
        if (sync_deadline > deadline)
            sync_deadline = deadline;

        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::unique_lock<std::mutex> lock(tick_mutex_);
                if (tick_paint_received_)
                    return !closing_ && !not_found_;
                if (closing_ || not_found_)
                    return false;
            }

            if (std::chrono::steady_clock::now() >= sync_deadline && !force_accept_) {
                std::lock_guard<std::mutex> lock(tick_mutex_);
                force_accept_ = true;
            }

            html::begin_invoke([this] {
                if (closing_ || browser_ == nullptr)
                    return;
                ++sync_retries_;
                browser_->GetHost()->Invalidate(PET_VIEW);
                browser_->GetHost()->SendExternalBeginFrame();
            });

            std::unique_lock<std::mutex> lock(tick_mutex_);
            tick_cv_.wait_for(lock, std::chrono::milliseconds(SYNC_PUMP_MS), [&] {
                return tick_paint_received_ || closing_ || not_found_;
            });
        }

        CASPAR_LOG(warning) << print() << L" [vtc] tick " << frame_count_ << L" gave up at deadline (begin-frames="
                            << sync_retries_.load() << L", paints=" << paints_this_tick_.load() << L")";
        return false;
    }

    void notify_tick_paint()
    {
        if (!vtc_enabled_)
            return;

        {
            std::lock_guard<std::mutex> lock(tick_mutex_);
            tick_paint_received_ = true;
        }
        tick_cv_.notify_all();
    }

    // ── CefDevToolsMessageObserver ─────────────────────────────────────────

    void OnDevToolsEvent(CefRefPtr<CefBrowser> browser,
                         const CefString&      method,
                         const void*           message,
                         size_t                message_size) override
    {
        CASPAR_ASSERT(CefCurrentlyOn(TID_UI));

        if (method == "Page.lifecycleEvent") {
            // Payload: {"frameId":"...","loaderId":"...","name":"firstPaint","timestamp":...}
            const std::string payload(static_cast<const char*>(message), message_size);
            if (payload.find("\"firstPaint\"") != std::string::npos) {
                CASPAR_LOG(info) << print() << L" CDP: firstPaint";
                first_paint_seen_ = true;
                maybe_arm_ready();
            }
        } else if (method == "Emulation.virtualTimeBudgetExpired") {
            // Virtual time reached this frame's timestamp and is paused; this tick's content
            // and marker are committed. tick() now pumps begin-frames (paced) until the
            // marker-matching paint arrives. We deliberately do NOT composite from here: a
            // begin-frame issued here could outlive the tick and satisfy the *next* tick with
            // a stale frame, which dropped/duplicated frames and made playback choppy.
            {
                std::lock_guard<std::mutex> lock(tick_mutex_);
                tick_budget_expired_ = true;
            }

            tick_cv_.notify_all();
        }
    }

    void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser,
                                int                   message_id,
                                bool                  success,
                                const void*           message,
                                size_t                message_size) override
    {
        if (!success) {
            const std::string payload(static_cast<const char*>(message), message_size);
            CASPAR_LOG(warning) << print() << L" CDP method failed: " << payload;
        }
    }

    IMPLEMENT_REFCOUNTING(html_client);
};

class html_producer : public core::frame_producer
{
    core::video_format_desc             format_desc_;
    const std::wstring                  url_;
    spl::shared_ptr<diagnostics::graph> graph_;
    bool                                vtc_enabled_ = false;

    CefRefPtr<html_client> client_;

  public:
    html_producer(const spl::shared_ptr<core::frame_factory>& frame_factory,
                  const core::video_format_desc&              format_desc,
                  const std::wstring&                         url)
        : format_desc_(format_desc)
        , url_(url)
    {
        html::invoke([&] {
            auto       gpu         = is_gpu_shared_texture_enabled();
            const bool vtc         = env::properties().get(L"configuration.html.enable-virtual-time", false);
            const bool wait_for_fp = env::properties().get(L"configuration.html.wait-for-fp", false) || vtc;

            vtc_enabled_ = vtc;

            client_ =
                new html_client(frame_factory, graph_, format_desc, gpu.first, gpu.second, vtc, wait_for_fp, url_);

            CefWindowInfo window_info;
            window_info.bounds.width                 = format_desc.square_width;
            window_info.bounds.height                = format_desc.square_height;
            window_info.windowless_rendering_enabled = true;
            window_info.shared_texture_enabled       = gpu.second;
            window_info.external_begin_frame_enabled = vtc;

            CefBrowserSettings browser_settings;
            browser_settings.webgl = gpu.first ? cef_state_t::STATE_ENABLED : cef_state_t::STATE_DISABLED;
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

    bool supports_deterministic_sync() const override { return vtc_enabled_; }

    bool wait_for_frame(const core::video_field field, std::chrono::milliseconds timeout) override
    {
        if (client_ != nullptr) {
            return client_->wait_for_frame(timeout);
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
