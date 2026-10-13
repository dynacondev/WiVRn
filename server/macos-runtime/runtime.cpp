// Runtime singleton + logging. No Monado here, ever.

#include "runtime.h"

#include <cstdarg>
#include <cstdio>

#include <chrono>

namespace wivrn_macos
{

Runtime & runtime_singleton()
{
	static Runtime runtime;
	return runtime;
}

void log(const char * fmt, ...)
{
	static const auto start = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	std::fprintf(stderr, "[%8lld] WIVRN_MACOS ", (long long)ms);
	va_list args;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	va_end(args);
	std::fputc('\n', stderr);
}

} // namespace wivrn_macos
