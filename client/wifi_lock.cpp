/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wifi_lock.h"

#ifdef __ANDROID__
#include "application.h"
#include "spdlog/spdlog.h"
#include <android/native_activity.h>
#include <sys/system_properties.h>

#include "android/jnipp.h"

wifi_lock::wifi_lock(decltype(multicast_) m, decltype(wifi_) w) :
        multicast_(std::move(m)), wifi_(std::move(w)) {}

#ifndef __ANDROID_LIB__
std::shared_ptr<wifi_lock> wifi_lock::make_wifi_lock(jobject activity)
{
	jni::object<""> act(application::native_app()->activity->clazz);

	jni::string lock_name("WiVRn");

	static int api_level = jni::klass("android/os/Build$VERSION").field<jni::Int>("SDK_INT");

	auto app = act.call<jni::object<"android/app/Application">>("getApplication");
	auto ctx = app.call<jni::object<"android/content/Context">>("getApplicationContext");
	auto wifi_service_id = ctx.klass().field<jni::string>("WIFI_SERVICE");
	auto system_service = ctx.call<jni::object<"java/lang/Object">>("getSystemService", wifi_service_id);
	return std::shared_ptr<wifi_lock>(new wifi_lock(
	        system_service.call<jni::object<"android/net/wifi/WifiManager$MulticastLock">>("createMulticastLock", lock_name),
	        system_service.call<jni::object<"android/net/wifi/WifiManager$WifiLock">>(
	                "createWifiLock",
	                jni::Int(api_level >= 29 ? 4 /*WIFI_MODE_FULL_LOW_LATENCY*/ : 3 /*WIFI_MODE_FULL_HIGH_PERF*/),
	                lock_name)));
}
#endif

void wifi_lock::print_wifi()
{
#ifndef __ANDROID_LIB__
	if (wifi_.call<jni::Bool>("isHeld"))
		spdlog::info("WifiLock low latency acquired");
	else
		spdlog::info("WifiLock low latency released");
#endif
}

void wifi_lock::print_multicast()
{
#ifndef __ANDROID_LIB__
	if (multicast_.call<jni::Bool>("isHeld"))
		spdlog::info("MulticastLock acquired");
	else
		spdlog::info("MulticastLock released");
#endif
}

std::shared_ptr<void> wifi_lock::get_wifi_lock()
{
	std::lock_guard lock(mutex);
	if (auto l = wifi_weak.lock())
		return l;

	wifi_.call<void>("acquire");
	print_wifi();

	// The shared pointer has a separate reference counter: destructor only calls a method
	auto res = std::shared_ptr<void>(
	        this,
	        [self = shared_from_this()](void *) {
	std::lock_guard lock(self->mutex);
	self->wifi_.call<void>("release");
	self->print_wifi(); });
	wifi_weak = res;
	return res;
}
std::shared_ptr<void> wifi_lock::get_multicast_lock()
{
	std::lock_guard lock(mutex);
	if (auto l = multicast_weak.lock())
		return l;

	multicast_.call<void>("acquire");
	print_multicast();

	// The shared pointer has a separate reference counter: destructor only calls a method
	auto res = std::shared_ptr<void>(
	        this,
	        [self = shared_from_this()](void *) {
	std::lock_guard lock(self->mutex);
	self->multicast_.call<void>("release");
	self->print_multicast(); });
	multicast_weak = res;
	return res;
}

#else

std::shared_ptr<void> wifi_lock::get_wifi_lock()
{
	return nullptr;
}
std::shared_ptr<void> wifi_lock::get_multicast_lock()
{
	return nullptr;
}

#endif
