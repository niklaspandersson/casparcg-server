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
 * Author: Robert Nagy, ronag89@gmail.com
 */
#include "output.h"

#include "channel_info.h"
#include "frame_consumer.h"

#include "../frame/frame.h"
#include "../frame/pixel_format.h"

#include <common/bit_depth.h>
#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/memory.h>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <thread>
#include <utility>

namespace caspar { namespace core {

using time_point_t = decltype(std::chrono::high_resolution_clock::now());

struct output::impl
{
    monitor::state                      state_;
    spl::shared_ptr<diagnostics::graph> graph_;
    const channel_info                  channel_info_;
    video_format_desc                   format_desc_;

    std::mutex                                     consumers_mutex_;
    std::map<int, spl::shared_ptr<frame_consumer>> consumers_;

    std::function<void()>             on_consumers_changed_;
    std::function<void(int port_idx)> on_consumer_error_;

    std::optional<time_point_t> time_;

  public:
    impl(const spl::shared_ptr<diagnostics::graph>& graph,
         const video_format_desc&                   format_desc,
         const core::channel_info&                  channel_info)
        : graph_(graph)
        , channel_info_(channel_info)
        , format_desc_(format_desc)
    {
    }

    void add(int index, spl::shared_ptr<frame_consumer> consumer)
    {
        if (channel_info_.deterministic && !consumer->supports_deterministic_sync()) {
            CASPAR_THROW_EXCEPTION(user_error()
                                   << msg_info(L"Cannot attach consumer that does not support deterministic sync to a "
                                               L"deterministic channel: " +
                                               consumer->print()));
        }

        remove(index);

        consumer->initialize(format_desc_, channel_info_, index);

        {
            std::lock_guard<std::mutex> lock(consumers_mutex_);
            consumers_.emplace(index, std::move(consumer));
        }

        if (on_consumers_changed_)
            on_consumers_changed_();
    }

    void add(const spl::shared_ptr<frame_consumer>& consumer) { add(consumer->index(), consumer); }

    bool remove(int index)
    {
        bool changed;
        {
            std::lock_guard<std::mutex> lock(consumers_mutex_);
            changed = consumers_.erase(index) > 0;
        }
        if (changed && on_consumers_changed_)
            on_consumers_changed_();
        return changed;
    }

    bool remove(const spl::shared_ptr<frame_consumer>& consumer) { return remove(consumer->index()); }

