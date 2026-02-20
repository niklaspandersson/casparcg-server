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

#include "env_macos.h"

#include "../../log.h"
#include "../../os/filesystem.h"

#include <boost/filesystem.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>

#include <cstdlib>

namespace caspar { namespace env { namespace macos {

std::wstring get_user_home_directory()
{
    const char* home = std::getenv("HOME");
    if (home) {
        return u16(std::string(home));
    }
    return L"";
}

std::wstring get_user_config_directory()
{
    // XDG_CONFIG_HOME or default to ~/.config/CasparCG
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && xdg_config[0] != '\0') {
        return u16(std::string(xdg_config)) + L"/CasparCG";
    }
    auto home = get_user_home_directory();
    if (!home.empty()) {
        return home + L"/.config/CasparCG";
    }
    return L"";
}

std::wstring get_user_data_directory()
{
    // XDG_DATA_HOME or default to ~/.local/share/CasparCG
    const char* xdg_data = std::getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] != '\0') {
        return u16(std::string(xdg_data)) + L"/CasparCG";
    }
    auto home = get_user_home_directory();
    if (!home.empty()) {
        return home + L"/.local/share/CasparCG";
    }
    return L"";
}

std::wstring get_user_state_directory()
{
    // XDG_STATE_HOME or default to ~/.local/state/CasparCG (for logs)
    const char* xdg_state = std::getenv("XDG_STATE_HOME");
    if (xdg_state && xdg_state[0] != '\0') {
        return u16(std::string(xdg_state)) + L"/CasparCG";
    }
    auto home = get_user_home_directory();
    if (!home.empty()) {
        return home + L"/.local/state/CasparCG";
    }
    return L"";
}

std::wstring get_user_cache_directory()
{
    // XDG_CACHE_HOME or default to ~/.cache/CasparCG
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    if (xdg_cache && xdg_cache[0] != '\0') {
        return u16(std::string(xdg_cache)) + L"/CasparCG";
    }
    auto home = get_user_home_directory();
    if (!home.empty()) {
        return home + L"/.cache/CasparCG";
    }
    return L"";
}

std::wstring get_bundle_resources_path()
{
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (!mainBundle) {
        return L"";
    }

    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
    if (!resourcesURL) {
        return L"";
    }

    char path[PATH_MAX];
    if (!CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8*)path, PATH_MAX)) {
        CFRelease(resourcesURL);
        return L"";
    }

    CFRelease(resourcesURL);
    return u16(std::string(path));
}

bool is_running_from_app_bundle()
{
    // Check if we're running from within a .app bundle by checking if
    // the executable path contains ".app/Contents/MacOS"
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::string exe_path(path);
        return exe_path.find(".app/Contents/MacOS") != std::string::npos;
    }
    return false;
}

std::wstring find_config_file(const std::wstring& filename, const std::wstring& initial_path)
{
    // 1. Check current working directory (for command-line specified or local config)
    if (boost::filesystem::exists(filename)) {
        return filename;
    }

    // 2. Check initial directory (where executable is)
    std::wstring fullpath = initial_path + L"/" + filename;
    if (boost::filesystem::exists(fullpath)) {
        return fullpath;
    }

    // 3. Check XDG config directory (~/.config/CasparCG/)
    auto config_dir = get_user_config_directory();
    if (!config_dir.empty()) {
        fullpath = config_dir + L"/" + filename;
        if (boost::filesystem::exists(fullpath)) {
            CASPAR_LOG(info) << L"Using config from XDG config directory: " << fullpath;
            return fullpath;
        }
    }

    // 4. Check app bundle Resources (if running from .app bundle)
    if (is_running_from_app_bundle()) {
        auto resources = get_bundle_resources_path();
        if (!resources.empty()) {
            fullpath = resources + L"/" + filename;
            if (boost::filesystem::exists(fullpath)) {
                CASPAR_LOG(info) << L"Using config from app bundle Resources: " << fullpath;
                return fullpath;
            }
        }
    }

    // Not found
    return L"";
}

}}} // namespace caspar::env::macos
