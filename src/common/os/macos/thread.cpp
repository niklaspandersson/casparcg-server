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

#include "../thread.h"
#include "../../utf.h"

#include <pthread.h>
#include <sched.h>

namespace caspar {

void set_thread_name(const std::wstring& name)
{
    // macOS pthread_setname_np only takes a single argument (the name)
    // and sets the name of the calling thread
    pthread_setname_np(u8(name).c_str());
}

void set_thread_realtime_priority()
{
    pthread_t          handle = pthread_self();
    int                policy;
    struct sched_param param;

    if (pthread_getschedparam(handle, &policy, &param) != 0)
        return;

    param.sched_priority = 2;
    pthread_setschedparam(handle, SCHED_FIFO, &param);
}

} // namespace caspar
