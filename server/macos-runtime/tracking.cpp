// Synthetic tracking source: deterministic, time-based, no headset.
// HMD sits at (0, 1.6, 0) with a slow ±10° yaw oscillation (8 s period);
// controllers are static at ±0.2/1.3/-0.3 m. Right hand carries the
// "pressed" demo state so tests can assert both polarities.

#include "tracking.h"

#include "streamer.h"

#include <chrono>
#include <cmath>
#include <mutex>

namespace wivrn_macos
{
namespace
{

int64_t now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	               std::chrono::steady_clock::now().time_since_epoch())
	        .count();
}

XrQuaternionf yaw_quat(double yaw)
{
	return XrQuaternionf{
	        .x = 0,
	        .y = (float)std::sin(yaw / 2),
	        .z = 0,
	        .w = (float)std::cos(yaw / 2),
	};
}

class SyntheticSource : public TrackingSource
{
public:
	TrackingSample latest() override
	{
		int64_t now = now_ns();
		double t = now / 1e9;
		double yaw = (10.0 * M_PI / 180.0) * std::sin(2 * M_PI * t / 8.0);

		TrackingSample sample;
		sample.timestampNs = now;
		sample.hmd.orientation = yaw_quat(yaw);
		sample.hmd.position = {0, 1.6f, 0};

		sample.left.pose.orientation = {0, 0, 0, 1};
		sample.left.pose.position = {-0.2f, 1.3f, -0.3f};
		sample.left.active = true;
		sample.left.menuClick = true;

		sample.right.pose.orientation = {0, 0, 0, 1};
		sample.right.pose.position = {0.2f, 1.3f, -0.3f};
		sample.right.active = true;
		sample.right.selectClick = true;
		sample.right.squeezeClick = true;
		sample.right.triggerValue = 0.5f;
		sample.right.thumbstick = {0.25f, -0.5f};
		return sample;
	}
};

std::mutex & source_mutex()
{
	static std::mutex mutex;
	return mutex;
}

std::shared_ptr<TrackingSource> & active_source()
{
	static std::shared_ptr<TrackingSource> source = std::make_shared<SyntheticSource>();
	return source;
}

} // namespace

void set_tracking_source(std::shared_ptr<TrackingSource> source)
{
	std::lock_guard lock(source_mutex());
	if (source)
		active_source() = std::move(source);
}

TrackingSample current_sample()
{
	// Live headset feed wins when fresh; synthetic otherwise (keeps
	// headless tests + pre-connect apps fully working).
	if (auto headset = streamer_headset_sample())
		return *headset;
	std::lock_guard lock(source_mutex());
	return active_source()->latest();
}

} // namespace wivrn_macos
