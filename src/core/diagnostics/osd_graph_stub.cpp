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

// Stub implementation of osd_graph for non-OpenGL builds (macOS with Vulkan)

#include "../StdAfx.h"

#include "osd_graph.h"

#include <common/diagnostics/graph.h>

namespace caspar { namespace core { namespace diagnostics { namespace osd {

void register_sink()
{
}

void show_graphs(bool)
{
}

void shutdown()
{
}

}}}} // namespace caspar::core::diagnostics::osd
