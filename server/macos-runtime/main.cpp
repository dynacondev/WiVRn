// wivrn-server-macos launcher (Phase 5d): resolves the runtime manifest
// and spawns an OpenXR application with XR_RUNTIME_JSON set, so the app
// loads our runtime (which owns networking/streaming in-process).
// The manifest lives next to this executable after install; --runtime
// overrides, XR_RUNTIME_JSON passes through when already set.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char ** environ;

namespace
{

void usage(const char * prog)
{
	std::printf(
	        "usage: %s [--port PORT] [--runtime PATH] -- APP [ARGS...]\n"
	        "\n"
	        "  --port PORT     headset TCP/UDP port (default 9757)\n"
	        "  --runtime PATH  runtime dylib (default: XR_RUNTIME_JSON,\n"
	        "                  else wivrn-macos-runtime.dylib next to %s)\n"
	        "  --help          this text\n"
	        "\n"
	        "Spawns APP with XR_RUNTIME_JSON pointed at our manifest.\n"
	        "Headset discovery (Bonjour) and streaming run inside APP's\n"
	        "process via the loaded runtime.\n",
	        prog,
	        prog);
}

std::string sibling(const char * prog, const char * name)
{
	std::string path = prog;
	size_t slash = path.find_last_of('/');
	if (slash == std::string::npos)
		return name;
	return path.substr(0, slash + 1) + name;
}

} // namespace

int main(int argc, char ** argv)
{
	const char * runtime = std::getenv("XR_RUNTIME_JSON");
	const char * port = "9757";
	int app_at = -1;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
		{
			usage(argv[0]);
			return 0;
		}
		else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
			port = argv[++i];
		else if (std::strcmp(argv[i], "--runtime") == 0 && i + 1 < argc)
			runtime = argv[++i];
		else if (std::strcmp(argv[i], "--") == 0)
		{
			app_at = i + 1;
			break;
		}
		else
		{
			std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}
	if (app_at < 0 || app_at >= argc)
	{
		usage(argv[0]);
		return 2;
	}
	// Port travels via env (the runtime reads it at streamer start).
	::setenv("WIVRN_PORT", port, 1);
	// Manifest: explicit flag, existing env, or sibling of this binary.
	std::string manifest_storage;
	const char * manifest = runtime;
	if (manifest == nullptr)
	{
		manifest_storage = sibling(argv[0], "wivrn-macos.json");
		manifest = manifest_storage.c_str();
	}
	::setenv("XR_RUNTIME_JSON", manifest, 1);
	// Exactly one MoltenVK in the child: macOS hosts commonly ship two
	// (Homebrew + LunarG), and mapping both prints duplicate-class
	// warnings and leaves driver selection to loader order. Pin Homebrew's
	// (matches our loader/headers era) unless overridden.
	if (::getenv("VK_ICD_FILENAMES") == nullptr &&
	    ::access("/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json", R_OK) == 0)
		::setenv("VK_ICD_FILENAMES", "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json", 1);
	std::printf("wivrn-server-macos: launching %s (runtime %s, port %s)\n", argv[app_at], manifest, port);

	pid_t pid = 0;
	std::vector<char *> args(argv + app_at, argv + argc);
	args.push_back(nullptr);
	int status = ::posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ);
	if (status != 0)
	{
		std::fprintf(stderr, "spawn failed: %s\n", std::strerror(status));
		return 1;
	}
	int wstatus = 0;
	while (::waitpid(pid, &wstatus, 0) < 0)
	{
	}
	if (WIFEXITED(wstatus))
		return WEXITSTATUS(wstatus);
	return 128;
}
