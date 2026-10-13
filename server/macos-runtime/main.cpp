// macOS bespoke server v1 MVP — scaffold entry point.
//
// Phase 0: prove target wiring only. Real runtime (loader negotiation,
// session, swapchains, encode, network, Bonjour) lands in Phases 2-5.
// This file must never include Monado headers (xrt_*/comp_*/ipc_*/u_*);
// see ROADMAP.md "macOS Server v1 MVP" for the isolation rule.

#include <cstdio>

int main(int, char **)
{
	std::puts("wivrn-server-macos: scaffold (Phase 0) — runtime lands in Phases 2-5, see ROADMAP.md");
	return 0;
}
