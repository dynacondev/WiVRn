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

#include "flatpak.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

static const std::filesystem::path info_path = "/.flatpak-info";
static const bool is_flatpak_v = std::filesystem::exists(info_path);

bool wivrn::is_flatpak()
{
	return is_flatpak_v;
}

std::optional<std::string> wivrn::flatpak_key(std::string section, std::string key)
{
	section = "[" + section + "]";
	key += "=";
	std::string line;
	std::ifstream info(info_path);
	bool in_section = false;
	while (std::getline(info, line))
	{
		if (line.starts_with("["))
			in_section = section == line;

		if (in_section and line.starts_with(key))
			return line.substr(key.size());
	}
	return {};
}

std::optional<std::vector<std::byte>> wivrn::read_file_via_host(const std::string & path, size_t max_size)
{
	if (path.empty() or max_size == 0)
		return std::nullopt;

	if (not wivrn::is_flatpak())
		return std::nullopt;

	if (wivrn::flatpak_key(wivrn::flatpak::section::session_bus_policy, "org.freedesktop.Flatpak") != "talk")
		return std::nullopt;

	int pipefd[2] = {-1, -1};
	if (pipe(pipefd) != 0)
		return std::nullopt;

	posix_spawn_file_actions_t actions;
	if (posix_spawn_file_actions_init(&actions) != 0)
	{
		close(pipefd[0]);
		close(pipefd[1]);
		return std::nullopt;
	}
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, pipefd[0]);
	posix_spawn_file_actions_addclose(&actions, pipefd[1]);

	// posix_spawnp takes char *const[]; the "--" keeps paths starting
	// with '-' (or containing spaces) from being parsed as options.
	char arg0[] = "flatpak-spawn";
	char arg1[] = "--host";
	char arg2[] = "cat";
	char arg3[] = "--";
	char * const argv[] = {arg0, arg1, arg2, arg3, const_cast<char *>(path.c_str()), nullptr};

	extern char ** environ;
	pid_t pid = -1;
	int spawn_rc = posix_spawnp(&pid, "flatpak-spawn", &actions, nullptr, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pipefd[1]);
	pipefd[1] = -1;

	if (spawn_rc != 0)
	{
		close(pipefd[0]);
		return std::nullopt;
	}

	std::vector<std::byte> data;
	data.reserve(1 << 20);
	char buf[65536];
	bool oversize = false;
	bool read_error = false;
	size_t total = 0;
	for (;;)
	{
		ssize_t n = read(pipefd[0], buf, sizeof(buf));
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			read_error = true;
			break;
		}
		if (n == 0)
			break;
		if (total + (size_t)n > max_size)
		{
			oversize = true;
			break;
		}
		size_t prev = data.size();
		data.resize(prev + (size_t)n);
		memcpy(data.data() + prev, buf, (size_t)n);
		total += (size_t)n;
	}
	close(pipefd[0]);

	// Reap the child in all cases. On oversize the child is likely
	// blocked writing / killed by SIGPIPE once we closed the pipe.
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 and errno == EINTR)
	{
	}

	if (oversize or read_error)
		return std::nullopt;
	if (not WIFEXITED(status) or WEXITSTATUS(status) != 0)
		return std::nullopt;
	if (data.empty())
		return std::nullopt;

	return data;
}
