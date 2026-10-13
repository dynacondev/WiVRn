// Injectable tracking source. Phase 3 ships the synthetic source only
// (deterministic, time-based, no headset needed); Phase 5 adds the
// headset-packet feed behind the same interface. No Monado here, ever.

#pragma once

#include "runtime.h"

#include <memory>

namespace wivrn_macos
{

class TrackingSource
{
public:
	virtual ~TrackingSource() = default;
	virtual TrackingSample latest() = 0;
};

// Swap the active source (Phase 5 network feed). Thread-safe.
void set_tracking_source(std::shared_ptr<TrackingSource> source);

// Latest sample from the active source (synthetic until Phase 5).
TrackingSample current_sample();

} // namespace wivrn_macos
