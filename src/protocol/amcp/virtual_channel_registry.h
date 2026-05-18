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

#pragma once

#include "amcp_shared.h"

#include <common/bit_depth.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <functional>
#include <map>
#include <mutex>

namespace caspar { namespace protocol { namespace amcp {

class virtual_channel_registry
{
  public:
    using channel_factory = std::function<
        channel_context(int virtual_id, const core::video_format_desc&, core::color_space, common::bit_depth)>;

    explicit virtual_channel_registry(channel_factory factory);

    int  create(const core::video_format_desc& format, core::color_space cs, common::bit_depth depth);
    void destroy(int id);

    // Returns the channel_context for `id`, or throws user_error if not found.
    channel_context get(int id) const;

    // Returns the channel_context for `id`, or an empty channel_context if not found.
    channel_context try_get(int id) const;

  private:
    mutable std::mutex                 mutex_;
    std::map<int, channel_context>     channels_;
    int                                next_id_ = 1;
    channel_factory                    factory_;
};

}}} // namespace caspar::protocol::amcp
