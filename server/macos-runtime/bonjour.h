// Bonjour publisher for `_wivrn._tcp` (replaces Avahi on macOS).
// Same service type, port, and TXT keys the Linux server publishes,
// so unmodified clients discover us. dns_sd.h ships in the Xcode SDK
// (libSystem, no extra dependency).

#pragma once

#include <cstdint>
#include <string>

namespace wivrn_macos
{

struct BonjourConfig
{
	std::string name = "WiVRn";
	uint16_t port = 9757;
	uint64_t protocol_version = 0;
	std::string version = "0.1.0";
	uint64_t cookie = 0;
};

class BonjourPublisher
{
public:
	BonjourPublisher() = default;
	~BonjourPublisher();

	BonjourPublisher(const BonjourPublisher &) = delete;
	BonjourPublisher & operator=(const BonjourPublisher &) = delete;

	bool start(const BonjourConfig & config);
	void stop();
	bool running() const;

private:
	void * ref_ = nullptr; // DNSServiceRef (pimpl keeps dns_sd.h local)
	bool running_ = false;
};

} // namespace wivrn_macos