    std::future<bool> call(int index, const std::vector<std::wstring>& params)
    {
        std::lock_guard<std::mutex> lock(consumers_mutex_);
        auto                        it = consumers_.find(index);
        if (it != consumers_.end()) {
            try {
                return it->second->call(params);
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        } else {
            CASPAR_LOG(warning) << print() << L" No consumer found for index " << index << L".";
        }
        return caspar::make_ready_future(false);
    }

    size_t consumer_count()
    {
        std::lock_guard<std::mutex> lock(consumers_mutex_);
        return consumers_.size();
    }

    void set_on_consumers_changed(std::function<void()> callback) { on_consumers_changed_ = std::move(callback); }
    void set_on_consumer_error(std::function<void(int)> callback) { on_consumer_error_ = std::move(callback); }

    void operator()(const const_frame&             input_frame1,
                    const const_frame&             input_frame2,
                    const core::video_format_desc& format_desc)
    {
        auto time = std::move(time_);

        if (format_desc_ != format_desc) {
            std::lock_guard<std::mutex> lock(consumers_mutex_);
            for (auto it = consumers_.begin(); it != consumers_.end();) {
                try {
                    it->second->initialize(format_desc, channel_info_, it->first);
                    ++it;
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                    it = consumers_.erase(it);
                }
            }
            format_desc_ = format_desc;
            time_        = {};
            return;
        }

        // If no frame is provided, this should only happen when the channel has no consumers.
        // Take a shortcut and perform the sleep to let the channel tick correctly.
        if (!input_frame1) {
            if (channel_info_.deterministic) {
                // In deterministic mode we never pace by wall-clock; the channel just spins
                // through frames as fast as the producers/consumers allow.
                time_.reset();
                return;
            }
            if (!time) {
                time = std::chrono::high_resolution_clock::now();
            } else {
                std::this_thread::sleep_until(*time);
            }
            time_ = *time + std::chrono::microseconds(static_cast<int>(1e6 / format_desc_.hz));
            return;
        }

        const auto bytesPerComponent1 =
            input_frame1.pixel_format_desc().planes.at(0).depth == common::bit_depth::bit8 ? 1 : 2;
        if (input_frame1.size() != format_desc_.size * bytesPerComponent1) {
            CASPAR_LOG(warning) << print() << L" Invalid input frame size.";
            return;
        }

        if (input_frame2) {
            const auto bytesPerComponent2 =
                input_frame2.pixel_format_desc().planes.at(0).depth == common::bit_depth::bit8 ? 1 : 2;

            if (input_frame2.size() != format_desc_.size * bytesPerComponent2) {
                CASPAR_LOG(warning) << print() << L" Invalid input frame size.";
                return;
            }
        }

        decltype(consumers_) consumers;
        {
            std::lock_guard<std::mutex> lock(consumers_mutex_);
            consumers = consumers_;
        }

        auto drop_consumer = [this, &consumers](int index) {
            consumers.erase(index);
            {
                std::lock_guard<std::mutex> lock(consumers_mutex_);
                consumers_.erase(index);
            }
            if (on_consumer_error_)
                on_consumer_error_(index);
            if (on_consumers_changed_)
                on_consumers_changed_();
        };

        auto do_send = [&](core::video_field field, const core::const_frame& frame) {
            std::map<int, std::future<bool>> futures;

            for (auto it = consumers.begin(); it != consumers.end();) {
                try {
                    futures.emplace(it->first, it->second->send(field, frame));
                    ++it;
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                    auto index = it->first;
                    it         = consumers.erase(it);
                    {
                        std::lock_guard<std::mutex> lock(consumers_mutex_);
                        consumers_.erase(index);
                    }
                    if (on_consumer_error_)
                        on_consumer_error_(index);
                    if (on_consumers_changed_)
                        on_consumers_changed_();
                }
            }

            for (auto& p : futures) {
                try {
                    if (!p.second.get()) {
                        drop_consumer(p.first);
                    }
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                    drop_consumer(p.first);
                }
            }
        };

        if (format_desc_.field_count == 2) {
            do_send(core::video_field::a, input_frame1);
            do_send(core::video_field::b, input_frame2);
        } else {
            do_send(core::video_field::progressive, input_frame1);
        }

        monitor::state state;
        for (auto& p : consumers) {
            state["port"][p.first]             = p.second->state();
            state["port"][p.first]["consumer"] = p.second->name();
        }
        state_ = std::move(state);

        const auto needs_sync = std::all_of(
            consumers.begin(), consumers.end(), [](auto& p) { return !p.second->has_synchronization_clock(); });

        if (needs_sync && !channel_info_.deterministic) {
            if (!time) {
                time = std::chrono::high_resolution_clock::now();
            } else {
                std::this_thread::sleep_until(*time);
            }
            time_ = *time + std::chrono::microseconds(static_cast<int>(1e6 / format_desc_.hz));
        } else {
            time_.reset();
        }
    }

    std::wstring print() const { return L"output[" + std::to_wstring(channel_info_.index) + L"]"; }
};

output::output(const spl::shared_ptr<diagnostics::graph>& graph,
               const video_format_desc&                   format_desc,
               const core::channel_info&                  channel_info)
    : impl_(new impl(graph, format_desc, channel_info))
{
}
output::~output() {}
void output::add(int index, const spl::shared_ptr<frame_consumer>& consumer) { impl_->add(index, consumer); }
void output::add(const spl::shared_ptr<frame_consumer>& consumer) { impl_->add(consumer); }
bool output::remove(int index) { return impl_->remove(index); }
bool output::remove(const spl::shared_ptr<frame_consumer>& consumer) { return impl_->remove(consumer); }
std::future<bool> output::call(int index, const std::vector<std::wstring>& params)
{
    return impl_->call(index, params);
}
size_t output::consumer_count() const { return impl_->consumer_count(); }
void   output::set_on_consumers_changed(std::function<void()> callback)
{
    impl_->set_on_consumers_changed(std::move(callback));
}
void output::set_on_consumer_error(std::function<void(int)> callback)
{
    impl_->set_on_consumer_error(std::move(callback));
}
void   output::operator()(const const_frame& frame, const const_frame& frame2, const video_format_desc& format_desc)
{
    return (*impl_)(frame, frame2, format_desc);
}
core::monitor::state output::state() const { return impl_->state_; }
}} // namespace caspar::core
