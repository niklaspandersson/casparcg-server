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
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.erase(id);
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

}}} // namespace caspar::protocol::amcp
