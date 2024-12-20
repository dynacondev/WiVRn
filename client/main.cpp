/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "application.h"
#include "scenes/lobby.h"
#include "spdlog/spdlog.h"

#include "utils/named_thread.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __ANDROID__
#include "lib.h"
#include "spdlog/sinks/android_sink.h"
#include <android/native_window.h>
#include <android_native_app_glue.h>
#else
#include "spdlog/sinks/stdout_color_sinks.h"
#endif

#ifdef __ANDROID__
void real_main(android_app * native_app)
#else
void real_main()
#endif
{
#ifndef __ANDROID_LIB__
	try
	{
#ifdef __ANDROID__
		application_info info;
		info.native_app = native_app;
		info.name = "WiVRn";
		info.version = VK_MAKE_VERSION(1, 0, 0);
#endif

		application app(info);
		app.push_scene<scenes::lobby>();
	}
	catch (std::exception & e)
	{
		spdlog::error("Caught exception: \"{}\"", e.what());
	}
	catch (...)
	{
		spdlog::error("Caught unknown exception");
	}

#ifdef __ANDROID__
	ANativeActivity_finish(native_app->activity);

	// Read all pending events.
	while (!native_app->destroyRequested)
	{
		int events;
		struct android_poll_source * source;

		while (ALooper_pollOnce(100, nullptr, &events, (void **)&source) >= 0)
		{
			// Process this event.
			if (source != nullptr)
				source->process(native_app, source);
		}
	}
	exit(0);
#endif
#endif
}

#ifdef __ANDROID__
void android_main(android_app * native_app) __attribute__((visibility("default")));
void android_main(android_app * native_app)
{
	static auto logger = spdlog::android_logger_mt("WiVRn", "WiVRn");

	spdlog::set_default_logger(logger);

#ifdef __ANDROID_LIB__
	// Never gets run from here - this only runs if android system starts this built as an app
#else
	real_main(native_app);
#endif
}
#else
int main(int argc, char * argv[])
{
	spdlog::set_default_logger(spdlog::stdout_color_mt("WiVRn"));

	char * loglevel = getenv("WIVRN_LOGLEVEL");
	if (loglevel)
	{
		if (!strcasecmp(loglevel, "trace"))
			spdlog::set_level(spdlog::level::trace);
		else if (!strcasecmp(loglevel, "debug"))
			spdlog::set_level(spdlog::level::debug);
		else if (!strcasecmp(loglevel, "info"))
			spdlog::set_level(spdlog::level::info);
		else if (!strcasecmp(loglevel, "warning"))
			spdlog::set_level(spdlog::level::warn);
		else if (!strcasecmp(loglevel, "error"))
			spdlog::set_level(spdlog::level::err);
		else if (!strcasecmp(loglevel, "critical"))
			spdlog::set_level(spdlog::level::critical);
		else if (!strcasecmp(loglevel, "off"))
			spdlog::set_level(spdlog::level::off);
		else
			spdlog::warn("Invalid value for WIVRN_LOGLEVEL environment variable");
	}

	real_main();
}
#endif
