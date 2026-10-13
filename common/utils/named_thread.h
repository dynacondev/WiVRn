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

#include <pthread.h>
#include <thread>
#include <utility>

namespace utils
{
template <typename... Args>
std::thread named_thread(const std::string & name, Args &&... args)
{
	std::thread t{std::forward<Args>(args)...};
#ifdef __APPLE__
	// macOS pthread_setname_np() only names the calling thread (no thread
	// argument, 64-char limit). Naming the new thread from here is not
	// possible, so this is intentionally a no-op on Apple; threads that
	// need a name must call pthread_setname_np() themselves.
	(void)name;
#else
	pthread_setname_np(t.native_handle(), name.substr(0, 15).c_str());
#endif
	return t;
}
} // namespace utils
