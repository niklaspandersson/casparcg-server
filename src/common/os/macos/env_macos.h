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

#include <string>

namespace caspar { namespace env { namespace macos {

// XDG Base Directory Specification helpers
// Provides cross-platform consistency with Linux

std::wstring get_user_home_directory();
std::wstring get_user_config_directory();   // XDG_CONFIG_HOME or ~/.config/CasparCG
std::wstring get_user_data_directory();     // XDG_DATA_HOME or ~/.local/share/CasparCG
std::wstring get_user_state_directory();    // XDG_STATE_HOME or ~/.local/state/CasparCG
std::wstring get_user_cache_directory();    // XDG_CACHE_HOME or ~/.cache/CasparCG

// macOS app bundle helpers
std::wstring get_bundle_resources_path();
bool         is_running_from_app_bundle();

// Config file search with macOS-specific locations
std::wstring find_config_file(const std::wstring& filename, const std::wstring& initial_path);

}}} // namespace caspar::env::macos
