#ifndef EMULATOR_SRC_COMMON_LOCALTOGGLES_H_
#define EMULATOR_SRC_COMMON_LOCALTOGGLES_H_

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace Common {

// Local only. KYTY_LOCAL_DISABLE is a comma-separated list of local performance changes to turn
// off at run time, so a regression can be bisected with a relaunch instead of a rebuild (a rebuild
// also resets the pipeline cache). Names: retired, bdaskip, linecache, denseslots, cleanread,
// matreuse, srtcompile, buffersync, dispatchflush, pageclean. (dispatchargs moved to
// KYTY_LOCAL_ENABLE below, because it is now off by default.)
// The value is read once and printed, so the log records what a run actually had.
inline bool LocalFeatureDisabled(std::string_view name) {
	static const std::string list = [] {
		const char* value  = std::getenv("KYTY_LOCAL_DISABLE");
		std::string result = value != nullptr ? value : "";
		std::printf("KYTY_LOCAL_DISABLE=%s\n", result.empty() ? "(none)" : result.c_str());
		std::fflush(stdout);
		return result;
	}();
	std::string_view rest = list;
	while (!rest.empty()) {
		const auto comma = rest.find(',');
		if (rest.substr(0, comma) == name) {
			return true;
		}
		if (comma == std::string_view::npos) {
			break;
		}
		rest.remove_prefix(comma + 1);
	}
	return false;
}

// Local only. KYTY_LOCAL_ENABLE is the same kind of list for changes that are off by default
// because they are not yet reliable. Names: dispatchargs (skip the CPU read of indirect dispatch
// arguments; it loses the GPU device on some loads, see the comment in graphicsRun.cpp).
inline bool LocalFeatureEnabled(std::string_view name) {
	static const std::string list = [] {
		const char* value  = std::getenv("KYTY_LOCAL_ENABLE");
		std::string result = value != nullptr ? value : "";
		std::printf("KYTY_LOCAL_ENABLE=%s\n", result.empty() ? "(none)" : result.c_str());
		std::fflush(stdout);
		return result;
	}();
	std::string_view rest = list;
	while (!rest.empty()) {
		const auto comma = rest.find(',');
		if (rest.substr(0, comma) == name) {
			return true;
		}
		if (comma == std::string_view::npos) {
			break;
		}
		rest.remove_prefix(comma + 1);
	}
	return false;
}

} // namespace Common

#endif // EMULATOR_SRC_COMMON_LOCALTOGGLES_H_
