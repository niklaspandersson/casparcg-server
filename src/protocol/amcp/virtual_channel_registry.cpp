/*
 * Copyright (c) 2026 Sveriges Television AB <info@casparcg.com>
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
 */

#include "../StdAfx.h"

#include "virtual_channel_registry.h"

#include <common/except.h>

namespace caspar { namespace protocol { namespace amcp {

virtual_channel_registry::virtual_channel_registry(channel_factory factory)
    : factory_(std::move(factory))
{
}

int virtual_channel_registry::create(const core::video_format_desc& format,
                                     core::color_space              cs,
                                     common::bit_depth              depth)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int                         id = next_id_++;
    channels_.emplace(id, factory_(id, format, cs, depth));
    return id;
}

void virtual_channel_registry::destroy(int id)
{
    // Unlink the node under the lock, then let it drop after unlocking. ~video_channel joins
    // the channel's tick thread, which can be blocked for the deterministic stall timeout
    // inside wait_for_frame; doing that join while holding mutex_ would stall every other
    // registry operation (create/get/destroy). extract() unlinks without touching the
    // (const-membered, non-assignable) channel_context, and the node handle destroys it here.
    std::map<int, channel_context>::node_type node;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = channels_.find(id);
        if (it == channels_.end())
            return;
        node = channels_.extract(it);
    }
}

channel_context virtual_channel_registry::get(int id) const
{
    auto ctx = try_get(id);
    if (!ctx.raw_channel) {
        CASPAR_THROW_EXCEPTION(user_error()
                               << msg_info(L"Virtual channel $" + std::to_wstring(id) + L" not found"));
    }
    return ctx;
}

channel_context virtual_channel_registry::try_get(int id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        it = channels_.find(id);
    if (it == channels_.end())
        return channel_context();
    return it->second;
}

std::vector<std::pair<int, std::shared_ptr<core::video_channel>>> virtual_channel_registry::list() const
{
    std::vector<std::pair<int, std::shared_ptr<core::video_channel>>> result;
    std::lock_guard<std::mutex>                                       lock(mutex_);
    result.reserve(channels_.size());
    for (const auto& [id, ctx] : channels_) // std::map iterates ascending by id
        result.emplace_back(id, ctx.raw_channel);
    return result;
}

}}} // namespace caspar::protocol::amcp
