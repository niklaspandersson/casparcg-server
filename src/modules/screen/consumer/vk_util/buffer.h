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

#pragma once

#include <boost/property_tree/ptree_fwd.hpp>
#include <memory>

namespace caspar { namespace accelerator { namespace vk {

/**
 * Vulkan buffer - GPU buffer for CPU-GPU data transfers
 *
 * Provides:
 * - Host-visible, coherent memory for fast CPU-GPU transfers
 * - Persistent mapped pointer for zero-copy access
 * - Read (download) and write (upload) modes
 * - Statistics tracking for pool monitoring
 */
class buffer final
{
  public:
    static boost::property_tree::wptree info();

    // device and physical_device are VkDevice and VkPhysicalDevice cast to void*
    buffer(void* device, void* physical_device, int size, bool write);
    buffer(const buffer&) = delete;
    buffer(buffer&& other);
    ~buffer();

    buffer& operator=(const buffer&) = delete;
    buffer& operator=(buffer&& other);

    void* handle() const;  // Returns VkBuffer
    void* data();          // Returns persistent mapped pointer
    int   size() const;
    bool  write() const;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}}} // namespace caspar::accelerator::vk
