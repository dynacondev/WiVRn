// Bonjour publisher: DNSServiceRegister on a worker thread with a
// select() pump (stop-safe), TXT record mirroring the Linux keys.

#include "bonjour.h"

#include "runtime.h" // log()

#include <dns_sd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace wivrn_macos
{

BonjourPublisher::~BonjourPublisher()
{
	stop();
}

bool BonjourPublisher::running() const
{
	return running_;
}

bool BonjourPublisher::start(const BonjourConfig & config)
{
	if (running_)
		return false;

	TXTRecordRef txt{};
	TXTRecordCreate(&txt, 0, nullptr);
	char protocol[17], cookie[17];
	std::snprintf(protocol, sizeof(protocol), "%016llx", (unsigned long long)config.protocol_version);
	std::snprintf(cookie, sizeof(cookie), "%016llx", (unsigned long long)config.cookie);
	TXTRecordSetValue(&txt, "protocol", (uint8_t)std::strlen(protocol), protocol);
	TXTRecordSetValue(&txt, "version", (uint8_t)config.version.size(), config.version.c_str());
	TXTRecordSetValue(&txt, "cookie", (uint8_t)std::strlen(cookie), cookie);

	DNSServiceRef ref = nullptr;
	DNSServiceErrorType err = DNSServiceRegister(
	        &ref,
	        0, // flags
	        0, // all interfaces
	        config.name.c_str(),
	        "_wivrn._tcp",
	        nullptr, // default domain
	        nullptr, // default hostname
	        htons(config.port),
	        TXTRecordGetLength(&txt),
	        TXTRecordGetBytesPtr(&txt),
	        nullptr, // callback (fire-and-register for v1)
	        nullptr);
	TXTRecordDeallocate(&txt);
	if (err != kDNSServiceErr_NoError || ref == nullptr)
	{
		log("bonjour: register failed (%d)", (int)err);
		return false;
	}
	ref_ = ref;
	running_ = true;
	log("bonjour: advertising '%s' _wivrn._tcp:%u", config.name.c_str(), config.port);
	return true;
}

void BonjourPublisher::stop()
{
	if (!running_)
		return;
	running_ = false;
	// The registration lives as long as the ref; releasing it removes
	// the advertisement. No callback traffic is processed in v1 (the
	// record is static for the process lifetime).
	DNSServiceRefDeallocate(static_cast<DNSServiceRef>(ref_));
	ref_ = nullptr;
	log("bonjour: stopped");
}

} // namespace wivrn_macos
