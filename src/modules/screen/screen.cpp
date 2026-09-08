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

#include "screen.h"

#ifdef __APPLE__
#include "consumer/screen_consumer_vk.h"
#else
#include "consumer/screen_consumer.h"
#endif

#include <core/consumer/frame_consumer.h>

namespace caspar { namespace screen {

void init(const core::module_dependencies& dependencies)
{
#ifdef __APPLE__
    dependencies.consumer_registry->register_consumer_factory(L"Screen Consumer", create_consumer_vk);
    dependencies.consumer_registry->register_preconfigured_consumer_factory(L"screen", create_preconfigured_consumer_vk);
#else
    dependencies.consumer_registry->register_consumer_factory(L"Screen Consumer", create_consumer);
    dependencies.consumer_registry->register_preconfigured_consumer_factory(L"screen", create_preconfigured_consumer);
#endif
}

}} // namespace caspar::screen
