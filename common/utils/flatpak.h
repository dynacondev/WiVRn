/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wivrn
{

namespace flatpak::section
{
inline const char * instance = "Instance";
inline const char * session_bus_policy = "Session Bus Policy";
}; // namespace flatpak::section

bool is_flatpak();
std::optional<std::string> flatpak_key(std::string section, std::string key);

// Reads a whole host-side file from inside a flatpak sandbox via
// `flatpak-spawn --host cat`. Returns nullopt when not running as a
// flatpak, without the Flatpak D-Bus talk permission, or when the file
// is missing/unreadable/empty/larger than max_size. Argv-based spawn
// (no shell), output capped at max_size.
std::optional<std::vector<std::byte>> read_file_via_host(const std::string & path, size_t max_size);
} // namespace wivrn
