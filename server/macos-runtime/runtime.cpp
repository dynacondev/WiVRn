// Runtime singleton + logging. No Monado here, ever.

#include "runtime.h"

#include <cstdarg>
#include <cstdio>

namespace wivrn_macos
{

Runtime & runtime_singleton()
{
	static Runtime runtime;
	return runtime;
}

void log(const char * fmt, ...)
{
	std::fputs("WIVRN_MACOS ", stderr);
	va_list args;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	va_end(args);
	std::fputc('\n', stderr);
}

} // namespace wivrn_macos
